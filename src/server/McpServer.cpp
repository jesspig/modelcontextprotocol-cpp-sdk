#include <mcp/JsonValue.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/RequestState.hpp>
#include <mcp/server/McpTaskStore.hpp>
#include <mcp/Content.hpp>
#include <mcp/McpError.hpp>
#include <mcp/McpVersion.hpp>
#include <mcp/Transport.hpp>
#include <mcp/Log.hpp>
#include <mcp/ext/Apps.hpp>
#include <mcp/ext/ServerCard.hpp>
#include <detail/JsonFields.hpp>
#include <detail/JsonSchemaValidator.hpp>
#include <detail/UriTemplate.hpp>
#include <mcp/detail/McpParamAnnotations.hpp>

#include <array>
#include <condition_variable>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <thread>
#include <set>
#include <algorithm>
#include <ctime>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef SendMessage
#undef GetMessage
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/time.h>
#endif
#endif

#if defined(MCP_HAVE_OPENSSL)
#include <openssl/rand.h>
#endif

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#if defined(_MSC_VER)
#pragma warning(disable : 4996)
#endif

namespace mcp {

namespace {
    constexpr size_t kDefaultPageSize = 100;

    constexpr std::chrono::seconds kElicitTimeout(600);
    constexpr std::chrono::seconds kNoWait(0);

    constexpr int64_t kMinCacheTtlMs = 0;
    constexpr std::string_view kModernDefaultCacheScope = "private";
    constexpr std::string_view kPublicCacheScope = "public";

    bool HasCacheHint(const CacheHint& hint) {
        return hint.ttl_ms.has_value() || hint.cache_scope.has_value();
    }

    void StripCacheWireFields(JsonValue& result) {
        auto& object = result.GetObject();
        object.erase(std::string(detail::kCacheHint));
        object.erase(std::string(detail::kTTLMs));
        object.erase(std::string(detail::kCacheScope));
    }

    void ValidateModernCacheHint(const CacheHint& hint, std::string_view method) {
        std::string problem;
        const auto append = [&problem](std::string text) {
            if (!problem.empty()) problem += "; ";
            problem += std::move(text);
        };
        if (!hint.ttl_ms) {
            append("missing ttlMs");
        } else if (*hint.ttl_ms < kMinCacheTtlMs) {
            append("ttlMs must be >= 0, got " + std::to_string(*hint.ttl_ms));
        }
        if (!hint.cache_scope) {
            append("missing cacheScope");
        } else if (*hint.cache_scope != kPublicCacheScope &&
                   *hint.cache_scope != kModernDefaultCacheScope) {
            append("cacheScope must be \"public\" or \"private\", got \"" +
                   *hint.cache_scope + "\"");
        }
        if (!problem.empty()) {
            throw McpError(McpErrorCode::InvalidParams,
                "invalid cache hint for " + std::string(method) + ": " + problem);
        }
    }

    template <typename Result, typename SerializeFn>
    JsonValue ApplyCacheHintAndSerialize(Result& result, const CacheHint& hint,
                                         SerializeFn&& serialize) {
        if (HasCacheHint(hint)) result.cache_hint = hint;
        return serialize(result);
    }

    constexpr std::string_view kUrlElicitationInputKey = "elicitation";
    constexpr std::string_view kFormElicitationInputKey = "elicitation";

    struct ElicitationInputRequired : std::runtime_error {
        InputRequests input_requests;
        explicit ElicitationInputRequired(InputRequests requests)
            : std::runtime_error("elicitation requires an MRTR input round-trip")
            , input_requests(std::move(requests)) {}
    };

    constexpr std::string_view kDefaultLoggerName = "mcp-server";

    constexpr std::string_view kMcpNameHeader = "Mcp-Name";
    constexpr std::string_view kMcpNameHeaderLower = "mcp-name";

    size_t ParseCursor(const std::optional<std::string>& cursor) {
        if (!cursor || cursor->empty()) return 0;
        try {
            return std::stoul(*cursor);
        } catch (const std::exception&) {
            throw McpError(McpErrorCode::InvalidParams, "invalid cursor: " + *cursor);
        }
    }

    std::string MakeNextCursor(size_t next_index) {
        return std::to_string(next_index);
    }

    constexpr std::string_view kSkillFileSuffix = "/SKILL.md";
    constexpr std::string_view kDirectoryMimeType = "inode/directory";
    constexpr std::string_view kSchemeSeparator = "://";

    bool IsSkillEntryUri(std::string_view uri) {
        if (uri.size() <= kSkillFileSuffix.size()) return false;
        if (uri.compare(uri.size() - kSkillFileSuffix.size(),
                kSkillFileSuffix.size(), kSkillFileSuffix) != 0) return false;
        auto scheme_end = uri.find(kSchemeSeparator);
        if (scheme_end == std::string_view::npos || scheme_end == 0) return false;
        std::string_view after = uri.substr(scheme_end + kSchemeSeparator.size());
        auto slash = after.find('/');
        if (slash == std::string_view::npos || slash == 0) return false;
        return after.size() > slash + 1;
    }

    std::string SkillRootFromEntryUri(const std::string& uri) {
        return uri.substr(0, uri.size() - kSkillFileSuffix.size());
    }

    std::string NormalizeDirectoryUri(const std::string& uri) {
        std::string out = uri;
        while (out.size() > 1 && out.back() == '/') out.pop_back();
        return out;
    }

    struct SkillDirectoryView {
        bool is_directory{false};
        std::vector<DirectoryChild> children;
    };

    SkillDirectoryView CollectDirectoryChildren(
        const std::vector<Skill>& skills, const std::string& dir)
    {
        SkillDirectoryView view;
        std::set<std::string> seen;
        for (const auto& skill : skills) {
            if (!IsSkillEntryUri(skill.uri)) continue;
            if (dir == SkillRootFromEntryUri(skill.uri)) view.is_directory = true;
            auto* manifest = std::get_if<std::vector<SkillResource>>(&skill.resources);
            if (!manifest) continue;
            for (const auto& res : *manifest) {
                const std::string& uri = res.uri;
                if (uri == dir) continue;
                if (uri.size() <= dir.size() + 1) continue;
                if (uri.compare(0, dir.size(), dir) != 0 || uri[dir.size()] != '/') continue;
                view.is_directory = true;
                std::string rest = uri.substr(dir.size() + 1);
                auto slash = rest.find('/');
                DirectoryChild child;
                if (slash == std::string::npos) {
                    if (rest.empty()) continue;
                    child.uri = uri;
                    child.name = rest;
                } else {
                    std::string seg = rest.substr(0, slash);
                    if (seg.empty()) continue;
                    child.uri = dir + "/" + seg;
                    child.name = seg;
                    child.mime_type = std::string(kDirectoryMimeType);
                }
                if (seen.insert(child.uri).second) view.children.push_back(std::move(child));
            }
        }
        std::sort(view.children.begin(), view.children.end(),
            [](const DirectoryChild& a, const DirectoryChild& b) { return a.uri < b.uri; });
        return view;
    }

    template <typename Entry, typename Fn, typename IncludeFn = std::nullptr_t>
    bool PaginateEntries(
        const std::vector<Entry>& entries,
        size_t cursor_val,
        size_t page_size,
        size_t& next_index,
        Fn&& emit,
        IncludeFn&& should_include = nullptr)
    {
        size_t index = 0;
        size_t sent = 0;
        for (const auto& entry : entries) {
            if constexpr (std::is_invocable_v<IncludeFn, const Entry&>) {
                if (!should_include(entry)) continue;
            }
            if (index++ < cursor_val) continue;
            if (sent >= page_size) {
                next_index = index - 1;
                return true;
            }
            emit(entry);
            sent++;
        }
        return false;
    }

    bool IsTerminalTaskStatus(TaskStatus status) {
        return status == TaskStatus::Completed
            || status == TaskStatus::Failed
            || status == TaskStatus::Cancelled;
    }

    std::string TaskStatusToWireString(TaskStatus status) {
        switch (status) {
            case TaskStatus::Working:
            case TaskStatus::Pending:
                return "working";
            case TaskStatus::InputRequired:
                return "input_required";
            case TaskStatus::Completed:
                return "completed";
            case TaskStatus::Failed:
                return "failed";
            case TaskStatus::Cancelled:
                return "cancelled";
        }
        return "working";
    }

    std::vector<std::string> ComputePendingInputKeys(
        const std::optional<InputRequests>& parked,
        const std::optional<JsonValue>& accumulated)
    {
        std::vector<std::string> pending;
        if (!parked) return pending;
        for (const auto& entry : *parked) {
            bool answered = accumulated && accumulated->IsObject()
                && accumulated->Find(entry.first) != nullptr;
            if (!answered) pending.push_back(entry.first);
        }
        return pending;
    }

    InputRequests FilterInputRequests(
        const std::optional<InputRequests>& parked,
        const std::vector<std::string>& pending)
    {
        InputRequests pruned;
        if (!parked) return pruned;
        for (const auto& key : pending) {
            auto it = parked->find(key);
            if (it != parked->end()) pruned[key] = it->second;
        }
        return pruned;
    }

    bool HasAppsUriScheme(std::string_view uri) {
        return uri.size() >= ext::kAppsUriScheme.size() &&
            uri.compare(0, ext::kAppsUriScheme.size(), ext::kAppsUriScheme) == 0;
    }

    bool IsValidToolName(std::string_view name) {
        if (name.empty() || name.size() > 128) return false;
        return std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') ||
                   (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') ||
                   c == '.' || c == '_' || c == '-';
        });
    }

    void SendTaskNotification(
        McpSessionHandler& handler,
        std::string_view method,
        std::string_view task_id,
        TaskStatus status,
        bool modern,
        std::string_view session_id = {})
    {
        TaskStatusNotificationParams params;
        params.task_id = std::string(task_id);
        params.status = TaskStatusToWireString(status);
        JsonValue wire = SerializeTaskStatusNotificationParams(params);
        if (modern) {
            handler.NotifySubscribers(method, std::move(wire), std::nullopt,
                                      session_id);
            return;
        }
        handler.SendNotification(method, std::move(wire), session_id);
    }

    std::string MakeIso8601Now() {
        std::time_t now = std::time(nullptr);
        std::tm tm_buf{};
#ifdef _WIN32
        gmtime_s(&tm_buf, &now);
#else
        gmtime_r(&now, &tm_buf);
#endif
        std::array<char, 32> buf;
        std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
        return std::string(buf.data());
    }

    JsonValue MakeGetTaskResultJson(
        const TaskState& task, bool include_optional_fields, bool complete_result_type)
    {
        GetTaskResult r;
        r.task_id = task.task_id;
        r.status = TaskStatusToWireString(task.status);
        if (complete_result_type) r.task_result_type = detail::kCompleteValue;
        if (task.status == TaskStatus::Failed) {
            r.error = task.result;
        } else {
            r.result = task.result;
        }
        if (include_optional_fields) {
            r.error_message = task.error_message;
            r.input_required = task.input_required;
        }
        r.status_message = task.status_message;
        if (!task.created_at.empty()) r.created_at = task.created_at;
        r.last_updated_at = task.last_updated_at;
        r.ttl_ms = task.ttl_ms;
        r.poll_interval_ms = task.poll_interval_ms;
        r.input_requests = task.input_requests;
        return SerializeGetTaskResult(r);
    }

    std::optional<std::string> TryGetMcpName(const JsonRpcRequest& req) {
        if (!req.meta || !req.meta->IsObject()) return std::nullopt;
        if (auto* v = req.meta->Find(kMcpNameHeader); v && v->IsString())
            return v->GetString();
        if (auto* v = req.meta->Find(kMcpNameHeaderLower); v && v->IsString())
            return v->GetString();
        return std::nullopt;
    }

    constexpr int64_t kDefaultTaskTtlMs = 300000;

    bool ClientDeclaresTasksExtension(const ClientCapabilities& caps) {
        if (!caps.extensions) return false;
        return caps.extensions->find(std::string(kTasksExtensionId))
            != caps.extensions->end();
    }

    bool TasksExtensionDeclared(
        const std::shared_ptr<const ClientCapabilities>& stored,
        const std::optional<ClientCapabilities>& per_request)
    {
        if (stored && ClientDeclaresTasksExtension(*stored)) return true;
        if (per_request && ClientDeclaresTasksExtension(*per_request)) return true;
        return false;
    }

    bool SamplingDeclared(
        const std::shared_ptr<const ClientCapabilities>& stored,
        const std::optional<ClientCapabilities>& per_request)
    {
        if (stored && stored->sampling) return true;
        if (per_request && per_request->sampling) return true;
        return false;
    }

    McpError MissingTasksExtensionError() {
        JsonValue extension(JsonValue::object_tag);
        JsonValue extensions(JsonValue::object_tag);
        extensions[std::string(kTasksExtensionId)] = std::move(extension);
        JsonValue required(JsonValue::object_tag);
        required[detail::kExtensions] = std::move(extensions);
        JsonValue data(JsonValue::object_tag);
        data[detail::kRequiredCapabilities] = std::move(required);
        return McpError(McpErrorCode::MissingRequiredClientCapability,
            "Missing required client capability: io.modelcontextprotocol/tasks",
            std::move(data));
    }

    McpError MissingSamplingCapabilityError() {
        JsonValue sampling(JsonValue::object_tag);
        JsonValue required(JsonValue::object_tag);
        required[detail::kSampling] = std::move(sampling);
        JsonValue data(JsonValue::object_tag);
        data[detail::kRequiredCapabilities] = std::move(required);
        return McpError(McpErrorCode::MissingRequiredClientCapability,
            "Missing required client capability: sampling",
            std::move(data));
    }

    bool RequiresSamplingInput(const CallToolResult& result) {
        if (!result.input_required) return false;
        for (const auto& entry : result.input_required->input_requests) {
            if (entry.second.method == methods::kCreateMessage) return true;
        }
        return false;
    }

    JsonValue DecodeStatePayloadForMint(const std::optional<std::string>& state) {
        JsonValue fallback(JsonValue::object_tag);
        if (!state) return fallback;
        try {
            JsonValue parsed = JsonValue::Parse(*state);
            if (parsed.IsObject()) return parsed;
        } catch (...) {
        }
        auto decoded = detail::DecodeRequestStatePayload(*state);
        if (decoded && decoded->IsObject()) return *decoded;
        return fallback;
    }

    bool FillRandomBytes(uint8_t* buffer, size_t size) {
#if defined(MCP_HAVE_OPENSSL)
        return RAND_bytes(buffer, static_cast<int>(size)) == 1;
#else
        std::random_device rd;
        for (size_t i = 0; i < size; ++i) {
            buffer[i] = static_cast<uint8_t>(rd());
        }
        return true;
#endif
    }

    std::string MakeRandomHex(size_t byte_count) {
        std::vector<uint8_t> buffer(byte_count);
        if (!FillRandomBytes(buffer.data(), buffer.size())) {
            throw McpError(McpErrorCode::InternalError,
                "failed to generate secure random identifier");
        }
        static constexpr char kHexDigits[] = "0123456789abcdef";
        std::string out;
        out.reserve(byte_count * 2);
        for (auto byte : buffer) {
            out.push_back(kHexDigits[byte >> 4]);
            out.push_back(kHexDigits[byte & 0x0F]);
        }
        return out;
    }

    int64_t ServerNowMillis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    bool IsKnownMcpErrorCode(int64_t value) {
        static constexpr int64_t kKnownCodes[] = {
            static_cast<int64_t>(McpErrorCode::ParseError),
            static_cast<int64_t>(McpErrorCode::InvalidRequest),
            static_cast<int64_t>(McpErrorCode::MethodNotFound),
            static_cast<int64_t>(McpErrorCode::InvalidParams),
            static_cast<int64_t>(McpErrorCode::InternalError),
            static_cast<int64_t>(McpErrorCode::HeaderMismatch),
            static_cast<int64_t>(McpErrorCode::MissingRequiredClientCapability),
            static_cast<int64_t>(McpErrorCode::UnsupportedProtocolVersion),
            static_cast<int64_t>(McpErrorCode::UrlElicitationRequired),
            static_cast<int64_t>(McpErrorCode::ResourceNotFound),
            static_cast<int64_t>(McpErrorCode::ConnectionClosed),
            static_cast<int64_t>(McpErrorCode::RequestTimeout),
            static_cast<int64_t>(McpErrorCode::RequestCancelled),
            static_cast<int64_t>(McpErrorCode::ConnectionRefused),
            static_cast<int64_t>(McpErrorCode::TlsHandshakeFailed),
            static_cast<int64_t>(McpErrorCode::ProtocolViolation),
            static_cast<int64_t>(McpErrorCode::TaskNotFound),
            static_cast<int64_t>(McpErrorCode::HandlerError),
            static_cast<int64_t>(McpErrorCode::DeserializeFailed),
            static_cast<int64_t>(McpErrorCode::SessionExpired),
        };
        for (auto code : kKnownCodes) {
            if (code == value) return true;
        }
        return false;
    }

    std::optional<McpErrorCode> FindResponseErrorCode(const JsonValue& payload) {
        auto* code = payload.Find(detail::kCode);
        if (!code || !code->IsInt()) return std::nullopt;
        auto value = code->GetInt();
        if (!IsKnownMcpErrorCode(value)) return std::nullopt;
        return static_cast<McpErrorCode>(value);
    }
}

namespace {

CallToolResult MakeToolIsErrorResult(std::string text) {
    CallToolResult result;
    result.is_error = true;
    result.content.push_back(TextContent{"text", std::move(text)});
    return result;
}

constexpr int64_t kFileInputMaxBytes = 8 * 1024 * 1024;
constexpr int64_t kFileInputInlineMaxBytes = 256 * 1024;
constexpr std::size_t kMaxFileInputRoots = 64;

bool HasFileInputPrefix(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool IsFileInputRefObject(const JsonValue& node) {
    if (!node.IsObject()) return false;
    const auto& obj = node.GetObject();
    if (obj.empty() || obj.size() > 3) return false;
    auto uri_it = obj.find("uri");
    if (uri_it == obj.end() || !uri_it->second.IsString()) return false;
    for (const auto& entry : obj) {
        if (entry.first == "uri") continue;
        if (entry.first == "offset" || entry.first == "length") {
            if (!entry.second.IsInt()) return false;
            continue;
        }
        return false;
    }
    return true;
}

bool SliceFileInputRange(const std::string& full, int64_t offset, int64_t length,
    int64_t max_bytes, std::string& out, std::string& error)
{
    int64_t size = static_cast<int64_t>(full.size());
    if (offset < 0 || length < 0 || offset > size) {
        error = "file input range out of bounds";
        return false;
    }
    if (length > size - offset) {
        error = "file input range out of bounds";
        return false;
    }
    int64_t end = (length == 0) ? size : offset + length;
    if (end < offset || end > size) {
        error = "file input range out of bounds";
        return false;
    }
    if (end - offset > max_bytes) {
        error = "file input exceeds maximum size";
        return false;
    }
    out = full.substr(static_cast<size_t>(offset),
        static_cast<size_t>(end - offset));
    return true;
}

using FileInputPathParts = std::vector<std::string>;

std::string FileInputLowerAscii(std::string_view value) {
    std::string result(value);
    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return result;
}

std::string FileInputComparePart(std::string_view value) {
#if defined(_WIN32)
    return FileInputLowerAscii(value);
#else
    return std::string(value);
#endif
}

bool FileInputIsDrivePart(const std::string& part) {
    return part.size() == 2 &&
        ((part[0] >= 'A' && part[0] <= 'Z') ||
         (part[0] >= 'a' && part[0] <= 'z')) && part[1] == ':';
}

FileInputPathParts FileInputPathPartsFrom(const std::filesystem::path& path) {
    FileInputPathParts parts;
    for (const auto& component : path) {
        std::string part = component.string();
        if (part.empty() || part == "/" || part == "\\") continue;
#if defined(_WIN32)
        if (part == "\\\\?") continue;
        constexpr std::string_view prefix = "\\\\?\\";
        if (part.size() >= prefix.size() &&
            part.compare(0, prefix.size(), prefix) == 0) {
            part.erase(0, prefix.size());
            if (part.empty()) continue;
        }
#endif
        parts.push_back(std::move(part));
    }
    return parts;
}

bool FileInputCanonicalParts(
    const std::filesystem::path& path, FileInputPathParts& parts)
{
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) return false;
    parts = FileInputPathPartsFrom(canonical);
    return !parts.empty();
}

bool FileInputIsPrefix(
    const FileInputPathParts& prefix, const FileInputPathParts& parts)
{
    if (prefix.size() > parts.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (FileInputComparePart(prefix[i]) != FileInputComparePart(parts[i])) {
            return false;
        }
    }
    return true;
}

int FileInputHexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool FileInputDecodeHex(std::string_view raw, std::string& decoded) {
    decoded.clear();
    decoded.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '%') {
            decoded.push_back(raw[i]);
            continue;
        }
        if (i + 2 >= raw.size()) return false;
        const int high = FileInputHexDigit(raw[i + 1]);
        const int low = FileInputHexDigit(raw[i + 2]);
        if (high < 0 || low < 0) return false;
        decoded.push_back(static_cast<char>(high * 16 + low));
        i += 2;
    }
    return true;
}

bool FileInputIsReservedDeviceName(const std::string& part) {
    const std::size_t dot = part.find('.');
    const std::string base =
        FileInputLowerAscii(dot == std::string::npos ? part : part.substr(0, dot));
    if (base == "con" || base == "prn" || base == "aux" || base == "nul") {
        return true;
    }
    return base.size() == 4 && (base.compare(0, 3, "com") == 0 ||
        base.compare(0, 3, "lpt") == 0) && base[3] >= '1' && base[3] <= '9';
}

bool FileInputIsSensitivePart(const std::string& part) {
    static constexpr std::array<std::string_view, 34> kNames = {
        ".env", ".git", ".svn", ".hg", ".bzr", ".ssh", ".aws", ".azure",
        ".kube", ".docker", ".npmrc", ".pypirc", ".netrc", "_netrc",
        ".htpasswd", ".git-credentials", ".gitconfig", "credentials",
        "credentials.json", "token.json", ".token", "id_rsa", "id_ed25519",
        "id_ecdsa", "id_dsa", "secrets.json", ".git-credential-store",
        ".aws-credentials", ".kube-config", ".docker-config.json",
        "authorized_keys", "known_hosts", "shadow", "passwd"
    };
    static constexpr std::array<std::string_view, 9> kExtensions = {
        ".pem", ".key", ".pfx", ".p12", ".jks", ".keystore", ".truststore",
        ".kdbx", ".gpg"
    };
    const std::string lower = FileInputLowerAscii(part);
    for (auto name : kNames) {
        if (lower == name) return true;
    }
    if (lower.size() > 5 && lower.compare(0, 5, ".env.") == 0) return true;
    if (lower.size() > 4 &&
        lower.compare(lower.size() - 4, 4, ".env") == 0) return true;
    for (auto extension : kExtensions) {
        if (lower.size() > extension.size() &&
            lower.compare(lower.size() - extension.size(),
                extension.size(), extension) == 0) {
            return true;
        }
    }
    return false;
}

bool FileInputIsSensitive(const FileInputPathParts& parts) {
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (FileInputIsSensitivePart(parts[i])) return true;
        if (FileInputLowerAscii(parts[i]) == ".config" && i + 1 < parts.size() &&
            FileInputLowerAscii(parts[i + 1]) == "gcloud") {
            return true;
        }
    }
    return false;
}

bool FileInputParseUri(std::string_view uri, FileInputPathParts& parts) {
    constexpr std::string_view scheme = "file://";
    if (!HasFileInputPrefix(uri, scheme)) return false;
    const std::string_view rest = uri.substr(scheme.size());
    for (char c : rest) {
        if (c == '\0' || c == '?' || c == '#' || c == '\\') return false;
    }
    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos) return false;
    const std::string_view authority = rest.substr(0, slash);
    bool drive_authority = false;
    if (!authority.empty() && FileInputLowerAscii(authority) != "localhost") {
        if (!FileInputIsDrivePart(std::string(authority))) return false;
        drive_authority = true;
    }
    const std::string_view raw = rest.substr(slash);
    if (raw.empty() || raw[0] != '/' || (raw.size() > 1 && raw[1] == '/')) {
        return false;
    }
    if (!drive_authority && raw.size() > 2 &&
        ((raw[1] >= 'A' && raw[1] <= 'Z') ||
         (raw[1] >= 'a' && raw[1] <= 'z')) && raw[2] == ':') {
        if (raw.size() < 4 || raw[3] != '/') return false;
    }
    std::string decoded;
    if (drive_authority) {
        if (!FileInputDecodeHex(authority, decoded)) return false;
        std::string tail;
        if (!FileInputDecodeHex(raw.substr(1), tail)) return false;
        decoded += "/" + tail;
    } else if (!FileInputDecodeHex(raw.substr(1), decoded)) {
        return false;
    }
    if (decoded.empty() || decoded.back() == '/') return false;
    if (decoded.find_first_of(std::string("\0?#\\%", 4)) != std::string::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start < decoded.size()) {
        const std::size_t end = decoded.find('/', start);
        const std::size_t stop = end == std::string::npos ? decoded.size() : end;
        if (stop == start) return false;
        std::string part = decoded.substr(start, stop - start);
        if (part == "." || part == ".." ||
            (!FileInputIsDrivePart(part) && part.find(':') != std::string::npos) ||
            FileInputIsReservedDeviceName(part) ||
            FileInputIsSensitivePart(part)) {
            return false;
        }
        parts.push_back(std::move(part));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return !parts.empty();
}

std::filesystem::path FileInputBuildPath(const FileInputPathParts& parts) {
#if defined(_WIN32)
    if (!parts.empty() && FileInputIsDrivePart(parts[0])) {
        std::filesystem::path result(parts[0] + "\\");
        for (std::size_t i = 1; i < parts.size(); ++i) result /= parts[i];
        return result;
    }
    std::filesystem::path result("\\");
    for (const auto& part : parts) result /= part;
    return result;
#else
    std::filesystem::path result("/");
    for (const auto& part : parts) result /= part;
    return result;
#endif
}

#if defined(_WIN32)

class FileInputHandle {
public:
    explicit FileInputHandle(HANDLE handle = INVALID_HANDLE_VALUE)
        : handle_(handle) {}
    ~FileInputHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
    }
    FileInputHandle(const FileInputHandle&) = delete;
    FileInputHandle& operator=(const FileInputHandle&) = delete;
    HANDLE Get() const { return handle_; }
    void Reset(HANDLE handle) {
        if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
        handle_ = handle;
    }
    HANDLE Release() {
        HANDLE result = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return result;
    }
private:
    HANDLE handle_;
};

bool FileInputOpenWin(
    const std::filesystem::path& path, bool directory, FileInputHandle& handle)
{
    const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS |
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN;
    HANDLE raw = ::CreateFileW(path.wstring().c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, flags, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(raw, &info) ||
        ::GetFileType(raw) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (directory && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) ||
        (!directory && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) {
        ::CloseHandle(raw);
        return false;
    }
    handle.Reset(raw);
    return true;
}

bool FileInputHandlePathMatches(
    HANDLE handle, const FileInputPathParts& expected)
{
    wchar_t buffer[4096];
    const DWORD length = ::GetFinalPathNameByHandleW(handle, buffer, 4096,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length >= 4096) return false;
    const auto parts = FileInputPathPartsFrom(
        std::filesystem::path(std::wstring(buffer, length)));
    if (parts.size() != expected.size()) return false;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (FileInputComparePart(parts[i]) !=
            FileInputComparePart(expected[i])) {
            return false;
        }
    }
    return true;
}

bool FileInputRangeBounds(
    int64_t size, int64_t offset, int64_t length, int64_t max_bytes,
    int64_t& count, std::string& error)
{
    if (offset < 0 || length < 0 || offset > size || length > size - offset) {
        error = "file input range out of bounds";
        return false;
    }
    const int64_t end = length == 0 ? size : offset + length;
    if (end < offset || end > size) {
        error = "file input range out of bounds";
        return false;
    }
    if (end - offset > max_bytes) {
        error = "file input exceeds maximum size";
        return false;
    }
    count = end - offset;
    return true;
}

bool FileInputReadSecure(const std::filesystem::path& root,
    const FileInputPathParts& relative, const FileInputPathParts& canonical,
    int64_t max_bytes, int64_t offset, int64_t length,
    std::string& out, std::string& error)
{
    FileInputHandle directory;
    if (!FileInputOpenWin(root, true, directory)) {
        error = "file input open failed";
        return false;
    }
    std::filesystem::path current = root;
    for (std::size_t i = 0; i + 1 < relative.size(); ++i) {
        current /= relative[i];
        FileInputHandle next;
        if (!FileInputOpenWin(current, true, next)) {
            error = "file input path rejected";
            return false;
        }
        directory.Reset(next.Release());
    }
    current /= relative.back();
    FileInputHandle file;
    if (!FileInputOpenWin(current, false, file)) {
        error = "file input open failed";
        return false;
    }
    BY_HANDLE_FILE_INFORMATION before{};
    if (!::GetFileInformationByHandle(file.Get(), &before) ||
        before.nNumberOfLinks != 1 ||
        !FileInputHandlePathMatches(file.Get(), canonical)) {
        error = "file input path rejected";
        return false;
    }
    const int64_t size = (static_cast<int64_t>(before.nFileSizeHigh) << 32) |
        static_cast<int64_t>(before.nFileSizeLow);
    if (size < 0 || size > max_bytes) {
        error = "file input exceeds maximum size";
        return false;
    }
    int64_t count = 0;
    if (!FileInputRangeBounds(size, offset, length, max_bytes, count, error)) {
        return false;
    }
    LARGE_INTEGER position{};
    position.QuadPart = offset;
    if (!::SetFilePointerEx(file.Get(), position, nullptr, FILE_BEGIN)) {
        error = "file input seek failed";
        return false;
    }
    std::string buffer(static_cast<std::size_t>(count), '\0');
    std::size_t done = 0;
    while (done < static_cast<std::size_t>(count)) {
        const DWORD chunk = static_cast<DWORD>(std::min<int64_t>(
            count - static_cast<int64_t>(done),
            std::numeric_limits<DWORD>::max()));
        DWORD read = 0;
        if (!::ReadFile(file.Get(), buffer.data() + done, chunk, &read, nullptr) ||
            read == 0) {
            error = "file input changed during read";
            return false;
        }
        done += read;
    }
    BY_HANDLE_FILE_INFORMATION after{};
    if (!::GetFileInformationByHandle(file.Get(), &after) ||
        after.nFileSizeHigh != before.nFileSizeHigh ||
        after.nFileSizeLow != before.nFileSizeLow ||
        after.ftLastWriteTime.dwLowDateTime != before.ftLastWriteTime.dwLowDateTime ||
        after.ftLastWriteTime.dwHighDateTime != before.ftLastWriteTime.dwHighDateTime ||
        after.nFileIndexHigh != before.nFileIndexHigh ||
        after.nFileIndexLow != before.nFileIndexLow ||
        after.nNumberOfLinks != 1) {
        error = "file input changed during read";
        return false;
    }
    out = std::move(buffer);
    return true;
}

#else

class FileInputFd {
public:
    explicit FileInputFd(int fd = -1) : fd_(fd) {}
    ~FileInputFd() {
        if (fd_ >= 0) ::close(fd_);
    }
    FileInputFd(const FileInputFd&) = delete;
    FileInputFd& operator=(const FileInputFd&) = delete;
    int Get() const { return fd_; }
    int Release() {
        int result = fd_;
        fd_ = -1;
        return result;
    }
private:
    int fd_;
};

bool FileInputStatMatches(const struct stat& before, const struct stat& after) {
#if defined(__APPLE__)
    const auto& before_time = before.st_mtimespec;
    const auto& after_time = after.st_mtimespec;
#else
    const auto& before_time = before.st_mtim;
    const auto& after_time = after.st_mtim;
#endif
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
        before.st_size == after.st_size && before.st_nlink == after.st_nlink &&
        before_time.tv_sec == after_time.tv_sec &&
        before_time.tv_nsec == after_time.tv_nsec;
}

bool FileInputHandlePathMatches(int fd, const FileInputPathParts& expected) {
    char buffer[4096];
    std::string path;
#if defined(__APPLE__)
    if (::fcntl(fd, F_GETPATH, buffer) != -1) {
        path.assign(buffer);
    }
#else
    const std::string link = "/proc/self/fd/" + std::to_string(fd);
    const ssize_t length = ::readlink(link.c_str(), buffer, sizeof(buffer));
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(buffer)) {
        path.assign(buffer, static_cast<std::size_t>(length));
    }
#endif
    if (path.empty()) return false;
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(
        std::filesystem::path(path), ec);
    if (ec) return false;
    return FileInputPathPartsFrom(canonical) == expected;
}

bool FileInputRangeBounds(
    int64_t size, int64_t offset, int64_t length, int64_t max_bytes,
    int64_t& count, std::string& error)
{
    if (offset < 0 || length < 0 || offset > size || length > size - offset) {
        error = "file input range out of bounds";
        return false;
    }
    const int64_t end = length == 0 ? size : offset + length;
    if (end < offset || end > size) {
        error = "file input range out of bounds";
        return false;
    }
    if (end - offset > max_bytes) {
        error = "file input exceeds maximum size";
        return false;
    }
    count = end - offset;
    return true;
}

bool FileInputReadSecure(const std::filesystem::path& root,
    const FileInputPathParts& relative, const FileInputPathParts& canonical,
    int64_t max_bytes, int64_t offset, int64_t length,
    std::string& out, std::string& error)
{
    const int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    int current = ::open(root.c_str(), flags);
    if (current < 0) {
        error = "file input open failed";
        return false;
    }
    struct stat root_stat{};
    if (::fstat(current, &root_stat) != 0 || !S_ISDIR(root_stat.st_mode)) {
        ::close(current);
        error = "file input open failed";
        return false;
    }
    for (std::size_t i = 0; i + 1 < relative.size(); ++i) {
        const int next = ::openat(current, relative[i].c_str(), flags);
        if (next < 0) {
            ::close(current);
            error = "file input path rejected";
            return false;
        }
        ::close(current);
        current = next;
        struct stat directory_stat{};
        if (::fstat(current, &directory_stat) != 0 ||
            !S_ISDIR(directory_stat.st_mode)) {
            ::close(current);
            error = "file input path rejected";
            return false;
        }
    }
    const int raw_fd = ::openat(current, relative.back().c_str(),
        O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (current >= 0) ::close(current);
    if (raw_fd < 0) {
        error = "file input open failed";
        return false;
    }
    FileInputFd file(raw_fd);
    struct stat before{};
    if (::fstat(file.Get(), &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_nlink != 1 || before.st_size < 0) {
        error = "file input rejected";
        return false;
    }
    if (!FileInputHandlePathMatches(file.Get(), canonical)) {
        error = "file input path rejected";
        return false;
    }
    int64_t count = 0;
    if (!FileInputRangeBounds(static_cast<int64_t>(before.st_size), offset,
            length, max_bytes, count, error)) {
        return false;
    }
    std::string buffer(static_cast<std::size_t>(count), '\0');
    int64_t done = 0;
    while (done < count) {
        const ssize_t read = ::pread(file.Get(), buffer.data() + done,
            static_cast<std::size_t>(count - done),
            static_cast<off_t>(offset + done));
        if (read < 0) {
            if (errno == EINTR) continue;
            error = "file input read failed";
            return false;
        }
        if (read == 0) {
            error = "file input changed during read";
            return false;
        }
        done += read;
    }
    struct stat after{};
    if (::fstat(file.Get(), &after) != 0 || !FileInputStatMatches(before, after)) {
        error = "file input changed during read";
        return false;
    }
    out = std::move(buffer);
    return true;
}

#endif

class FileInputBudget {
public:
    explicit FileInputBudget(const ServerOptions::FileInputConfig& config)
        : max_refs_(config.max_refs), max_total_bytes_(config.max_total_bytes) {}

    bool ReserveRef() {
        if (refs_ >= max_refs_) return false;
        ++refs_;
        return true;
    }

    bool AddBytes(int64_t bytes) {
        if (bytes < 0 || bytes > max_total_bytes_ - total_bytes_) return false;
        total_bytes_ += bytes;
        return true;
    }

private:
    std::size_t max_refs_;
    int64_t max_total_bytes_;
    std::size_t refs_{0};
    int64_t total_bytes_{0};
};

struct FileInputResourceSnapshot {
    std::string uri_pattern;
    bool is_template{false};
    std::function<ReadResourceResult(const std::string&)> handler;
    std::function<ReadResourceResult(
        const std::string&, const std::map<std::string, std::string>&)> template_handler;
};

class FileInputResolver {
public:
    FileInputResolver(const ServerOptions::FileInputConfig& config,
        const std::vector<FileInputResourceSnapshot>& resources,
        FileInputBudget& budget)
        : config_(config), resources_(resources), budget_(budget)
    {
        if (!config_.allowed_roots.empty() &&
            config_.allowed_roots.size() <= kMaxFileInputRoots) {
            for (const auto& root_text : config_.allowed_roots) {
                FileInputPathParts root;
                if (!FileInputCanonicalParts(
                        std::filesystem::path(root_text), root) ||
                    FileInputIsSensitive(root)) {
                    continue;
                }
                roots_.push_back(std::move(root));
            }
        }
    }

    bool Resolve(const FileRef& ref, JsonValue& node, std::string& error) {
        if (HasFileInputPrefix(ref.uri, "https://") ||
            HasFileInputPrefix(ref.uri, "http://")) {
            return true;
        }
        if (HasFileInputPrefix(ref.uri, "resource://")) {
            return ResolveResource(ref, node, error);
        }
        if (HasFileInputPrefix(ref.uri, "file://")) {
            return ResolveFile(ref, node, error);
        }
        return true;
    }

private:
    static void Inline(const FileRef& ref, JsonValue& node, std::string content) {
        JsonValue inlined(JsonValue::object_tag);
        inlined["uri"] = JsonValue(ref.uri);
        inlined["content"] = JsonValue(std::move(content));
        node = std::move(inlined);
    }

    bool ResolveResource(
        const FileRef& ref, JsonValue& node, std::string& error)
    {
        if (!budget_.ReserveRef()) {
            error = "file input reference budget exceeded";
            return false;
        }
        bool found = false;
        ReadResourceResult read;
        for (const auto& entry : resources_) {
            if (entry.is_template || entry.uri_pattern != ref.uri) continue;
            found = true;
            try {
                read = entry.handler(ref.uri);
            } catch (...) {
                error = "file input resource read failed: " + ref.uri;
                return false;
            }
            break;
        }
        if (!found) {
            for (const auto& entry : resources_) {
                if (!entry.is_template) continue;
                std::string template_error;
                auto tmpl = detail::UriTemplate::Parse(
                    entry.uri_pattern, template_error);
                if (!tmpl) continue;
                auto variables = tmpl->Match(ref.uri);
                if (!variables) continue;
                found = true;
                try {
                    read = entry.template_handler(ref.uri, *variables);
                } catch (...) {
                    error = "file input resource read failed: " + ref.uri;
                    return false;
                }
                break;
            }
        }
        if (!found) {
            error = "file input resource not found: " + ref.uri;
            return false;
        }
        if (read.input_required) {
            error = "file input resource requires input: " + ref.uri;
            return false;
        }
        std::string full;
        for (const auto& content : read.contents) {
            if (auto* text = std::get_if<TextResourceContents>(&content)) {
                full += text->text;
            } else if (auto* blob = std::get_if<BlobResourceContents>(&content)) {
                full += blob->blob;
            }
        }
        if (!budget_.AddBytes(static_cast<int64_t>(full.size()))) {
            error = "file input byte budget exceeded";
            return false;
        }
        std::string slice;
        if (!SliceFileInputRange(full, ref.offset, ref.length,
                config_.max_file_bytes, slice, error)) {
            return false;
        }
        if (static_cast<int64_t>(slice.size()) <= config_.max_inline_bytes) {
            Inline(ref, node, std::move(slice));
        }
        return true;
    }

    bool ResolveFile(const FileRef& ref, JsonValue& node, std::string& error) {
        if (roots_.empty()) {
            error = "file input has no allowed roots";
            return false;
        }
        if (!budget_.ReserveRef()) {
            error = "file input reference budget exceeded";
            return false;
        }
        FileInputPathParts relative_full;
        if (!FileInputParseUri(ref.uri, relative_full)) {
            error = "file input not allowed: " + ref.uri;
            return false;
        }
        if (FileInputIsSensitive(relative_full)) {
            error = "file input sensitive path rejected: " + ref.uri;
            return false;
        }
        FileInputPathParts canonical;
        if (!FileInputCanonicalParts(
                FileInputBuildPath(relative_full), canonical) ||
            FileInputIsSensitive(canonical)) {
            error = "file input not allowed: " + ref.uri;
            return false;
        }
        const FileInputPathParts* selected = nullptr;
        for (const auto& root : roots_) {
            if (FileInputIsPrefix(root, relative_full) &&
                FileInputIsPrefix(root, canonical)) {
                selected = &root;
                break;
            }
        }
        if (!selected) {
            error = "file input not allowed: " + ref.uri;
            return false;
        }
        const FileInputPathParts relative(
            relative_full.begin() + static_cast<std::ptrdiff_t>(selected->size()),
            relative_full.end());
        if (relative.empty()) {
            error = "file input not allowed: " + ref.uri;
            return false;
        }
        std::string content;
        if (!FileInputReadSecure(FileInputBuildPath(*selected), relative,
                canonical, config_.max_file_bytes, ref.offset, ref.length,
                content, error)) {
            return false;
        }
        if (!budget_.AddBytes(static_cast<int64_t>(content.size()))) {
            error = "file input byte budget exceeded";
            return false;
        }
        if (static_cast<int64_t>(content.size()) <= config_.max_inline_bytes) {
            Inline(ref, node, std::move(content));
        }
        return true;
    }

    const ServerOptions::FileInputConfig& config_;
    const std::vector<FileInputResourceSnapshot>& resources_;
    FileInputBudget& budget_;
    std::vector<FileInputPathParts> roots_;
};

std::string ToolExecutionFailureMessage(const std::string& name) {
    return std::string("Error executing tool ") + name;
}

bool ValidateToolCallArguments(
    const Tool& tool_def,
    const std::optional<JsonValue>& args,
    std::string& error_out)
{
    JsonValue instance(JsonValue::object_tag);
    if (args) instance = *args;
    return detail::ValidateJsonSchema(instance, tool_def.input_schema, error_out);
}

std::size_t ToolInputElementCount(const std::optional<JsonValue>& args) {
    if (!args) return 0;
    if (args->IsObject()) return args->GetObject().size();
    if (args->IsArray()) return args->GetArray().size();
    return 1;
}

std::optional<LoggingLevel> ParseRequestLogLevel(const JsonRpcRequest& req) {
    if (!req.meta || !req.meta->IsObject()) return std::nullopt;
    auto* lv = req.meta->Find(detail::kMetaLogLevelKey);
    if (!lv) return std::nullopt;
    if (lv->IsInt()) return static_cast<LoggingLevel>(lv->GetInt());
    if (lv->IsString()) {
        try {
            return DeserializeLoggingLevel(*lv);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

}

static bool RequireInitialized(bool initialized, bool modern_era, std::promise<JsonValue>& p) {
    if (modern_era || initialized) return true;
    p.set_exception(std::make_exception_ptr(
        McpError(McpErrorCode::InvalidRequest, "Server not initialized")));
    return false;
}

CacheHint McpServer::ResolveCacheHint(std::string_view method,
                                      std::string_view session_id) const {
    const bool modern = handler_->IsJuly2026OrLater(session_id);
    std::function<std::optional<CacheHint>(std::string_view)> hook;
    {
        std::lock_guard<std::mutex> lock(cache_hook_mutex_);
        hook = cache_hint_hook_;
    }
    if (hook) {
        if (auto resolved = hook(method)) {
            if (modern) ValidateModernCacheHint(*resolved, method);
            return *resolved;
        }
    }
    if (options_.cache_hints) {
        auto it = options_.cache_hints->find(method);
        if (it != options_.cache_hints->end()) {
            if (modern) ValidateModernCacheHint(it->second, method);
            return it->second;
        }
    }
    if (modern) {
        CacheHint hint;
        hint.ttl_ms = kMinCacheTtlMs;
        hint.cache_scope = std::string(kModernDefaultCacheScope);
        return hint;
    }
    return {};
}

std::vector<std::string> McpServer::EffectiveSupportedVersions() const {
    if (options_.supported_protocol_versions &&
        !options_.supported_protocol_versions->empty()) {
        return *options_.supported_protocol_versions;
    }
    std::vector<std::string> versions;
    versions.reserve(std::size(kProtocolVersions));
    for (auto v : kProtocolVersions) versions.emplace_back(v);
    return versions;
}

Implementation McpServer::ResolveServerInfo() const {
    if (options_.server_info) return *options_.server_info;
    return Implementation{"mcp-server", std::string(kSdkVersion), {}};
}

void McpServer::SetCacheable(
    std::function<std::optional<CacheHint>(std::string_view method)> resolver) {
    std::lock_guard<std::mutex> lock(cache_hook_mutex_);
    cache_hint_hook_ = std::move(resolver);
}

std::unique_ptr<McpServer> McpServer::Create(
    std::shared_ptr<ITransport> transport,
    const ServerOptions& options)
{
    return std::unique_ptr<McpServer>(
        new McpServer(std::move(transport), options));
}

McpServer::McpServer(
    std::shared_ptr<ITransport> transport,
    ServerOptions options)
    : transport_(std::move(transport))
    , options_(std::move(options))
    , session_id_(MakeRandomHex(8))
{
    SetEtagCacheEnabled(options_.enable_etag_cache);
    auto codec = MakeWireCodec(
        options_.protocol_version.value_or(std::string(kLatestProtocolVersion)));
    handler_ = std::make_shared<McpSessionHandler>(
        transport_, std::move(codec),
        options_.incoming_filters,
        options_.outgoing_filters);
    handler_->SetServerInfo(ResolveServerInfo());

    is_stateless_ = transport_->IsStateless();

    WireHandlers();

    DeriveCapabilities();

    handler_->SetSupportedProtocolVersions(EffectiveSupportedVersions());
    if (options_.cache_hint_resolver) {
        std::lock_guard<std::mutex> lock(cache_hook_mutex_);
        cache_hint_hook_ = options_.cache_hint_resolver;
    }

    if (options_.protocol_version) {
        handler_->SetNegotiatedProtocolVersion(*options_.protocol_version);
    }

    if (options_.request_state_verifier) {
        handler_->SetRequestStateVerifier(options_.request_state_verifier);
    } else if (options_.request_state_key) {
        auto key = *options_.request_state_key;
        auto ttl = options_.request_state_ttl;
        handler_->SetRequestStateVerifier(
            [key, ttl](std::string_view state) {
                JsonValue payload;
                return detail::VerifyRequestState(key, state, payload, ttl);
            });
    }

    if (options_.on_request || options_.on_method_called) {
        handler_->SetOnRequestCallback(
            [this](std::string_view method, const JsonRpcRequest& req) {
                if (options_.on_request) options_.on_request(method, req);
                if (options_.on_method_called) options_.on_method_called(method);
            });
    }
    if (options_.on_response) {
        handler_->SetOnResponseCallback(options_.on_response);
    }
    if (options_.on_error || options_.on_protocol_error) {
        handler_->SetOnErrorCallback(
            [this](const JsonRpcErrorResponse& err) {
                if (options_.on_error) options_.on_error(err);
                if (options_.on_protocol_error) options_.on_protocol_error(err.error.message);
            });
    }
    if (options_.on_notification) {
        handler_->SetOnNotificationCallback(options_.on_notification);
    }
    if (options_.span_handler) {
        handler_->SetSpanHandler(std::move(options_.span_handler));
    }
    if (options_.on_transport_close || options_.on_transport_error) {
        if (auto* tb = dynamic_cast<TransportBase*>(transport_.get())) {
            if (options_.on_transport_close)
                tb->SetOnClose(options_.on_transport_close);
            if (options_.on_transport_error)
                tb->SetOnError(options_.on_transport_error);
        }
    }

    transport_->Start();

    handler_->Start();

    running_ = true;
}

void McpServer::Run() {
    std::unique_lock<std::mutex> lock(run_mutex_);
    run_cv_.wait(lock, [this] { return !running_; });
}

void McpServer::Close() {
    {
        std::lock_guard<std::mutex> lock(pending_async_mutex_);
        for (auto& fut : pending_async_futures_) {
            fut.wait();
        }
        pending_async_futures_.clear();
    }
    handler_->Close();
    {
        std::lock_guard<std::mutex> lock(run_mutex_);
        running_ = false;
    }
    run_cv_.notify_one();
}

void McpServer::RegisterTool(std::shared_ptr<McpServerTool> tool) {
    const auto& t = tool->ProtocolTool();
    if (!IsValidToolName(t.name)) {
        throw McpError(McpErrorCode::InvalidParams,
            "invalid tool name: " + t.name);
    }
    auto header_annotations = detail::ParseToolParamAnnotations(t.input_schema);
    if (!header_annotations.IsValid()) {
        throw McpError(McpErrorCode::InvalidParams, header_annotations.invalid_reason);
    }
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        if (tools_.find(t.name) != tools_.end()) {
            throw McpError(McpErrorCode::InvalidParams,
                "tool already registered: " + t.name);
        }
        tools_[t.name] = std::move(tool);
        cached_tools_ = std::nullopt;
    }
    WireHandlers();
    DeriveCapabilities();
    if (IsRunning()) {
        SendListChangedNotification(notifications::kToolListChanged);
    }
}

void McpServer::RegisterResource(
    std::string_view name,
    std::string_view uri,
    const ResourceOptions& opts,
    std::function<ReadResourceResult(const std::string&)> handler)
{
    if (!handler) {
        throw McpError(McpErrorCode::InvalidParams,
            "resource handler must not be empty: " + std::string(name));
    }
    ResourceEntry entry;
    entry.name = std::string(name);
    entry.uri_pattern = std::string(uri);
    entry.is_template = false;
    entry.description = opts.description;
    entry.title = opts.title;
    entry.mime_type = opts.mime_type;
    entry.icons = opts.icons;
    entry.handler = std::move(handler);
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        for (const auto& existing : resources_) {
            if (existing.name == entry.name) {
                throw McpError(McpErrorCode::InvalidParams,
                    "resource already registered: " + entry.name);
            }
            if (existing.uri_pattern == entry.uri_pattern) {
                throw McpError(McpErrorCode::InvalidParams,
                    "resource uri already registered: " + entry.uri_pattern);
            }
        }
        resources_.push_back(std::move(entry));
    }
    WireHandlers();
    DeriveCapabilities();
    if (IsRunning()) {
        SendListChangedNotification(notifications::kResourceListChanged);
    }
}

void McpServer::RegisterResourceTemplate(
    std::string_view name,
    std::string_view uri_template,
    const ResourceOptions& opts,
    std::function<ReadResourceResult(
        const std::string&,
        const std::map<std::string, std::string>&)> handler)
{
    if (!handler) {
        throw McpError(McpErrorCode::InvalidParams,
            "resource template handler must not be empty: " + std::string(name));
    }
    std::string error;
    if (!detail::UriTemplate::Parse(uri_template, error)) {
        throw McpError(McpErrorCode::InvalidParams,
                       "invalid resource template '" + std::string(uri_template) +
                           "': " + error);
    }

    ResourceEntry entry;
    entry.name = std::string(name);
    entry.uri_pattern = std::string(uri_template);
    entry.is_template = true;
    entry.description = opts.description;
    entry.title = opts.title;
    entry.mime_type = opts.mime_type;
    entry.icons = opts.icons;
    entry.template_handler = std::move(handler);
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        for (const auto& existing : resources_) {
            if (existing.name == entry.name) {
                throw McpError(McpErrorCode::InvalidParams,
                    "resource already registered: " + entry.name);
            }
            if (existing.uri_pattern == entry.uri_pattern) {
                throw McpError(McpErrorCode::InvalidParams,
                    "resource uri already registered: " + entry.uri_pattern);
            }
        }
        resources_.push_back(std::move(entry));
    }
    WireHandlers();
    DeriveCapabilities();
    if (IsRunning()) {
        SendListChangedNotification(notifications::kResourceListChanged);
    }
}

void McpServer::RegisterPrompt(
    std::string_view name,
    const PromptOptions& opts,
    std::function<GetPromptResult(
        const std::string&,
        const std::optional<JsonValue>&)> handler)
{
    if (!handler) {
        throw McpError(McpErrorCode::InvalidParams,
            "prompt handler must not be empty: " + std::string(name));
    }
    PromptEntry entry;
    entry.name = std::string(name);
    entry.description = opts.description;
    entry.title = opts.title;
    entry.icons = opts.icons;
    entry.arguments = opts.arguments;
    entry.handler = std::move(handler);
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        for (const auto& existing : prompts_) {
            if (existing.name == entry.name) {
                throw McpError(McpErrorCode::InvalidParams,
                    "prompt already registered: " + entry.name);
            }
        }
        prompts_.push_back(std::move(entry));
    }
    WireHandlers();
    DeriveCapabilities();
    if (IsRunning()) {
        SendListChangedNotification(notifications::kPromptListChanged);
    }
}

void McpServer::RegisterSkill(Skill skill) {
    if (!IsSkillEntryUri(skill.uri)) {
        throw McpError(McpErrorCode::InvalidParams,
            "invalid skill uri: " + skill.uri);
    }
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        for (const auto& existing : skills_) {
            if (existing.uri == skill.uri) {
                throw McpError(McpErrorCode::InvalidParams,
                    "skill already registered: " + skill.uri);
            }
        }
        skills_.push_back(std::move(skill));
    }
    WireHandlers();
    DeriveCapabilities();
}

void McpServer::SetSkillsDirectoryRead(bool enabled) {
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        skills_directory_read_ = enabled;
    }
    DeriveCapabilities();
}

void McpServer::SendToolListChanged() {
    SendListChangedNotification(notifications::kToolListChanged);
}

void McpServer::SendResourceListChanged() {
    SendListChangedNotification(notifications::kResourceListChanged);
}

void McpServer::SendResourceUpdated(const std::string& uri) {
    JsonValue params(JsonValue::object_tag);
    params[detail::kUri] = JsonValue(uri);
    handler_->NotifySubscribers(notifications::kResourceUpdated, std::move(params), uri);
}

void McpServer::SendPromptListChanged() {
    SendListChangedNotification(notifications::kPromptListChanged);
}

void McpServer::SendListChangedNotification(std::string_view method) {
    if (handler_->IsJuly2026OrLater()) {
        handler_->NotifySubscribers(method, JsonValue(JsonValue::object_tag));
        return;
    }
    handler_->SendNotification(method, JsonValue(JsonValue::object_tag));
}

void McpServer::SendLoggingMessage(LoggingLevel level, std::string_view data) {
    if (handler_->IsJuly2026OrLater())
        return;
    std::optional<LoggingLevel> current_level;
    {
        std::lock_guard<std::mutex> lock(log_level_mutex_);
        current_level = current_log_level_;
    }
    if (current_level && static_cast<int>(level) < static_cast<int>(*current_level))
        return;
    EmitLoggingMessage(level, data);
}

void McpServer::SendLoggingMessage(LoggingLevel level, std::string_view data, std::optional<LoggingLevel> min_level) {
    if (min_level) {
        if (static_cast<int>(level) < static_cast<int>(*min_level))
            return;
        EmitLoggingMessage(level, data);
        return;
    }
    SendLoggingMessage(level, data);
}

void McpServer::EmitLoggingMessage(LoggingLevel level, std::string_view data) {
    JsonValue params(JsonValue::object_tag);
    params[detail::kLevel] = SerializeLoggingLevel(level);
    params[detail::kData] = JsonValue(std::string(data));
    params["logger"] = JsonValue(std::string(kDefaultLoggerName));
    if (IsModernProtocolVersion(handler_->NegotiatedProtocolVersion())) {
        JsonRpcNotification notif;
        notif.method = std::string(notifications::kMessage);
        notif.params = params;
        RequestMeta meta;
        meta.protocol_version = handler_->NegotiatedProtocolVersion();
        notif.meta = SerializeRequestMeta(meta);
        if (transport_->BufferRequestStreamNotification(JsonRpcMessage{std::move(notif)}))
            return;
        if (transport_->RequiresRequestScopedNotifications())
            return;
    }
    handler_->SendNotification(notifications::kMessage, std::move(params));
}

void McpServer::SendTaskStatus(std::string_view task_id, TaskStatus status) {
    std::string owner;
    std::optional<TaskState> stored;
    if (options_.task_store) {
        stored = options_.task_store->GetTask(std::string(task_id));
        if (stored) owner = stored->owner;
    }
    const bool modern =
        IsModernProtocolVersion(handler_->NegotiatedProtocolVersion(owner));
    if (modern && stored) {
        SendTaskDetail(*stored);
        return;
    }
    SendTaskNotification(*handler_, notifications::kTaskStatus, task_id, status,
        modern, owner);
}

std::string McpServer::MakeTaskId(std::string_view owner) const {
    return "task-" + std::string(owner) + "-" + MakeRandomHex(16);
}

bool McpServer::IsTaskOwnedBySession(const TaskState& task,
                                     std::string_view owner) const {
    return task.owner.empty() || task.owner == owner;
}

std::string McpServer::TaskOwnerFor(const JsonRpcRequest& req) const {
    if (auto principal = AuthenticatedPrincipalFor(req)) return *principal;
    const std::string session = req.session_id.empty() ? session_id_ : req.session_id;
    return session;
}

void McpServer::SetSessionPrincipal(std::string_view session_id,
                                    std::string_view principal) {
    std::lock_guard<std::mutex> lock(task_principal_mutex_);
    session_principals_[std::string(session_id)] = std::string(principal);
}

void McpServer::ClearSessionPrincipal(std::string_view session_id) {
    std::lock_guard<std::mutex> lock(task_principal_mutex_);
    session_principals_.erase(std::string(session_id));
}

void McpServer::SetDefaultPrincipal(std::string_view principal) {
    std::lock_guard<std::mutex> lock(task_principal_mutex_);
    default_principal_ = std::string(principal);
}

std::optional<std::string> McpServer::AuthenticatedPrincipalFor(
    std::string_view session_id) const {
    const std::string key = session_id.empty() ? session_id_ : std::string(session_id);
    std::lock_guard<std::mutex> lock(task_principal_mutex_);
    auto it = session_principals_.find(key);
    if (it != session_principals_.end() && !it->second.empty()) return it->second;
    if (!default_principal_.empty()) return default_principal_;
    return std::nullopt;
}

std::optional<std::string> McpServer::AuthenticatedPrincipalFor(
    const JsonRpcRequest& req) const
{
    if (req.request_context && !req.request_context->Principal().empty())
        return std::string(req.request_context->Principal());
    return AuthenticatedPrincipalFor(req.session_id);
}

std::vector<std::string> McpServer::AuthorizedScopesFor(
    const JsonRpcRequest& req) const
{
    if (!req.request_context) return {};
    return req.request_context->Scopes();
}

bool McpServer::TaskScopeAllows(const TaskState& task,
                                const JsonRpcRequest& req) const
{
    if (task.authorized_scopes.empty()) return true;
    const auto current = AuthorizedScopesFor(req);
    if (current.empty()) return true;
    for (const auto& recorded : task.authorized_scopes) {
        if (std::find(current.begin(), current.end(), recorded) != current.end())
            return true;
    }
    return false;
}

void McpServer::RequireTaskPrincipal(const JsonRpcRequest& req) const {
    if (!IsModernProtocolVersion(handler_->NegotiatedProtocolVersion(req.session_id))) return;
    if (AuthenticatedPrincipalFor(req)) return;
    MCP_LOG(Warning, "task access denied: no authenticated principal");
    throw McpError(McpErrorCode::TaskNotFound,
        "task access requires an authenticated principal");
}

void McpServer::EnforceTaskBudgets(std::string_view owner) {
    if (!options_.task_store) return;
    size_t count = 0;
    for (const auto& task : options_.task_store->GetAllTasks()) {
        if (task.owner == owner) ++count;
    }
    if (count >= kMaxTasksPerPrincipal) {
        MCP_LOG(Warning, "task creation denied: per-principal task limit reached");
        throw McpError(McpErrorCode::TaskNotFound, "task limit reached");
    }
}

void McpServer::SendTaskDetail(const TaskState& task, std::string_view session_id) {
    DetailedTask detail;
    detail.task_id = task.task_id;
    detail.status = TaskStatusToWireString(task.status);
    detail.created_at = task.created_at;
    detail.last_updated_at = task.last_updated_at;
    detail.ttl_ms = task.ttl_ms;
    if (task.status == TaskStatus::Failed) {
        detail.error = task.result;
    } else {
        detail.result = task.result;
    }
    detail.input_required = task.input_required;
    detail.input_requests = task.input_requests;
    JsonValue wire = SerializeDetailedTask(detail);
    if (wire.Dump().size() > kMaxTaskNotificationBytes) {
        MCP_LOG(Warning, "task notification dropped: payload exceeds budget");
        return;
    }
    handler_->NotifySubscribers(notifications::kTasks, std::move(wire),
                                std::nullopt, session_id);
}

std::shared_ptr<const ClientCapabilities> McpServer::ClientCapabilitiesFor(
    std::string_view session_id) const {
    std::lock_guard<std::mutex> lock(client_info_mutex_);
    if (session_id.empty()) return client_capabilities_;
    auto it = client_sessions_.find(std::string(session_id));
    if (it == client_sessions_.end()) return nullptr;
    return it->second.capabilities;
}

std::shared_ptr<const Implementation> McpServer::ClientInfoFor(
    std::string_view session_id) const {
    std::lock_guard<std::mutex> lock(client_info_mutex_);
    if (session_id.empty()) return client_info_;
    auto it = client_sessions_.find(std::string(session_id));
    if (it == client_sessions_.end()) return nullptr;
    return it->second.client_info;
}

bool McpServer::IsSessionInitialized(std::string_view session_id) const {
    if (session_id.empty()) return initialized_.load();
    std::lock_guard<std::mutex> lock(client_info_mutex_);
    auto it = client_sessions_.find(std::string(session_id));
    return it != client_sessions_.end() && it->second.initialized;
}

void McpServer::MarkSessionInitialized(std::string_view session_id) {
    initialized_.store(true);
    if (session_id.empty()) return;
    std::lock_guard<std::mutex> lock(client_info_mutex_);
    client_sessions_[std::string(session_id)].initialized = true;
}

std::string McpServer::EffectiveSessionId(std::string_view session_id) const {
    if (!session_id.empty()) return std::string(session_id);
    std::lock_guard<std::mutex> lock(client_info_mutex_);
    if (client_capabilities_) return std::string();
    if (client_sessions_.size() != 1) return std::string();
    auto it = client_sessions_.begin();
    if (it->first.empty()) return std::string();
    return it->first;
}

void McpServer::SweepExpiredTasks(const std::shared_ptr<IMcpTaskStore>& store) {
    if (!store) return;
    std::vector<std::string> expired;
    try {
        expired = store->GetExpiredTaskIds(ServerNowMillis());
    } catch (const std::exception& e) {
        MCP_LOG(Error, std::string("task store: failed to enumerate expired tasks: ") +
            e.what());
        return;
    }
    for (const auto& task_id : expired) {
        try {
            (void)store->RemoveTask(task_id);
        } catch (const std::exception& e) {
            MCP_LOG(Error, std::string("task store: failed to remove expired task ") +
                task_id + ": " + e.what());
        }
    }
}

void McpServer::SendProgress(const ProgressToken& token, double progress,
                             std::optional<double> total,
                             std::optional<std::string> message) {
    ProgressNotificationParams params;
    params.progress_token = token;
    params.progress = progress;
    params.total = total;
    params.message = std::move(message);
    JsonValue wire = SerializeProgressNotificationParams(params);
    if (IsModernProtocolVersion(handler_->NegotiatedProtocolVersion())) {
        JsonRpcNotification notif;
        notif.method = std::string(notifications::kProgress);
        notif.params = wire;
        RequestMeta meta;
        meta.protocol_version = handler_->NegotiatedProtocolVersion();
        notif.meta = SerializeRequestMeta(meta);
        if (transport_->BufferRequestStreamNotification(JsonRpcMessage{std::move(notif)}))
            return;
    }
    handler_->SendNotification(notifications::kProgress, std::move(wire));
}

void McpServer::RegisterTrigger(const std::string& id,
                                std::vector<std::string> types,
                                const std::string& webhook_url) {
    triggers_.Register(id, std::move(types), webhook_url);
}

bool McpServer::UnregisterTrigger(const std::string& id) {
    return triggers_.Unregister(id);
}

void McpServer::EmitTriggerEvent(const std::string& type, JsonValue data) {
    if (type.empty()) {
        throw McpError(McpErrorCode::InvalidParams, "trigger event type must not be empty");
    }
    if (!options_.enable_triggers_events) return;
    auto matches = triggers_.Match(type);
    if (matches.empty()) return;
    JsonValue params = ext::SerializeTriggerEvent(type, data);
    std::string body = params.Dump();
    handler_->NotifySubscribers(notifications::kTriggersEvent, std::move(params));
    for (const auto& match : matches) {
        if (match.second.webhook_url.empty()) continue;
        ext::PostTriggerWebhook(match.second.webhook_url, body);
    }
}

std::future<ElicitResult> McpServer::Elicit(
    const ElicitRequestParams& params, std::string_view session_id) {
    const std::string session = EffectiveSessionId(session_id);
    const std::string version = handler_->NegotiatedProtocolVersion(session);
    if (IsModernProtocolVersion(version)) {
        InputRequest request;
        request.method = std::string(methods::kElicit);
        request.params = SerializeElicitRequestParams(params, version);
        InputRequests requests;
        requests[std::string(kFormElicitationInputKey)] = std::move(request);
        throw ElicitationInputRequired(std::move(requests));
    }

    const bool url_mode = params.mode == "url";
    const auto caps = GetClientCapabilities(session);
    const ElicitationCapability* elicitation =
        (caps && caps->elicitation) ? &*caps->elicitation : nullptr;
    const bool declared = elicitation != nullptr &&
        (url_mode ? elicitation->url.has_value()
                  : (elicitation->form.has_value() || !elicitation->url.has_value()));
    if (!declared) {
        JsonValue missing(JsonValue::object_tag);
        missing[url_mode ? detail::kUrl : detail::kForm] = JsonValue(JsonValue::object_tag);
        JsonValue required(JsonValue::object_tag);
        required[detail::kElicitation] = std::move(missing);
        JsonValue data(JsonValue::object_tag);
        data[detail::kRequiredCapabilities] = std::move(required);
        throw McpError(McpErrorCode::MissingRequiredClientCapability,
            "Missing required client capabilities: elicitation", std::move(data));
    }

    RequestMeta meta;
    meta.protocol_version = version.empty()
        ? std::string(kLatestProtocolVersion) : version;

    auto future = handler_->SendRequest(
        methods::kElicit, SerializeElicitRequestParams(params), meta,
        options_.input_required_config
            ? options_.input_required_config->round_timeout
            : kElicitTimeout);

    auto result_future = std::async(std::launch::async, [future = std::move(future)]() mutable {
        auto jv = future.get();
        if (auto code = FindResponseErrorCode(jv)) {
            auto msg = jv.Find(detail::kMessage);
            throw McpError(
                *code,
                msg ? msg->GetString() : std::string("elicitation failed"));
        }
        return DeserializeElicitResult(jv);
    });

    return result_future;
}

std::future<ElicitResult> McpServer::ElicitUrl(
    const std::string& url,
    const std::string& message,
    std::chrono::seconds timeout,
    std::string_view session_id) {
    const std::string session = EffectiveSessionId(session_id);
    if (IsModernProtocolVersion(handler_->NegotiatedProtocolVersion(session))) {
        (void)timeout;
        ElicitRequestParams params;
        params.message = message;
        params.mode = "url";
        params.url = url;
        InputRequest request;
        request.method = std::string(methods::kElicit);
        request.params = SerializeElicitRequestParams(
            params, handler_->NegotiatedProtocolVersion(session));
        InputRequests requests;
        requests[std::string(kUrlElicitationInputKey)] = std::move(request);
        throw ElicitationInputRequired(std::move(requests));
    }
    const std::string elicitation_id =
        "elicit-" + std::to_string(next_elicitation_id_.fetch_add(1));

    ElicitRequestParams params;
    params.message = message;
    params.mode = "url";
    params.url = url;
    params.elicitation_id = elicitation_id;

    RequestMeta meta;
    auto vers = handler_->NegotiatedProtocolVersion(session);
    meta.protocol_version = vers.empty()
        ? std::string(kLatestProtocolVersion) : std::string(vers);

    auto pending = std::make_shared<PendingUrlElicitation>();
    auto result_future = pending->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(pending_url_elicitations_mutex_);
        pending_url_elicitations_.emplace(elicitation_id, pending);
    }

    std::future<JsonValue> request_future;
    try {
        request_future = handler_->SendRequest(
            methods::kElicit, SerializeElicitRequestParams(params), meta, timeout);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(pending_url_elicitations_mutex_);
            pending_url_elicitations_.erase(elicitation_id);
        }
        throw;
    }

    auto watchdog = std::async(std::launch::async,
        [this, elicitation_id, pending, timeout,
         request_future = std::move(request_future)]() mutable {
            const auto deadline = std::chrono::steady_clock::now() + timeout;

            std::exception_ptr error;
            if (request_future.wait_until(deadline) != std::future_status::ready) {
                error = std::make_exception_ptr(McpError(
                    McpErrorCode::RequestTimeout,
                    "url elicitation timed out (no response): " + elicitation_id));
            } else {
                auto jv = request_future.get();
                if (auto code = FindResponseErrorCode(jv)) {
                    auto msg = jv.Find(detail::kMessage);
                    error = std::make_exception_ptr(McpError(
                        *code,
                        msg ? msg->GetString() : std::string("elicitation failed")));
                }
            }
            if (error) {
                AbandonPendingUrlElicitation(elicitation_id, std::move(error));
                return;
            }

            std::unique_lock<std::mutex> lock(pending_url_elicitations_mutex_);
            if (!pending_url_elicitations_cv_.wait_until(lock, deadline,
                    [&] { return pending->completed; })) {
                lock.unlock();
                AbandonPendingUrlElicitation(elicitation_id,
                    std::make_exception_ptr(McpError(
                        McpErrorCode::RequestTimeout,
                        "url elicitation timed out (no complete notification): " + elicitation_id)));
            }
        });

    {
        std::lock_guard<std::mutex> lock(pending_async_mutex_);
        pending_async_futures_.push_back(watchdog.share());
        pending_async_futures_.erase(
            std::remove_if(pending_async_futures_.begin(), pending_async_futures_.end(),
                [](const auto& f) { return f.wait_for(kNoWait) == std::future_status::ready; }),
            pending_async_futures_.end());
    }

    return result_future;
}

void McpServer::AbandonPendingUrlElicitation(
    const std::string& elicitation_id, std::exception_ptr error) {
    std::shared_ptr<PendingUrlElicitation> pending;
    {
        std::lock_guard<std::mutex> lock(pending_url_elicitations_mutex_);
        auto it = pending_url_elicitations_.find(elicitation_id);
        if (it == pending_url_elicitations_.end()) return;
        pending = std::move(it->second);
        pending_url_elicitations_.erase(it);
    }
    pending->promise.set_exception(std::move(error));
}

void McpServer::WireHandlers() {
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);

    WireToolHandlers();
    WireResourceHandlers();
    WirePromptHandlers();
    WireCoreHandlers();
    WireExtensionHandlers();
    WireTaskHandlers();
    WireSkillHandlers();
    WireSubscriptionHandlers();
}

void McpServer::WireToolHandlers() {
    if (!tools_.empty()) {
        handler_->SetRequestHandler(methods::kListTools,
            [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                HandleListTools(req, std::move(p));
            });
    }

    handler_->SetRequestHandler(methods::kCallTool,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            HandleCallTool(req, std::move(p));
        });
}

void McpServer::WireResourceHandlers() {
    if (!resources_.empty() && std::any_of(resources_.begin(), resources_.end(),
            [](const auto& r) { return !r.is_template; })) {
        handler_->SetRequestHandler(methods::kListResources,
            [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                HandleListResources(req, std::move(p));
            });
    }

    if (!resources_.empty() && std::any_of(resources_.begin(), resources_.end(),
            [](const auto& r) { return r.is_template; })) {
        handler_->SetRequestHandler(methods::kListResourceTemplates,
            [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                HandleListResourceTemplates(req, std::move(p));
            });
    }

    if (!resources_.empty()) {
        handler_->SetRequestHandler(methods::kReadResource,
            [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                HandleReadResource(req, std::move(p));
            });
    }

    if (!resources_.empty()) {
        handler_->SetRequestHandler(methods::kSubscribeResource,
            [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (!RequireInitialized(IsSessionInitialized(req.session_id), handler_->IsJuly2026OrLater(req.session_id), p)) return;
                SubscribeRequestParams params;
                if (req.params) params = DeserializeResourceRequestParams(*req.params);
                SubscriptionFilter filter;
                filter.resource_subscriptions.push_back(params.uri);
                handler_->AddSubscription(Subscription{params.uri, std::move(filter)}, req.session_id);
                EmptyResult r;
                p.set_value(SerializeEmptyResult(r));
            });

        handler_->SetRequestHandler(methods::kUnsubscribeResource,
            [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (!RequireInitialized(IsSessionInitialized(req.session_id), handler_->IsJuly2026OrLater(req.session_id), p)) return;
                UnsubscribeRequestParams params;
                if (req.params) params = DeserializeResourceRequestParams(*req.params);
                handler_->RemoveSubscription(params.uri, req.session_id);
                EmptyResult r;
                p.set_value(SerializeEmptyResult(r));
            });
    }
}

void McpServer::WirePromptHandlers() {
    if (!prompts_.empty()) {
        handler_->SetRequestHandler(methods::kListPrompts,
            [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                HandleListPrompts(req, std::move(p));
            });
    }

    handler_->SetRequestHandler(methods::kGetPrompt,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            HandleGetPrompt(req, std::move(p));
        });
}

void McpServer::WireCoreHandlers() {
    handler_->SetRequestHandler(methods::kInitialize,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            HandleInitialize(req, std::move(p));
        });

    handler_->SetRequestHandler(methods::kDiscover,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            HandleDiscover(req, std::move(p));
        });

    handler_->SetRequestHandler(methods::kPing,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (!RequireInitialized(IsSessionInitialized(req.session_id), handler_->IsJuly2026OrLater(req.session_id), p)) return;
            PingResult r;
            p.set_value(SerializeEmptyResult(r));
        });

    handler_->SetNotificationHandler(notifications::kInitialized,
        [this](const JsonRpcNotification& notif) {
            bool was_initialized = IsSessionInitialized(notif.session_id);
            initialized_ = true;
            MarkSessionInitialized(notif.session_id);
            if (!was_initialized && options_.on_initialized) {
                try { options_.on_initialized(); } catch (...) {
                    MCP_LOG(Error, "on_initialized callback threw an exception");
                }
            }
        });

    handler_->SetRequestHandler(methods::kSetLoggingLevel,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (!RequireInitialized(IsSessionInitialized(req.session_id), handler_->IsJuly2026OrLater(req.session_id), p)) return;
            SetLevelRequestParams params;
            if (req.params) params = DeserializeSetLevelRequestParams(*req.params);
            {
                std::lock_guard<std::mutex> lock(log_level_mutex_);
                current_log_level_ = params.level;
            }
            EmptyResult r;
            p.set_value(SerializeEmptyResult(r));
        });

    handler_->SetNotificationHandler(notifications::kProgress,
        [this](const JsonRpcNotification& notif) {
            if (notif.params && notif.params->IsObject()) {
                auto* pt = notif.params->Find(detail::kProgressToken);
                if (pt) {
                    ProgressToken token;
                    if (pt->IsString())
                        token = pt->GetString();
                    else if (pt->IsInt())
                        token = pt->GetInt();
                    auto pt_key = std::visit([](const auto& v) -> std::string {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<T, std::string>) return v;
                        else return std::to_string(v);
                    }, token);
                    handler_->ResetTimeoutByProgressToken(pt_key);
                }
            }
        });

    handler_->SetRequestHandler(methods::kComplete,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            HandleComplete(req, std::move(p));
        });

    handler_->SetNotificationHandler(notifications::kElicitationComplete,
        [this](const JsonRpcNotification& notif) {
            if (handler_->IsJuly2026OrLater(notif.session_id)) return;
            if (!notif.params || !notif.params->IsObject()) return;
            auto* id = notif.params->Find("elicitationId");
            if (!id || !id->IsString()) return;
            std::shared_ptr<PendingUrlElicitation> pending;
            {
                std::lock_guard<std::mutex> lock(pending_url_elicitations_mutex_);
                auto it = pending_url_elicitations_.find(id->GetString());
                if (it == pending_url_elicitations_.end()) {
                    MCP_LOG(Warning,
                        "dropping elicitation complete for unknown id: " + id->GetString());
                    return;
                }
                pending = std::move(it->second);
                pending_url_elicitations_.erase(it);
                pending->completed = true;
            }
            pending_url_elicitations_cv_.notify_all();
            ElicitResult result;
            result.action = "accept";
            pending->promise.set_value(std::move(result));
        });
}

void McpServer::WireExtensionHandlers() {
    handler_->SetRequestHandler(methods::kListExtensions,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (!RequireInitialized(IsSessionInitialized(req.session_id), handler_->IsJuly2026OrLater(req.session_id), p)) return;
            std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
            JsonValue j(JsonValue::object_tag);
            JsonValue ext_list(JsonValue::array_tag);
            if (capabilities_.extensions) {
                for (const auto& [key, val] : *capabilities_.extensions) {
                    JsonValue entry(JsonValue::object_tag);
                    entry[detail::kName] = key;
                    entry["settings"] = val;
                    ext_list.PushBack(std::move(entry));
                }
            }
            j[detail::kExtensions] = std::move(ext_list);
            p.set_value(std::move(j));
        });
}

void McpServer::WireTaskHandlers() {
    auto& store = options_.task_store;
    if (store) {
        handler_->SetRequestHandler(methods::kGetTask,
            [this, store](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (!options_.enable_tasks_extension) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound,
                                 "tasks extension is disabled")));
                    return;
                }
                const bool modern =
                    IsModernProtocolVersion(handler_->NegotiatedProtocolVersion(req.session_id));
                if (modern && !TasksExtensionDeclared(GetClientCapabilities(req.session_id),
                        handler_->ExtractIncomingMeta(req).client_capabilities)) {
                    p.set_exception(std::make_exception_ptr(MissingTasksExtensionError()));
                    return;
                }
                RequireTaskPrincipal(req);
                if (modern) {
                    if (auto mcp_name = TryGetMcpName(req)) {
                        try {
                            auto codec = MakeWireCodec(
                                handler_->NegotiatedProtocolVersion(req.session_id));
                            JsonValue params = req.params
                                ? *req.params
                                : JsonValue(JsonValue::object_tag);
                            codec->ValidateTaskHeaders(req.method, params, *mcp_name);
                        } catch (...) {
                            p.set_exception(std::current_exception());
                            return;
                        }
                    }
                }
                GetTaskRequestParams params;
                if (req.params) params = DeserializeGetTaskRequestParams(*req.params);
                auto task = store->GetTask(params.task_id);
                if (!task || !IsTaskOwnedBySession(*task, TaskOwnerFor(req)) || !TaskScopeAllows(*task, req)) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::InvalidParams,
                                 "task not found: " + params.task_id)));
                    return;
                }
                p.set_value(MakeGetTaskResultJson(*task, true, modern));
            });

        handler_->SetRequestHandler(methods::kUpdateTask,
            [this, store](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (!options_.enable_tasks_extension) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound,
                                 "tasks extension is disabled")));
                    return;
                }
                const bool modern =
                    IsModernProtocolVersion(handler_->NegotiatedProtocolVersion(req.session_id));
                if (modern && !TasksExtensionDeclared(GetClientCapabilities(req.session_id),
                        handler_->ExtractIncomingMeta(req).client_capabilities)) {
                    p.set_exception(std::make_exception_ptr(MissingTasksExtensionError()));
                    return;
                }
                RequireTaskPrincipal(req);
                if (modern) {
                    if (auto mcp_name = TryGetMcpName(req)) {
                        try {
                            auto codec = MakeWireCodec(
                                handler_->NegotiatedProtocolVersion(req.session_id));
                            JsonValue params = req.params
                                ? *req.params
                                : JsonValue(JsonValue::object_tag);
                            codec->ValidateTaskHeaders(req.method, params, *mcp_name);
                        } catch (...) {
                            p.set_exception(std::current_exception());
                            return;
                        }
                    }
                }
                UpdateTaskRequestParams params;
                if (req.params) params = DeserializeUpdateTaskRequestParams(*req.params);
                if (params.result && params.result->Dump().size() > kMaxTaskResultBytes) {
                    MCP_LOG(Warning, "task result denied: payload exceeds budget");
                    throw McpError(McpErrorCode::TaskNotFound, "task payload exceeds budget");
                }
                if (params.input_responses) {
                    if (params.result) {
                        p.set_exception(std::make_exception_ptr(
                            McpError(McpErrorCode::InvalidParams,
                                     "tasks/update must not carry both result and inputResponses")));
                        return;
                    }
                    auto existing = store->GetTask(params.task_id);
                    if (!existing || !IsTaskOwnedBySession(*existing, TaskOwnerFor(req)) || !TaskScopeAllows(*existing, req)) {
                        p.set_exception(std::make_exception_ptr(
                            McpError(McpErrorCode::InvalidParams,
                                     "task not found: " + params.task_id)));
                        return;
                    }
                    if (existing->status != TaskStatus::InputRequired) {
                        p.set_exception(std::make_exception_ptr(
                            McpError(McpErrorCode::InvalidParams,
                                     "task is not waiting for input: " + params.task_id)));
                        return;
                    }
                    try {
                        if (!store->AdvanceTaskWithInput(params.task_id, *params.input_responses)) {
                            p.set_exception(std::make_exception_ptr(
                                McpError(McpErrorCode::InvalidParams,
                                         "task not found: " + params.task_id)));
                            return;
                        }
                    } catch (const std::exception& e) {
                        p.set_exception(std::make_exception_ptr(
                            McpError(McpErrorCode::InternalError,
                                     std::string("task persist failed: ") + e.what())));
                        return;
                    }
                    if (modern) {
                        if (auto detail_task = store->GetTask(params.task_id))
                            SendTaskDetail(*detail_task, req.session_id);
                    } else {
                        SendTaskNotification(*handler_,
                            notifications::kTaskWorking,
                            params.task_id, TaskStatus::Working, false,
                            TaskOwnerFor(req));
                    }
                    if (!modern) {
                        UpdateTaskResult r;
                        JsonValue ack = SerializeEmptyResult(r);
                        ack[detail::kResultType] = JsonValue(detail::kCompleteValue);
                        p.set_value(std::move(ack));
                        return;
                    }
                    auto resumed_task = store->GetTask(params.task_id);
                    std::shared_ptr<McpServerTool> resume_tool;
                    if (resumed_task && resumed_task->tool_name) {
                        std::shared_lock<std::shared_mutex> lock(registry_mutex_);
                        auto it = tools_.find(*resumed_task->tool_name);
                        if (it != tools_.end()) resume_tool = it->second;
                    }
                    if (!resumed_task || !resume_tool) {
                        UpdateTaskResult r;
                        JsonValue ack = SerializeEmptyResult(r);
                        ack[detail::kResultType] = JsonValue(detail::kCompleteValue);
                        p.set_value(std::move(ack));
                        return;
                    }
                    auto pending = ComputePendingInputKeys(
                        resumed_task->input_requests, resumed_task->input_responses);
                    if (!pending.empty()) {
                        try {
                            (void)store->ParkTaskForInput(params.task_id,
                                FilterInputRequests(resumed_task->input_requests, pending));
                        } catch (const std::exception& e) {
                            p.set_exception(std::make_exception_ptr(
                                McpError(McpErrorCode::InternalError,
                                         std::string("task persist failed: ") + e.what())));
                            return;
                        }
                        UpdateTaskResult r;
                        JsonValue ack = SerializeEmptyResult(r);
                        ack[detail::kResultType] = JsonValue(detail::kCompleteValue);
                        p.set_value(std::move(ack));
                        return;
                    }
                    CallToolRequestParams resume_params;
                    resume_params.name = *resumed_task->tool_name;
                    resume_params.arguments = resumed_task->tool_arguments;
                    resume_params.input_responses = resumed_task->input_responses;
                    std::shared_ptr<std::atomic<bool>> resume_flag;
                    {
                        std::lock_guard<std::mutex> lock(running_task_flags_mutex_);
                        auto it = running_task_cancel_flags_.find(params.task_id);
                        if (it == running_task_cancel_flags_.end()) {
                            resume_flag = std::make_shared<std::atomic<bool>>(false);
                            running_task_cancel_flags_[params.task_id] = resume_flag;
                        } else {
                            resume_flag = it->second;
                        }
                    }
                    auto resume_log_level = ParseRequestLogLevel(req);
                    auto resume_log = [this, resume_log_level](LoggingLevel level, std::string_view data) {
                        SendLoggingMessage(level, data, resume_log_level);
                    };
                    RequestContext<CallToolRequestParams> resume_ctx(
                        *this, req, std::move(resume_params),
                        std::move(resume_log), resume_flag,
                        handler_->IsJuly2026OrLater(req.session_id), req.session_id);
                    auto resume_promise = std::promise<CallToolResult>();
                    auto resume_future = resume_promise.get_future();
                    resume_tool->InvokeAsync(resume_ctx, std::move(resume_promise));
                    auto retire_resumed_task = [this, &params]() {
                        std::lock_guard<std::mutex> lock(running_task_flags_mutex_);
                        running_task_cancel_flags_.erase(params.task_id);
                    };
                    try {
                        auto resume_result = resume_future.get();
                        if (resume_flag->load()) {
                            (void)store->SetTaskStatus(params.task_id, TaskStatus::Cancelled);
                            retire_resumed_task();
                        } else if (resume_result.input_required) {
                            std::optional<InputRequests> fresh =
                                resume_result.input_required->input_requests;
                            auto remaining = ComputePendingInputKeys(
                                fresh, resumed_task->input_responses);
                            InputRequests next_park = remaining.empty()
                                ? *fresh
                                : FilterInputRequests(fresh, remaining);
                            (void)store->ParkTaskForInput(params.task_id, next_park);
                        } else {
                            (void)store->UpdateTask(params.task_id,
                                SerializeCallToolResult(resume_result));
                            (void)store->SetTaskStatus(params.task_id, TaskStatus::Completed);
                            retire_resumed_task();
                        }
                    } catch (const McpError& e) {
                        if (resume_flag->load()) {
                            (void)store->SetTaskStatus(params.task_id, TaskStatus::Cancelled);
                        } else {
                            JsonValue error_payload(JsonValue::object_tag);
                            error_payload[detail::kCode] =
                                JsonValue(static_cast<int64_t>(e.Code()));
                            error_payload[detail::kMessage] =
                                JsonValue(std::string(e.ErrorMessage()));
                            if (e.Data()) error_payload[detail::kData] = *e.Data();
                            (void)store->FailTask(params.task_id, error_payload);
                        }
                        retire_resumed_task();
                    } catch (const std::exception& e) {
                        if (resume_flag->load()) {
                            (void)store->SetTaskStatus(params.task_id, TaskStatus::Cancelled);
                        } else {
                            JsonValue error_payload(JsonValue::object_tag);
                            error_payload[detail::kCode] = JsonValue(
                                static_cast<int64_t>(McpErrorCode::InternalError));
                            error_payload[detail::kMessage] =
                                JsonValue(std::string(e.what()));
                            (void)store->FailTask(params.task_id, error_payload);
                        }
                        retire_resumed_task();
                    } catch (...) {
                        if (resume_flag->load()) {
                            (void)store->SetTaskStatus(params.task_id, TaskStatus::Cancelled);
                        } else {
                            JsonValue error_payload(JsonValue::object_tag);
                            error_payload[detail::kCode] = JsonValue(
                                static_cast<int64_t>(McpErrorCode::InternalError));
                            error_payload[detail::kMessage] =
                                JsonValue(std::string("tool execution failed"));
                            (void)store->FailTask(params.task_id, error_payload);
                        }
                        retire_resumed_task();
                    }
                    UpdateTaskResult r;
                    JsonValue ack = SerializeEmptyResult(r);
                    ack[detail::kResultType] = JsonValue(detail::kCompleteValue);
                    p.set_value(std::move(ack));
                    return;
                }
                auto target = store->GetTask(params.task_id);
                if (!target || !IsTaskOwnedBySession(*target, TaskOwnerFor(req)) || !TaskScopeAllows(*target, req)) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::InvalidParams,
                                 "task not found: " + params.task_id)));
                    return;
                }
                if (IsTerminalTaskStatus(target->status)) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::InvalidParams,
                                 "task already reached a terminal state: " +
                                     params.task_id)));
                    return;
                }
                try {
                    if (!store->UpdateTask(params.task_id, params.result)) {
                        p.set_exception(std::make_exception_ptr(
                            McpError(McpErrorCode::InvalidParams,
                                     "task not found: " + params.task_id)));
                        return;
                    }
                } catch (const std::exception& e) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::InternalError,
                                 std::string("task persist failed: ") + e.what())));
                    return;
                }
                if (modern) {
                    if (auto detail_task = store->GetTask(params.task_id))
                        SendTaskDetail(*detail_task, req.session_id);
                } else {
                    SendTaskNotification(*handler_,
                        params.result ? notifications::kTaskCompleted : notifications::kTaskWorking,
                        params.task_id,
                        params.result ? TaskStatus::Completed : TaskStatus::Working,
                        false, TaskOwnerFor(req));
                }
                UpdateTaskResult r;
                JsonValue ack = SerializeEmptyResult(r);
                ack[detail::kResultType] = JsonValue(detail::kCompleteValue);
                p.set_value(std::move(ack));
            });

        handler_->SetRequestHandler(methods::kCancelTask,
            [this, store](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (!options_.enable_tasks_extension) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound,
                                 "tasks extension is disabled")));
                    return;
                }
                const bool modern =
                    IsModernProtocolVersion(handler_->NegotiatedProtocolVersion(req.session_id));
                if (modern && !TasksExtensionDeclared(GetClientCapabilities(req.session_id),
                        handler_->ExtractIncomingMeta(req).client_capabilities)) {
                    p.set_exception(std::make_exception_ptr(MissingTasksExtensionError()));
                    return;
                }
                RequireTaskPrincipal(req);
                if (modern) {
                    if (auto mcp_name = TryGetMcpName(req)) {
                        try {
                            auto codec = MakeWireCodec(
                                handler_->NegotiatedProtocolVersion(req.session_id));
                            JsonValue params = req.params
                                ? *req.params
                                : JsonValue(JsonValue::object_tag);
                            codec->ValidateTaskHeaders(req.method, params, *mcp_name);
                        } catch (...) {
                            p.set_exception(std::current_exception());
                            return;
                        }
                    }
                }
                CancelTaskRequestParams params;
                if (req.params) params = DeserializeCancelTaskRequestParams(*req.params);
                auto existing = store->GetTask(params.task_id);
                if (!existing || !IsTaskOwnedBySession(*existing, TaskOwnerFor(req)) || !TaskScopeAllows(*existing, req)) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::InvalidParams,
                                 "task not found: " + params.task_id)));
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(running_task_flags_mutex_);
                    auto flag_it = running_task_cancel_flags_.find(params.task_id);
                    if (flag_it != running_task_cancel_flags_.end()) {
                        flag_it->second->store(true);
                        running_task_cancel_flags_.erase(flag_it);
                    }
                }
                const bool already_terminal =
                    existing->status == TaskStatus::Completed
                    || existing->status == TaskStatus::Failed
                    || existing->status == TaskStatus::Cancelled;
                if (already_terminal) {
                    CancelTaskResult r;
                    JsonValue ack = SerializeEmptyResult(r);
                    ack[detail::kResultType] = JsonValue(detail::kCompleteValue);
                    p.set_value(std::move(ack));
                    return;
                }
                try {
                    if (!store->CancelTask(params.task_id, params.reason)) {
                        p.set_exception(std::make_exception_ptr(
                            McpError(McpErrorCode::InvalidParams,
                                     "task not found: " + params.task_id)));
                        return;
                    }
                } catch (const std::exception& e) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::InternalError,
                                 std::string("task persist failed: ") + e.what())));
                    return;
                }
                if (modern) {
                    if (auto detail_task = store->GetTask(params.task_id))
                        SendTaskDetail(*detail_task, req.session_id);
                } else {
                    SendTaskNotification(*handler_, notifications::kTaskCancelled,
                        params.task_id, TaskStatus::Cancelled, false,
                        TaskOwnerFor(req));
                }
                CancelTaskResult r;
                JsonValue ack = SerializeEmptyResult(r);
                ack[detail::kResultType] = JsonValue(detail::kCompleteValue);
                p.set_value(std::move(ack));
            });

        handler_->SetRequestHandler(methods::kGetTaskPayload,
            [this, store](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (IsModernProtocolVersion(handler_->NegotiatedProtocolVersion(req.session_id))) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound,
                            "tasks/result is only available in 2025 and earlier protocol versions")));
                    return;
                }
                GetTaskRequestParams params;
                if (req.params) params = DeserializeGetTaskRequestParams(*req.params);
                auto task = store->GetTask(params.task_id);
                if (!task || !IsTaskOwnedBySession(*task, TaskOwnerFor(req)) || !TaskScopeAllows(*task, req)) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::InvalidParams,
                                 "task not found: " + params.task_id)));
                    return;
                }
                p.set_value(MakeGetTaskResultJson(*task, false, false));
            });

        handler_->SetRequestHandler(methods::kListTasks,
            [this, store](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (IsModernProtocolVersion(handler_->NegotiatedProtocolVersion(req.session_id))) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound,
                            "tasks/list is only available in 2025 and earlier protocol versions")));
                    return;
                }
                (void)req;
                const std::string owner = TaskOwnerFor(req);
                JsonValue result(JsonValue::object_tag);
                JsonValue tasks_arr(JsonValue::array_tag);
                auto all_tasks = store->GetAllTasks();
                for (const auto& t : all_tasks) {
                    if (!IsTaskOwnedBySession(t, owner) || !TaskScopeAllows(t, req)) continue;
                    JsonValue entry(JsonValue::object_tag);
                    entry[detail::kTaskId] = t.task_id;
                    entry["status"] = TaskStatusToWireString(t.status);
                    if (t.result) entry[detail::kResult] = *t.result;
                    tasks_arr.PushBack(std::move(entry));
                }
                result["tasks"] = std::move(tasks_arr);
                p.set_value(std::move(result));
            });

        handler_->SetRequestHandler(methods::kDeleteTask,
            [this, store](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (!options_.enable_tasks_extension) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound,
                                 "tasks extension is disabled")));
                    return;
                }
                if (!IsModernProtocolVersion(
                        handler_->NegotiatedProtocolVersion(req.session_id))) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound,
                                 "tasks/delete is only available in 2026 and later protocol versions")));
                    return;
                }
                if (!TasksExtensionDeclared(GetClientCapabilities(req.session_id),
                        handler_->ExtractIncomingMeta(req).client_capabilities)) {
                    p.set_exception(std::make_exception_ptr(MissingTasksExtensionError()));
                    return;
                }
                RequireTaskPrincipal(req);
                GetTaskRequestParams params;
                if (req.params) params = DeserializeGetTaskRequestParams(*req.params);
                auto existing = store->GetTask(params.task_id);
                if (!existing || !IsTaskOwnedBySession(*existing, TaskOwnerFor(req)) || !TaskScopeAllows(*existing, req)) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::TaskNotFound,
                                 "task not found: " + params.task_id)));
                    return;
                }
                try {
                    if (!store->RemoveTask(params.task_id)) {
                        p.set_exception(std::make_exception_ptr(
                            McpError(McpErrorCode::TaskNotFound,
                                     "task not found: " + params.task_id)));
                        return;
                    }
                } catch (const std::exception& e) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::InternalError,
                                 std::string("task persist failed: ") + e.what())));
                    return;
                }
                CancelTaskResult r;
                JsonValue ack = SerializeEmptyResult(r);
                ack[detail::kResultType] = JsonValue(detail::kCompleteValue);
                p.set_value(std::move(ack));
            });
    }
}

void McpServer::WireSkillHandlers() {
    if (skills_.empty()) return;
    handler_->SetRequestHandler(methods::kListSkills,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            HandleListSkills(req, std::move(p));
        });
    handler_->SetRequestHandler(methods::kGetSkill,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            HandleGetSkill(req, std::move(p));
        });
    handler_->SetRequestHandler(methods::kReadDirectory,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            HandleReadDirectory(req, std::move(p));
        });
}

void McpServer::WireSubscriptionHandlers() {
    handler_->SetRequestHandler(methods::kSubscribe,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            HandleSubscriptionsListen(req, std::move(p));
        });
}

void McpServer::DeriveCapabilities() {
    std::unique_lock<std::shared_mutex> registry_lock(registry_mutex_);
    if (!tools_.empty()) {
        capabilities_.tools = ToolsCapability{};
        capabilities_.tools->list_changed = true;
    }
    if (!resources_.empty()) {
        capabilities_.resources = ResourcesCapability{};
        capabilities_.resources->subscribe = true;
        capabilities_.resources->list_changed = true;
    }
    if (!prompts_.empty()) {
        capabilities_.prompts = PromptsCapability{};
        capabilities_.prompts->list_changed = true;
    }
    if (options_.declare_logging) {
        capabilities_.logging = LoggingCapability{};
    }
    if (options_.declare_completions) {
        capabilities_.completions = CompletionsCapability{};
    }
    if (options_.enable_tasks_extension) {
        if (!capabilities_.extensions)
            capabilities_.extensions = std::map<std::string, JsonValue>{};
        (*capabilities_.extensions)[std::string(kTasksExtensionId)] =
            SerializeTasksExtensionSettings(TasksExtensionSettings{});
    }
    if (options_.enable_apps_extension) {
        if (!capabilities_.extensions)
            capabilities_.extensions = std::map<std::string, JsonValue>{};
        (*capabilities_.extensions)[std::string(ext::kAppsExtensionId)] =
            JsonValue(JsonValue::object_tag);
    }
    if (options_.enable_server_card) {
        if (!capabilities_.extensions)
            capabilities_.extensions = std::map<std::string, JsonValue>{};
        (*capabilities_.extensions)[std::string(ext::kServerCardExtensionId)] =
            JsonValue(JsonValue::object_tag);
    }
    if (options_.enable_skills_extension && !skills_.empty() && capabilities_.resources) {
        if (!capabilities_.extensions)
            capabilities_.extensions = std::map<std::string, JsonValue>{};
        SkillsExtensionSettings skills_settings;
        skills_settings.directory_read = skills_directory_read_;
        (*capabilities_.extensions)[std::string(kSkillsExtensionId)] =
            SerializeSkillsExtensionSettings(skills_settings);
    }
}

ListToolsResult McpServer::BuildToolsResult() {
    ListToolsResult result;
    for (const auto& [name, tool_ptr] : tools_) {
        result.tools.push_back(tool_ptr->ProtocolTool());
    }
    return result;
}

std::vector<detail::McpParamAnnotation> McpServer::ResolveToolParamAnnotations(
    const std::string&, const std::string& name) const
{
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    auto it = tools_.find(name);
    if (it == tools_.end()) return {};
    auto parsed = detail::ParseToolParamAnnotations(it->second->ProtocolTool().input_schema);
    if (!parsed.IsValid()) return {};
    return std::move(parsed.annotations);
}

void McpServer::HandleListTools(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(IsSessionInitialized(req.session_id), handler_->IsJuly2026OrLater(req.session_id), promise)) return;
    if (!options_.enable_progressive_discovery) {
        std::optional<ListToolsResult> cached;
        {
            std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
            cached = cached_tools_;
        }
        if (!cached) {
            std::unique_lock<std::shared_mutex> registry_lock(registry_mutex_);
            if (!cached_tools_) {
                cached_tools_ = BuildToolsResult();
            }
            cached = cached_tools_;
        }
        auto hint = ResolveCacheHint("tools/list", req.session_id);
        promise.set_value(ApplyCacheHintAndSerialize(*cached, hint,
            SerializeListToolsResult));
        return;
    }
    std::optional<std::string> cursor;
    std::string query;
    size_t page_size = kDefaultPageSize;
    if (req.params && req.params->IsObject()) {
        cursor = DeserializePaginatedRequestParams(*req.params).cursor;
        if (auto* q = req.params->Find("query"); q && q->IsString())
            query = q->GetString();
        if (auto* ps = req.params->Find("pageSize"); ps && ps->IsInt()) {
            int64_t v = ps->GetInt();
            if (v < 1) v = 1;
            if (v > 100) v = 100;
            page_size = static_cast<size_t>(v);
        }
    }
    auto ascii_lower = [](std::string s) {
        for (auto& c : s) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        }
        return s;
    };
    std::string lowered_query = ascii_lower(query);
    std::vector<Tool> filtered;
    {
        std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
        for (const auto& [name, tool_ptr] : tools_) {
            const Tool& tool = tool_ptr->ProtocolTool();
            if (!lowered_query.empty()) {
                bool hit = ascii_lower(tool.name).find(lowered_query) != std::string::npos;
                if (!hit && tool.description)
                    hit = ascii_lower(*tool.description).find(lowered_query) != std::string::npos;
                if (!hit) continue;
            }
            filtered.push_back(tool);
        }
    }
    std::sort(filtered.begin(), filtered.end(),
        [](const Tool& a, const Tool& b) { return a.name < b.name; });
    ListToolsResult result;
    size_t next_index = 0;
    if (PaginateEntries(filtered, ParseCursor(cursor), page_size, next_index,
            [&result](const Tool& tool) { result.tools.push_back(tool); })) {
        result.next_cursor = MakeNextCursor(next_index);
    }
    auto hint = ResolveCacheHint("tools/list", req.session_id);
    promise.set_value(ApplyCacheHintAndSerialize(result, hint,
        SerializeListToolsResult));
}

void McpServer::HandleCallTool(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(IsSessionInitialized(req.session_id), handler_->IsJuly2026OrLater(req.session_id), promise)) return;

    CallToolRequestParams params;
    if (req.params) {
        params = DeserializeCallToolRequestParams(*req.params);
    }
    const std::string task_owner = TaskOwnerFor(req);
    const auto task_scopes = AuthorizedScopesFor(req);
    if (req.meta) {
        RequestMeta incoming;
        try {
            incoming = DeserializeRequestMeta(*req.meta);
        } catch (const McpError&) {
            throw;
        } catch (const std::exception& e) {
            throw McpError(McpErrorCode::InvalidParams,
                std::string("invalid _meta: ") + e.what());
        }
        if (!params.meta) {
            params.meta = std::move(incoming);
        } else {
            if (!params.meta->progress_token && incoming.progress_token) {
                params.meta->progress_token = incoming.progress_token;
            }
            if (req.meta->Find(detail::kMetaProtocolVersionKey)) {
                params.meta->protocol_version = incoming.protocol_version;
            }
            if (!params.meta->client_info && incoming.client_info) {
                params.meta->client_info = incoming.client_info;
            }
            if (!params.meta->client_capabilities && incoming.client_capabilities) {
                params.meta->client_capabilities = incoming.client_capabilities;
            }
            if (!params.meta->log_level && incoming.log_level) {
                params.meta->log_level = incoming.log_level;
            }
            if (incoming.extensions) {
                if (!params.meta->extensions) {
                    params.meta->extensions = incoming.extensions;
                } else {
                    for (const auto& [key, value] : incoming.extensions->GetObject()) {
                        if (!params.meta->extensions->Contains(key)) {
                            (*params.meta->extensions)[key] = value;
                        }
                    }
                }
            }
            if (!params.meta->traceparent && incoming.traceparent) {
                params.meta->traceparent = incoming.traceparent;
            }
            if (!params.meta->tracestate && incoming.tracestate) {
                params.meta->tracestate = incoming.tracestate;
            }
            if (!params.meta->baggage && incoming.baggage) {
                params.meta->baggage = incoming.baggage;
            }
        }
    }

    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    auto it = tools_.find(params.name);
    if (it == tools_.end()) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidParams,
                     "tool not found: " + params.name)));
        return;
    }
    auto tool = it->second;
    std::vector<FileInputResourceSnapshot> file_input_resources;
    if (options_.enable_file_input && params.arguments) {
        for (const auto& entry : resources_) {
            FileInputResourceSnapshot snapshot;
            snapshot.uri_pattern = entry.uri_pattern;
            snapshot.is_template = entry.is_template;
            snapshot.handler = entry.handler;
            snapshot.template_handler = entry.template_handler;
            file_input_resources.push_back(std::move(snapshot));
        }
    }
    registry_lock.unlock();

    if (options_.enable_file_input && params.arguments) {
        std::optional<std::string> file_input_error;
        const ServerOptions::FileInputConfig config = options_.file_input_config
            ? *options_.file_input_config
            : ServerOptions::FileInputConfig{};
        FileInputBudget budget(config);
        FileInputResolver resolver(config, file_input_resources, budget);
        std::function<void(JsonValue&)> resolve_file_input = [&](JsonValue& node) {
            if (file_input_error) return;
            if (node.IsArray()) {
                for (auto& item : node.GetArray()) resolve_file_input(item);
                return;
            }
            if (!IsFileInputRefObject(node)) {
                if (node.IsObject()) {
                    for (auto& entry : node.GetObject()) {
                        resolve_file_input(entry.second);
                    }
                }
                return;
            }
            const FileRef ref = DeserializeFileRef(node);
            std::string error;
            if (!resolver.Resolve(ref, node, error)) {
                file_input_error = std::move(error);
            }
        };
        resolve_file_input(*params.arguments);
        if (file_input_error) {
            promise.set_value(SerializeCallToolResult(
                MakeToolIsErrorResult(*file_input_error)));
            return;
        }
    }

    auto req_log_level = ParseRequestLogLevel(req);
    auto log_fn = [this, req_log_level](LoggingLevel level, std::string_view data) {
        SendLoggingMessage(level, data, req_log_level);
    };
    auto cancel_flag = handler_->GetIncomingCancellationFlag(req.id, req.session_id);
    if (!cancel_flag) {
        cancel_flag = std::make_shared<std::atomic<bool>>(false);
    }
    auto ctx = RequestContext<CallToolRequestParams>(
        *this, req, std::move(params), std::move(log_fn), cancel_flag,
        handler_->IsJuly2026OrLater(req.session_id), req.session_id);

    if (options_.max_tool_input_elements &&
        ToolInputElementCount(ctx.Params().arguments) > *options_.max_tool_input_elements) {
        promise.set_value(SerializeCallToolResult(MakeToolIsErrorResult(
            "tool arguments exceed maximum input element count")));
        return;
    }

    const auto& tool_def = tool->ProtocolTool();
    const bool task_declared = tool_def.execution
        && tool_def.execution->mode == ToolExecutionMode::Task;
    const bool modern_era =
        IsModernProtocolVersion(handler_->NegotiatedProtocolVersion(req.session_id));
    std::optional<ClientCapabilities> per_request_caps;
    if (ctx.Params().meta && ctx.Params().meta->client_capabilities) {
        per_request_caps = ctx.Params().meta->client_capabilities;
    }
    const bool tasks_declared = !modern_era
        || TasksExtensionDeclared(GetClientCapabilities(req.session_id), per_request_caps);

    if (task_declared && options_.task_store && !tasks_declared
        && tool_def.execution->task_support == TaskSupport::Required) {
        throw MissingTasksExtensionError();
    }

    if (task_declared && options_.task_store && tasks_declared &&
        options_.enable_tasks_extension) {
        const bool modern = modern_era;
        RequireTaskPrincipal(req);
        EnforceTaskBudgets(task_owner);
        const bool required_task =
            tool_def.execution->task_support == TaskSupport::Required;

        if (modern && required_task) {
            CallToolResult first_result;
            bool first_failed = false;
            JsonValue first_error(JsonValue::object_tag);
            std::string required_arguments_error;
            if (!ValidateToolCallArguments(
                    tool_def, ctx.Params().arguments, required_arguments_error)) {
                first_result = MakeToolIsErrorResult(
                    std::string("invalid arguments: ") + required_arguments_error);
            } else {
            auto required_promise = std::make_shared<std::promise<CallToolResult>>();
            auto required_future = required_promise->get_future();
            tool->InvokeAsync(ctx, std::move(*required_promise));
            try {
                first_result = required_future.get();
            } catch (const ElicitationInputRequired& e) {
                first_result.input_required = InputRequiredResult{};
                first_result.input_required->input_requests = e.input_requests;
            } catch (const McpError& e) {
                first_failed = true;
                first_error[detail::kCode] = JsonValue(static_cast<int64_t>(e.Code()));
                first_error[detail::kMessage] = JsonValue(std::string(e.ErrorMessage()));
                if (e.Data()) first_error[detail::kData] = *e.Data();
            } catch (const std::exception& e) {
                MCP_LOG(Error, std::string("tool '") + tool_def.name +
                    "' failed: " + e.what());
                first_failed = true;
                first_error[detail::kCode] =
                    JsonValue(static_cast<int64_t>(McpErrorCode::InternalError));
                first_error[detail::kMessage] =
                    JsonValue(ToolExecutionFailureMessage(tool_def.name));
            } catch (...) {
                MCP_LOG(Error, std::string("tool '") + tool_def.name +
                    "' failed with unknown exception");
                first_failed = true;
                first_error[detail::kCode] =
                    JsonValue(static_cast<int64_t>(McpErrorCode::InternalError));
                first_error[detail::kMessage] =
                    JsonValue(ToolExecutionFailureMessage(tool_def.name));
            }
            }
            if (!first_failed && first_result.input_required) {
                if (RequiresSamplingInput(first_result)
                    && !SamplingDeclared(GetClientCapabilities(req.session_id), per_request_caps)) {
                    promise.set_exception(
                        std::make_exception_ptr(MissingSamplingCapabilityError()));
                    return;
                }
                auto result = std::move(first_result);
                if (options_.request_state_key) {
                    JsonValue payload =
                        DecodeStatePayloadForMint(result.input_required->request_state);
                    payload["iat"] = JsonValue(
                        static_cast<int64_t>(std::time(nullptr)));
                    result.input_required->request_state =
                        detail::MintRequestState(*options_.request_state_key, payload);
                }
                promise.set_value(SerializeCallToolResult(result));
                return;
            }
            auto store = options_.task_store;
            SweepExpiredTasks(store);
            const std::string task_id = MakeTaskId(task_owner);
            {
                std::lock_guard<std::mutex> lock(running_task_flags_mutex_);
                running_task_cancel_flags_[task_id] = cancel_flag;
            }
            auto retire_required_task = [this, &task_id]() {
                std::lock_guard<std::mutex> lock(running_task_flags_mutex_);
                running_task_cancel_flags_.erase(task_id);
            };
            TaskStatus terminal_status = TaskStatus::Completed;
            try {
                store->CreateTask(task_id, task_owner, task_scopes, kDefaultTaskTtlMs);
                (void)store->BindTaskTool(task_id, tool_def.name, ctx.Params().arguments);
                if (cancel_flag->load()) {
                    (void)store->SetTaskStatus(task_id, TaskStatus::Cancelled);
                    terminal_status = TaskStatus::Cancelled;
                } else if (first_failed) {
                    (void)store->FailTask(task_id, first_error);
                    terminal_status = TaskStatus::Failed;
                } else {
                    (void)store->UpdateTask(task_id, SerializeCallToolResult(first_result));
                    (void)store->SetTaskStatus(task_id, TaskStatus::Completed);
                }
            } catch (...) {
                retire_required_task();
                throw;
            }
            retire_required_task();
            CreateTaskResult created;
            created.task_id = task_id;
            created.status = TaskStatusToWireString(terminal_status);
            created.ttl_ms = kDefaultTaskTtlMs;
            created.created_at = MakeIso8601Now();
            created.last_updated_at = created.created_at;
            JsonValue created_wire = SerializeCreateTaskResult(created);
            created_wire.GetObject().erase(std::string(detail::kTask));
            promise.set_value(std::move(created_wire));
            return;
        }

        auto store = options_.task_store;
        SweepExpiredTasks(store);
        const std::string task_id = MakeTaskId(task_owner);
        {
            std::lock_guard<std::mutex> lock(running_task_flags_mutex_);
            running_task_cancel_flags_[task_id] = cancel_flag;
        }
        store->CreateTask(task_id, task_owner, task_scopes, kDefaultTaskTtlMs);
        (void)store->BindTaskTool(task_id, tool_def.name, ctx.Params().arguments);
        (void)store->SetTaskStatus(task_id, TaskStatus::Working);
        if (modern) {
            if (auto detail_task = store->GetTask(task_id)) SendTaskDetail(*detail_task);
        } else {
            SendTaskNotification(*handler_, notifications::kTaskStatus,
                task_id, TaskStatus::Working, false, task_owner);
        }

        CreateTaskResult created;
        created.task_id = task_id;
        created.status = "working";
        created.ttl_ms = kDefaultTaskTtlMs;
        created.created_at = MakeIso8601Now();
        created.last_updated_at = created.created_at;
        JsonValue created_wire = SerializeCreateTaskResult(created);
        if (modern) created_wire.GetObject().erase(std::string(detail::kTask));
        promise.set_value(std::move(created_wire));

        auto fut = std::async(std::launch::async,
            [this, tool, ctx = std::move(ctx), store, task_id, cancel_flag, modern,
             task_owner]() mutable {
                auto notify_status = [&](TaskStatus status) {
                    if (modern) {
                        if (auto detail_task = store->GetTask(task_id))
                            SendTaskDetail(*detail_task);
                    } else {
                        SendTaskNotification(*handler_, notifications::kTaskStatus,
                            task_id, status, false, task_owner);
                    }
                };
                auto retire_task = [this, &task_id]() {
                    std::lock_guard<std::mutex> lock(running_task_flags_mutex_);
                    running_task_cancel_flags_.erase(task_id);
                };
                auto announce_stored_status = [&]() {
                    std::optional<TaskState> stored;
                    try {
                        stored = store->GetTask(task_id);
                    } catch (const std::exception& e) {
                        MCP_LOG(Error, "task store: failed to read task " + task_id +
                            " before status notification: " + e.what());
                        return;
                    }
                    if (!stored) {
                        MCP_LOG(Error, "task store: task " + task_id +
                            " vanished before status notification");
                        return;
                    }
                    notify_status(stored->status);
                };
                auto finish = [&](TaskStatus status, bool notify) {
                    const bool applied = store->SetTaskStatus(task_id, status);
                    if (notify) {
                        if (applied) {
                            notify_status(status);
                        } else {
                            announce_stored_status();
                        }
                    }
                    retire_task();
                };
                auto fail_with = [&](int64_t code, std::string message,
                        std::optional<JsonValue> data) {
                    JsonValue error_payload(JsonValue::object_tag);
                    error_payload[detail::kCode] = JsonValue(code);
                    error_payload[detail::kMessage] = JsonValue(std::move(message));
                    if (data) error_payload[detail::kData] = std::move(*data);
                    const bool applied = store->FailTask(task_id, error_payload);
                    if (applied) {
                        notify_status(TaskStatus::Failed);
                    } else {
                        announce_stored_status();
                    }
                    retire_task();
                };
                auto result_promise = std::make_shared<std::promise<CallToolResult>>();
                auto result_future = result_promise->get_future();
                std::string background_arguments_error;
                if (!ValidateToolCallArguments(tool->ProtocolTool(),
                        ctx.Params().arguments, background_arguments_error)) {
                    (void)store->UpdateTask(task_id, SerializeCallToolResult(
                        MakeToolIsErrorResult(std::string("invalid arguments: ") +
                            background_arguments_error)));
                    finish(TaskStatus::Completed, true);
                    return;
                }
                tool->InvokeAsync(ctx, std::move(*result_promise));
                CallToolResult pre_result;
                try {
                    pre_result = result_future.get();
                } catch (const ElicitationInputRequired& e) {
                    pre_result.input_required = InputRequiredResult{};
                    pre_result.input_required->input_requests = e.input_requests;
                } catch (const McpError& e) {
                    if (cancel_flag->load()) {
                        finish(TaskStatus::Cancelled, false);
                    } else {
                        fail_with(static_cast<int64_t>(e.Code()),
                            std::string(e.ErrorMessage()), e.Data());
                    }
                    return;
                } catch (const std::exception& e) {
                    MCP_LOG(Error, std::string("tool '") +
                        tool->ProtocolTool().name + "' failed: " + e.what());
                    if (cancel_flag->load()) {
                        finish(TaskStatus::Cancelled, false);
                    } else {
                        fail_with(static_cast<int64_t>(McpErrorCode::InternalError),
                            ToolExecutionFailureMessage(tool->ProtocolTool().name),
                            std::nullopt);
                    }
                    return;
                } catch (...) {
                    MCP_LOG(Error, std::string("tool '") +
                        tool->ProtocolTool().name + "' failed with unknown exception");
                    if (cancel_flag->load()) {
                        finish(TaskStatus::Cancelled, false);
                    } else {
                        fail_with(static_cast<int64_t>(McpErrorCode::InternalError),
                            ToolExecutionFailureMessage(tool->ProtocolTool().name),
                            std::nullopt);
                    }
                    return;
                }
                try {
                    auto result = std::move(pre_result);
                    if (cancel_flag->load()) {
                        finish(TaskStatus::Cancelled, false);
                        return;
                    }
                    if (modern && result.input_required) {
                        if (!store->ParkTaskForInput(
                                task_id, result.input_required->input_requests)) {
                            retire_task();
                            return;
                        }
                        return;
                    }
                    (void)store->UpdateTask(task_id, SerializeCallToolResult(result));
                    finish(TaskStatus::Completed, true);
                } catch (const McpError& e) {
                    if (cancel_flag->load()) {
                        finish(TaskStatus::Cancelled, false);
                    } else {
                        fail_with(static_cast<int64_t>(e.Code()),
                            std::string(e.ErrorMessage()), e.Data());
                    }
                } catch (const std::exception& e) {
                    if (cancel_flag->load()) {
                        finish(TaskStatus::Cancelled, false);
                    } else {
                        MCP_LOG(Error, std::string("tool '") +
                            tool->ProtocolTool().name + "' failed: " + e.what());
                        fail_with(static_cast<int64_t>(McpErrorCode::InternalError),
                            ToolExecutionFailureMessage(tool->ProtocolTool().name),
                            std::nullopt);
                    }
                } catch (...) {
                    if (cancel_flag->load()) {
                        finish(TaskStatus::Cancelled, false);
                    } else {
                        MCP_LOG(Error, std::string("tool '") +
                            tool->ProtocolTool().name + "' failed with unknown exception");
                        fail_with(static_cast<int64_t>(McpErrorCode::InternalError),
                            ToolExecutionFailureMessage(tool->ProtocolTool().name),
                            std::nullopt);
                    }
                }
            });

        std::lock_guard<std::mutex> lock(pending_async_mutex_);
        pending_async_futures_.push_back(fut.share());
        pending_async_futures_.erase(
            std::remove_if(pending_async_futures_.begin(), pending_async_futures_.end(),
                [](const auto& f) { return f.wait_for(kNoWait) == std::future_status::ready; }),
            pending_async_futures_.end());
        return;
    }

    auto captured_promise = std::make_shared<std::promise<JsonValue>>(std::move(promise));
    auto request_state_key = options_.request_state_key;
    auto fut = std::async(std::launch::async,
        [tool, ctx = std::move(ctx), captured_promise,
         request_state_key = std::move(request_state_key),
         modern_era, per_request_caps,
         stored_caps = GetClientCapabilities(req.session_id)]() mutable {
            std::string sync_arguments_error;
            if (!ValidateToolCallArguments(tool->ProtocolTool(),
                    ctx.Params().arguments, sync_arguments_error)) {
                captured_promise->set_value(SerializeCallToolResult(
                    MakeToolIsErrorResult(std::string("invalid arguments: ") +
                        sync_arguments_error)));
                return;
            }
            auto result_promise = std::make_shared<std::promise<CallToolResult>>();
            auto result_future = result_promise->get_future();
            tool->InvokeAsync(ctx, std::move(*result_promise));
            CallToolResult sync_result;
            try {
                sync_result = result_future.get();
            } catch (const ElicitationInputRequired& e) {
                sync_result.input_required = InputRequiredResult{};
                sync_result.input_required->input_requests = e.input_requests;
            } catch (const McpError& e) {
                captured_promise->set_value(SerializeCallToolResult(
                    MakeToolIsErrorResult(std::string(e.ErrorMessage()))));
                return;
            } catch (const std::exception& e) {
                MCP_LOG(Error, std::string("tool '") +
                    tool->ProtocolTool().name + "' failed: " + e.what());
                captured_promise->set_value(SerializeCallToolResult(
                    MakeToolIsErrorResult(
                        ToolExecutionFailureMessage(tool->ProtocolTool().name))));
                return;
            } catch (...) {
                MCP_LOG(Error, std::string("tool '") +
                    tool->ProtocolTool().name + "' failed with unknown exception");
                captured_promise->set_value(SerializeCallToolResult(
                    MakeToolIsErrorResult(
                        ToolExecutionFailureMessage(tool->ProtocolTool().name))));
                return;
            }
            if (modern_era && RequiresSamplingInput(sync_result)
                && !SamplingDeclared(stored_caps, per_request_caps)) {
                captured_promise->set_exception(
                    std::make_exception_ptr(MissingSamplingCapabilityError()));
                return;
            }
            try {
                auto result = std::move(sync_result);
                if (result.input_required) {
                    if (request_state_key) {
                        JsonValue payload =
                            DecodeStatePayloadForMint(result.input_required->request_state);
                        payload["iat"] = JsonValue(
                            static_cast<int64_t>(std::time(nullptr)));
                        result.input_required->request_state =
                            detail::MintRequestState(*request_state_key, payload);
                    }
                    captured_promise->set_value(SerializeCallToolResult(result));
                    return;
                }
                const auto& tool_def = tool->ProtocolTool();
                if (tool_def.output_schema && result.structured_content) {
                    std::string schema_error;
                    if (!detail::ValidateJsonSchema(*result.structured_content,
                            *tool_def.output_schema, schema_error)) {
                        captured_promise->set_exception(std::make_exception_ptr(
                            McpError(McpErrorCode::InvalidParams,
                                "tool output does not match outputSchema: " + schema_error)));
                        return;
                    }
                }
                captured_promise->set_value(SerializeCallToolResult(result));
            } catch (...) {
                captured_promise->set_exception(std::current_exception());
            }
        });

    std::lock_guard<std::mutex> lock(pending_async_mutex_);
    pending_async_futures_.push_back(fut.share());
    pending_async_futures_.erase(
        std::remove_if(pending_async_futures_.begin(), pending_async_futures_.end(),
            [](const auto& f) { return f.wait_for(kNoWait) == std::future_status::ready; }),
        pending_async_futures_.end());
}

void McpServer::HandleListResources(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(IsSessionInitialized(req.session_id), handler_->IsJuly2026OrLater(req.session_id), promise)) return;
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    ListResourcesResult result;
    size_t cursor_val = 0;
    if (req.params) {
        cursor_val = ParseCursor(DeserializePaginatedRequestParams(*req.params).cursor);
    }
    size_t next_index = 0;
    if (PaginateEntries(resources_, cursor_val, kDefaultPageSize, next_index,
            [&result](const ResourceEntry& entry) {
                Resource r;
                r.uri = entry.uri_pattern;
                r.name = entry.name;
                r.description = entry.description;
                r.title = entry.title;
                r.mime_type = entry.mime_type;
                if (!entry.icons.empty()) r.icons = entry.icons;
                result.resources.push_back(std::move(r));
            },
            [](const ResourceEntry& entry) { return !entry.is_template; })) {
        result.next_cursor = MakeNextCursor(next_index);
    }
    auto hint = ResolveCacheHint("resources/list", req.session_id);
    promise.set_value(ApplyCacheHintAndSerialize(result, hint,
        SerializeListResourcesResult));
}

void McpServer::HandleListResourceTemplates(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(IsSessionInitialized(req.session_id), handler_->IsJuly2026OrLater(req.session_id), promise)) return;
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    ListResourceTemplatesResult result;
    size_t cursor_val = 0;
    if (req.params) {
        cursor_val = ParseCursor(DeserializePaginatedRequestParams(*req.params).cursor);
    }
    size_t next_index = 0;
    if (PaginateEntries(resources_, cursor_val, kDefaultPageSize, next_index,
            [&result](const ResourceEntry& entry) {
                ResourceTemplate rt;
                rt.uri_template = entry.uri_pattern;
                rt.name = entry.name;
                rt.description = entry.description;
                rt.title = entry.title;
                rt.mime_type = entry.mime_type;
                if (!entry.icons.empty()) rt.icons = entry.icons;
                result.resource_templates.push_back(std::move(rt));
            },
            [](const ResourceEntry& entry) { return entry.is_template; })) {
        result.next_cursor = MakeNextCursor(next_index);
    }
    auto hint = ResolveCacheHint("resources/templates/list", req.session_id);
    promise.set_value(ApplyCacheHintAndSerialize(result, hint,
        SerializeListResourceTemplatesResult));
}

void McpServer::HandleReadResource(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(IsSessionInitialized(req.session_id), handler_->IsJuly2026OrLater(req.session_id), promise)) return;
    ReadResourceRequestParams params;
    if (req.params) {
        params = DeserializeResourceRequestParams(*req.params);
    }
    if (HasAppsUriScheme(params.uri)) {
        if (!options_.enable_apps_extension) {
            promise.set_exception(std::make_exception_ptr(
                McpError(McpErrorCode::HandlerError,
                         "apps extension is disabled")));
            return;
        }
        try {
            ext::UiMeta app_meta;
            app_meta.resource_uri = params.uri;
            ext::ValidateUiMeta(app_meta);
        } catch (...) {
            promise.set_exception(std::current_exception());
            return;
        }
    }

    std::function<ReadResourceResult(const std::string&)> resource_handler;
    std::function<ReadResourceResult(
        const std::string&, const std::map<std::string, std::string>&)> template_handler;
    std::map<std::string, std::string> variables;
    bool matched = false;
    {
        std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
        for (const auto& entry : resources_) {
            if (entry.is_template) continue;
            if (entry.uri_pattern != params.uri) continue;
            resource_handler = entry.handler;
            matched = true;
            break;
        }
        if (!matched) {
            for (const auto& entry : resources_) {
                if (!entry.is_template) continue;
                std::string error;
                auto tmpl = detail::UriTemplate::Parse(entry.uri_pattern, error);
                if (!tmpl) continue;
                auto matched_variables = tmpl->Match(params.uri);
                if (!matched_variables) continue;
                template_handler = entry.template_handler;
                variables = *matched_variables;
                matched = true;
                break;
            }
        }
    }

    if (!matched) {
        JsonValue data(JsonValue::object_tag);
        data["uri"] = JsonValue(params.uri);
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidParams,
                     "resource not found: " + params.uri, std::move(data))));
        return;
    }

    auto fut = std::async(std::launch::async,
        [this, resource_handler, template_handler,
         variables = std::move(variables), uri = params.uri,
         session_id = req.session_id,
         promise = std::move(promise)]() mutable {
            try {
                auto result = template_handler
                    ? template_handler(uri, variables)
                    : resource_handler(uri);
                if (result.input_required) {
                    if (options_.request_state_key) {
                        JsonValue payload =
                            DecodeStatePayloadForMint(result.input_required->request_state);
                        payload["iat"] = JsonValue(
                            static_cast<int64_t>(std::time(nullptr)));
                        result.input_required->request_state =
                            detail::MintRequestState(*options_.request_state_key, payload);
                    }
                    auto json = SerializeReadResourceResult(result);
                    StripCacheWireFields(json);
                    promise.set_value(std::move(json));
                    return;
                }
                auto hint = ResolveCacheHint("resources/read", session_id);
                promise.set_value(ApplyCacheHintAndSerialize(result, hint,
                    SerializeReadResourceResult));
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
        });

    {
        std::lock_guard<std::mutex> lock(pending_async_mutex_);
        pending_async_futures_.push_back(fut.share());
        pending_async_futures_.erase(
            std::remove_if(pending_async_futures_.begin(), pending_async_futures_.end(),
                [](const auto& f) { return f.wait_for(kNoWait) == std::future_status::ready; }),
            pending_async_futures_.end());
    }
}

void McpServer::HandleListPrompts(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(IsSessionInitialized(req.session_id), handler_->IsJuly2026OrLater(req.session_id), promise)) return;
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    ListPromptsResult result;
    size_t cursor_val = 0;
    if (req.params) {
        cursor_val = ParseCursor(DeserializePaginatedRequestParams(*req.params).cursor);
    }
    size_t next_index = 0;
    if (PaginateEntries(prompts_, cursor_val, kDefaultPageSize, next_index,
            [&result](const PromptEntry& entry) {
                Prompt p;
                p.name = entry.name;
                p.description = entry.description;
                p.title = entry.title;
                p.arguments = entry.arguments;
                if (!entry.icons.empty()) p.icons = entry.icons;
                result.prompts.push_back(std::move(p));
            })) {
        result.next_cursor = MakeNextCursor(next_index);
    }
    auto hint = ResolveCacheHint("prompts/list", req.session_id);
    promise.set_value(ApplyCacheHintAndSerialize(result, hint,
        SerializeListPromptsResult));
}

void McpServer::HandleGetPrompt(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(IsSessionInitialized(req.session_id), handler_->IsJuly2026OrLater(req.session_id), promise)) return;
    GetPromptRequestParams params;
    if (req.params) {
        params = DeserializeGetPromptRequestParams(*req.params);
    }

    std::function<GetPromptResult(
        const std::string&, const std::optional<JsonValue>&)> prompt_handler;
    {
        std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
        for (const auto& entry : prompts_) {
            if (entry.name != params.name) continue;
            prompt_handler = entry.handler;
            break;
        }
    }

    if (!prompt_handler) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidParams,
                     "prompt not found: " + params.name)));
        return;
    }

    auto fut = std::async(std::launch::async,
        [this, prompt_handler, name = params.name, arguments = params.arguments,
         promise = std::move(promise)]() mutable {
            try {
                auto result = prompt_handler(name, arguments);
                if (result.input_required) {
                    if (options_.request_state_key) {
                        JsonValue payload =
                            DecodeStatePayloadForMint(result.input_required->request_state);
                        payload["iat"] = JsonValue(
                            static_cast<int64_t>(std::time(nullptr)));
                        result.input_required->request_state =
                            detail::MintRequestState(*options_.request_state_key, payload);
                    }
                    promise.set_value(SerializeGetPromptResult(result));
                    return;
                }
                promise.set_value(SerializeGetPromptResult(result));
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
        });

    {
        std::lock_guard<std::mutex> lock(pending_async_mutex_);
        pending_async_futures_.push_back(fut.share());
        pending_async_futures_.erase(
            std::remove_if(pending_async_futures_.begin(), pending_async_futures_.end(),
                [](const auto& f) { return f.wait_for(kNoWait) == std::future_status::ready; }),
            pending_async_futures_.end());
    }
}

void McpServer::HandleListSkills(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(IsSessionInitialized(req.session_id), handler_->IsJuly2026OrLater(req.session_id), promise)) return;
    if (!options_.enable_skills_extension) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::MethodNotFound,
                     "skills extension is disabled")));
        return;
    }
    if (!IsModernProtocolVersion(handler_->NegotiatedProtocolVersion(req.session_id))) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::MethodNotFound,
                     "skills/list is only available in 2026 and later protocol versions")));
        return;
    }
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    if (!capabilities_.resources) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidParams,
                     "skills/list requires the resources capability")));
        return;
    }
    SkillsListResult result;
    size_t cursor_val = 0;
    if (req.params) {
        cursor_val = ParseCursor(DeserializePaginatedRequestParams(*req.params).cursor);
    }
    size_t next_index = 0;
    if (PaginateEntries(skills_, cursor_val, kDefaultPageSize, next_index,
            [&result](const Skill& skill) {
                result.skills.push_back(skill);
            })) {
        result.next_cursor = MakeNextCursor(next_index);
    }
    auto hint = ResolveCacheHint("skills/list", req.session_id);
    result.ttl_ms = hint.ttl_ms;
    result.cache_scope = hint.cache_scope;
    promise.set_value(SerializeSkillsListResult(result));
}

void McpServer::HandleGetSkill(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(IsSessionInitialized(req.session_id), handler_->IsJuly2026OrLater(req.session_id), promise)) return;
    if (!options_.enable_skills_extension) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::MethodNotFound,
                     "skills extension is disabled")));
        return;
    }
    if (!IsModernProtocolVersion(handler_->NegotiatedProtocolVersion(req.session_id))) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::MethodNotFound,
                     "skills/get is only available in 2026 and later protocol versions")));
        return;
    }
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    if (!capabilities_.resources) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidParams,
                     "skills/get requires the resources capability")));
        return;
    }
    SkillsGetRequestParams params;
    if (req.params) {
        params = DeserializeSkillsGetRequestParams(*req.params);
    }
    for (const auto& skill : skills_) {
        if (skill.uri == params.uri) {
            SkillsGetResult result;
            result.skill = skill;
            auto hint = ResolveCacheHint("skills/get", req.session_id);
            result.ttl_ms = hint.ttl_ms;
            result.cache_scope = hint.cache_scope;
            promise.set_value(SerializeSkillsGetResult(result));
            return;
        }
    }
    promise.set_exception(std::make_exception_ptr(
        McpError(McpErrorCode::InvalidParams,
                 "skill not found: " + params.uri)));
}

void McpServer::HandleReadDirectory(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(IsSessionInitialized(req.session_id), handler_->IsJuly2026OrLater(req.session_id), promise)) return;
    if (!options_.enable_skills_extension) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::MethodNotFound,
                     "skills extension is disabled")));
        return;
    }
    if (!IsModernProtocolVersion(handler_->NegotiatedProtocolVersion(req.session_id))) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::MethodNotFound,
                     "resources/directory/read is only available in 2026 and later protocol versions")));
        return;
    }
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    if (!capabilities_.resources) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidParams,
                     "resources/directory/read requires the resources capability")));
        return;
    }
    if (!skills_directory_read_) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidParams,
                     "resources/directory/read requires directoryRead to be declared")));
        return;
    }
    ReadDirectoryRequestParams params;
    if (req.params) {
        params = DeserializeReadDirectoryRequestParams(*req.params);
    }
    auto view = CollectDirectoryChildren(skills_, NormalizeDirectoryUri(params.uri));
    if (!view.is_directory) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidParams,
                     "directory not found: " + params.uri)));
        return;
    }
    ReadDirectoryResult result;
    size_t cursor_val = ParseCursor(params.cursor);
    size_t next_index = 0;
    if (PaginateEntries(view.children, cursor_val, kDefaultPageSize, next_index,
            [&result](const DirectoryChild& child) {
                result.resources.push_back(child);
            })) {
        result.next_cursor = MakeNextCursor(next_index);
    }
    auto hint = ResolveCacheHint("resources/directory/read", req.session_id);
    result.ttl_ms = hint.ttl_ms;
    result.cache_scope = hint.cache_scope;
    promise.set_value(SerializeReadDirectoryResult(result));
}

void McpServer::SetCompletionHandler(CompletionHandler handler) {
    if (!handler) {
        throw McpError(McpErrorCode::InvalidParams,
            "completion handler must not be empty");
    }
    std::lock_guard<std::mutex> lock(completion_handler_mutex_);
    completion_handler_ = std::move(handler);
}

void McpServer::HandleComplete(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(IsSessionInitialized(req.session_id), handler_->IsJuly2026OrLater(req.session_id), promise)) return;
    CompleteRequestParams params;
    if (req.params) params = DeserializeCompleteRequestParams(*req.params);

    CompletionHandler handler;
    {
        std::lock_guard<std::mutex> lock(completion_handler_mutex_);
        handler = completion_handler_;
    }

    if (!handler) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::MethodNotFound, "No completion handler registered")));
        return;
    }

    auto fut = std::async(std::launch::async,
        [handler, params, promise = std::move(promise)]() mutable {
            try {
                auto result = handler(params);
                promise.set_value(SerializeCompleteResult(result));
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
        });

    {
        std::lock_guard<std::mutex> lock(pending_async_mutex_);
        pending_async_futures_.push_back(fut.share());
        pending_async_futures_.erase(
            std::remove_if(pending_async_futures_.begin(), pending_async_futures_.end(),
                [](const auto& f) { return f.wait_for(kNoWait) == std::future_status::ready; }),
            pending_async_futures_.end());
    }
}

void McpServer::HandleDiscover(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    const auto incoming = handler_->ExtractIncomingMeta(req);
    if (!incoming.protocol_version.empty()
        && !IsModernProtocolVersion(incoming.protocol_version)) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::MethodNotFound,
                "server/discover is only available in 2026 and later protocol versions")));
        return;
    }
    initialized_ = true;
    MarkSessionInitialized(req.session_id);
    if (incoming.client_capabilities) {
        handler_->SetClientCapabilities(req.session_id, *incoming.client_capabilities);
        std::lock_guard<std::mutex> lock(client_info_mutex_);
        auto& state = client_sessions_[req.session_id];
        state.capabilities = std::make_shared<const ClientCapabilities>(
            *incoming.client_capabilities);
        if (req.session_id.empty()) client_capabilities_ = state.capabilities;
    }
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    if (options_.protocol_version) {
        handler_->SetNegotiatedProtocolVersion(req.session_id,
                                               *options_.protocol_version);
    } else {
        handler_->SetNegotiatedProtocolVersion(req.session_id,
                                               kLatestProtocolVersion);
    }
    DiscoverResult result;
    result.supported_versions = EffectiveSupportedVersions();
    result.capabilities = capabilities_;
    result.server_info = ResolveServerInfo();
    JsonValue discover_meta(JsonValue::object_tag);
    discover_meta[detail::kMetaServerInfoKey] =
        SerializeImplementation(result.server_info);
    result.meta = std::move(discover_meta);
    if (options_.server_instructions) {
        result.instructions = options_.server_instructions;
    }
    auto hint = ResolveCacheHint("server/discover", req.session_id);
    promise.set_value(ApplyCacheHintAndSerialize(result, hint,
        SerializeDiscoverResult));
}

void McpServer::HandleInitialize(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    InitializeRequestParams params;
    if (req.params) {
        params = DeserializeInitializeRequestParams(*req.params);
    }

    {
        std::lock_guard<std::mutex> lock(client_info_mutex_);
        auto caps = std::make_shared<const ClientCapabilities>(params.capabilities);
        auto info = std::make_shared<const Implementation>(params.client_info);
        if (req.session_id.empty()) {
            client_capabilities_ = caps;
            client_info_ = info;
        }
        auto& state = client_sessions_[req.session_id];
        state.capabilities = std::move(caps);
        state.client_info = std::move(info);
    }
    handler_->SetClientCapabilities(req.session_id, params.capabilities);

    if (options_.on_client_connected) {
        options_.on_client_connected(params.client_info);
    }

    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    if (options_.protocol_version) {
        handler_->SetNegotiatedProtocolVersion(req.session_id,
                                               *options_.protocol_version);
    } else {
        std::string selected(kDefaultNegotiatedProtocolVersion);
        if (!params.protocol_version.empty()) {
            selected = std::string(kLegacyProtocolVersion);
            for (const auto& v : EffectiveSupportedVersions()) {
                if (v == params.protocol_version && !IsModernProtocolVersion(v)) {
                    selected = v;
                    break;
                }
            }
        }
        handler_->SetNegotiatedProtocolVersion(req.session_id, selected);
    }

    InitializeResult result;
    result.protocol_version = handler_->NegotiatedProtocolVersion(req.session_id);
    result.capabilities = capabilities_;
    result.server_info = ResolveServerInfo();
    if (options_.server_instructions) {
        result.instructions = options_.server_instructions;
    }
    promise.set_value(SerializeInitializeResult(result));
}

void McpServer::HandleSubscriptionsListen(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!IsModernProtocolVersion(handler_->NegotiatedProtocolVersion(req.session_id))) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::MethodNotFound,
                     "subscriptions/listen not available in this protocol version")));
        return;
    }
    if (!options_.subscriptions_enabled) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::MethodNotFound,
                     "subscriptions are disabled")));
        return;
    }

    SubscriptionsListenRequestParams params;
    if (req.params) params = DeserializeSubscriptionsListenRequestParams(*req.params);

    auto meta = handler_->ExtractIncomingMeta(req);

    const bool client_named = meta.subscription_id.has_value() &&
        !meta.subscription_id->empty();
    SubscriptionEntry entry;
    entry.id = client_named
        ? *meta.subscription_id
        : "srv-" + std::to_string(next_subscription_id_++);
    entry.session_id = client_named ? *meta.subscription_id : std::string();
    entry.filter = params.notifications;
    entry.created_at = std::chrono::steady_clock::now();

    const std::string ack_subscription_id = entry.id;
    auto outcome = handler_->TryAddSubscriptionEntry(std::move(entry), req.session_id);
    if (outcome == SubscriptionAddOutcome::LimitExceeded) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidParams,
                "subscription limit reached for this session: " +
                    std::to_string(McpSessionHandler::kMaxSubscriptionEntriesPerSession))));
        return;
    }

    SendSubscriptionsAcknowledged(params.notifications, ack_subscription_id,
                                  req.session_id);

    JsonValue result = JsonValue(JsonValue::object_tag);
    promise.set_value(std::move(result));
}

void McpServer::SendSubscriptionsAcknowledged(
    const SubscriptionFilter& honored, std::string_view subscription_id,
    std::string_view session_id)
{
    JsonRpcNotification notif;
    notif.method = std::string(notifications::kSubscriptionsAcknowledged);
    notif.session_id = std::string(session_id);
    notif.params = SerializeSubscriptionsAcknowledgedNotificationParams(
        SubscriptionsAcknowledgedNotificationParams{honored});

    JsonValue meta(JsonValue::object_tag);
    meta[detail::kMetaProtocolVersionKey] =
        JsonValue(handler_->NegotiatedProtocolVersion(session_id));
    meta[detail::kMetaSubscriptionIdKey] = JsonValue(std::string(subscription_id));
    notif.meta = std::move(meta);

    handler_->SendMessage(JsonRpcMessage{std::move(notif)});
}

std::shared_ptr<const ClientCapabilities> McpServer::GetClientCapabilities() const {
    std::lock_guard<std::mutex> lock(client_info_mutex_);
    return client_capabilities_;
}

std::shared_ptr<const ClientCapabilities> McpServer::GetClientCapabilities(
    std::string_view session_id) const {
    return ClientCapabilitiesFor(session_id);
}

std::shared_ptr<const Implementation> McpServer::GetClientInfo() const {
    std::lock_guard<std::mutex> lock(client_info_mutex_);
    return client_info_;
}

std::shared_ptr<const Implementation> McpServer::GetClientInfo(
    std::string_view session_id) const {
    return ClientInfoFor(session_id);
}

std::string McpServer::GetNegotiatedProtocolVersion() const {
    return handler_->NegotiatedProtocolVersion();
}

std::string McpServer::GetNegotiatedProtocolVersion(
    std::string_view session_id) const {
    return handler_->NegotiatedProtocolVersion(session_id);
}

ServerCapabilities McpServer::GetCapabilities() const {
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    return capabilities_;
}

ext::ServerCard McpServer::GetServerCard() const {
    if (!options_.enable_server_card) {
        throw McpError(McpErrorCode::HandlerError, "server card is disabled");
    }
    ext::ServerCard card;
    if (options_.server_card) {
        card = *options_.server_card;
    } else {
        card.name = options_.server_info ? options_.server_info->name : "mcp-server";
        card.version = options_.server_info
            ? options_.server_info->version
            : std::string(kSdkVersion);
        ext::ServerCardEndpoint endpoint;
        endpoint.url = "http://localhost/";
        card.endpoints.push_back(std::move(endpoint));
        card.supported_protocol_versions = EffectiveSupportedVersions();
    }
    ext::ValidateCard(card);
    return card;
}

bool McpServer::IsMrtrSupported() const {
    if (is_stateless_) return false;
    auto caps = GetClientCapabilities(EffectiveSessionId({}));
    return caps && caps->elicitation.has_value();
}

bool McpServer::IsRunning() const {
    return initialized_.load() && handler_->IsRunning();
}

} // namespace mcp
