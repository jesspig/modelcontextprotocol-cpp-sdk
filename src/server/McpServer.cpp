#include <mcp/JsonValue.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/RequestState.hpp>
#include <mcp/Content.hpp>
#include <mcp/McpError.hpp>
#include <mcp/McpVersion.hpp>
#include <mcp/Transport.hpp>
#include <mcp/Log.hpp>
#include <detail/JsonFields.hpp>
#include <detail/JsonSchemaValidator.hpp>
#include <detail/UriTemplate.hpp>
#include <mcp/detail/McpParamAnnotations.hpp>

#include <condition_variable>
#include <cstddef>
#include <thread>
#include <set>
#include <algorithm>
#include <ctime>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <type_traits>

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

    constexpr std::string_view kUrlElicitationInputKey = "elicitation";

    struct UrlElicitationInputRequired : std::runtime_error {
        InputRequests input_requests;
        explicit UrlElicitationInputRequired(InputRequests requests)
            : std::runtime_error("url elicitation requires an MRTR input round-trip")
            , input_requests(std::move(requests)) {}
    };

    constexpr std::string_view kDefaultLoggerName = "mcp-server";
    constexpr std::string_view kTasksImplicitWarning =
        "task_store is set without enable_tasks_extension; tasks extension is implicitly enabled for compatibility";

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
        bool modern)
    {
        TaskStatusNotificationParams params;
        params.task_id = std::string(task_id);
        params.status = TaskStatusToWireString(status);
        JsonValue wire = SerializeTaskStatusNotificationParams(params);
        if (modern) {
            handler.NotifySubscribers(method, std::move(wire));
            return;
        }
        handler.SendNotification(method, std::move(wire));
    }

    std::string MakeIso8601Now() {
        std::time_t now = std::time(nullptr);
        std::tm tm_buf{};
#ifdef _WIN32
        gmtime_s(&tm_buf, &now);
#else
        gmtime_r(&now, &tm_buf);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
        return std::string(buf);
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
}

namespace {

CallToolResult MakeToolIsErrorResult(std::string text) {
    CallToolResult result;
    result.is_error = true;
    result.content.push_back(TextContent{"text", std::move(text)});
    return result;
}

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

CacheHint McpServer::ResolveCacheHint(std::string_view method) const {
    std::function<std::optional<CacheHint>(std::string_view)> hook;
    {
        std::lock_guard<std::mutex> lock(cache_hook_mutex_);
        hook = cache_hint_hook_;
    }
    if (hook) {
        if (auto resolved = hook(method)) return *resolved;
    }
    if (options_.cache_hints) {
        auto it = options_.cache_hints->find(method);
        if (it != options_.cache_hints->end()) return it->second;
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
{
    auto codec = MakeWireCodec(
        options_.protocol_version.value_or(std::string(kLatestProtocolVersion)));
    handler_ = std::make_shared<McpSessionHandler>(
        transport_, std::move(codec),
        options_.incoming_filters,
        options_.outgoing_filters);

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
        tools_[t.name] = std::move(tool);
        cached_tools_json_ = std::nullopt;
    }
    WireHandlers();
    DeriveCapabilities();
}

void McpServer::RegisterResource(
    std::string_view name,
    std::string_view uri,
    const ResourceOptions& opts,
    std::function<ReadResourceResult(const std::string&)> handler)
{
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
        resources_.push_back(std::move(entry));
    }
    WireHandlers();
    DeriveCapabilities();
}

void McpServer::RegisterResourceTemplate(
    std::string_view name,
    std::string_view uri_template,
    const ResourceOptions& opts,
    std::function<ReadResourceResult(
        const std::string&,
        const std::map<std::string, std::string>&)> handler)
{
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
        resources_.push_back(std::move(entry));
    }
    WireHandlers();
    DeriveCapabilities();
}

void McpServer::RegisterPrompt(
    std::string_view name,
    const PromptOptions& opts,
    std::function<GetPromptResult(
        const std::string&,
        const std::optional<JsonValue>&)> handler)
{
    PromptEntry entry;
    entry.name = std::string(name);
    entry.description = opts.description;
    entry.title = opts.title;
    entry.icons = opts.icons;
    entry.arguments = opts.arguments;
    entry.handler = std::move(handler);
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        prompts_.push_back(std::move(entry));
    }
    WireHandlers();
    DeriveCapabilities();
}

void McpServer::RegisterSkill(Skill skill) {
    if (!IsSkillEntryUri(skill.uri)) {
        throw McpError(McpErrorCode::InvalidParams,
            "invalid skill uri: " + skill.uri);
    }
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        auto it = skills_.begin();
        while (it != skills_.end() && it->uri != skill.uri) ++it;
        if (it != skills_.end()) *it = std::move(skill);
        else skills_.push_back(std::move(skill));
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
    }
    handler_->SendNotification(notifications::kMessage, std::move(params));
}

void McpServer::SendTaskStatus(std::string_view task_id, TaskStatus status) {
    SendTaskNotification(*handler_, notifications::kTaskStatus, task_id, status,
        IsModernProtocolVersion(handler_->NegotiatedProtocolVersion()));
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

std::future<ElicitResult> McpServer::Elicit(const ElicitRequestParams& params) {
    const bool url_mode = params.mode == "url";
    const auto caps = GetClientCapabilities();
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
    auto vers = handler_->NegotiatedProtocolVersion();
    meta.protocol_version = vers.empty()
        ? std::string(kLatestProtocolVersion) : std::string(vers);

    auto future = handler_->SendRequest(
        methods::kElicit, SerializeElicitRequestParams(params), meta,
        options_.input_required_config
            ? options_.input_required_config->round_timeout
            : kElicitTimeout);

    auto result_future = std::async(std::launch::async, [future = std::move(future)]() mutable {
        auto jv = future.get();
        if (auto* c = jv.Find("code"); c && c->IsInt() && c->GetInt() < 0) {
            auto msg = jv.Find("message");
            throw McpError(
                static_cast<McpErrorCode>(c->GetInt()),
                msg ? msg->GetString() : std::string("elicitation failed"));
        }
        return DeserializeElicitResult(jv);
    });

    return result_future;
}

std::future<ElicitResult> McpServer::ElicitUrl(
    const std::string& url,
    const std::string& message,
    std::chrono::seconds timeout) {
    if (IsModernProtocolVersion(handler_->NegotiatedProtocolVersion())) {
        (void)timeout;
        ElicitRequestParams params;
        params.message = message;
        params.mode = "url";
        params.url = url;
        InputRequest request;
        request.method = std::string(methods::kElicit);
        request.params = SerializeElicitRequestParams(
            params, handler_->NegotiatedProtocolVersion());
        InputRequests requests;
        requests[std::string(kUrlElicitationInputKey)] = std::move(request);
        throw UrlElicitationInputRequired(std::move(requests));
    }
    const std::string elicitation_id =
        "elicit-" + std::to_string(next_elicitation_id_.fetch_add(1));

    ElicitRequestParams params;
    params.message = message;
    params.mode = "url";
    params.url = url;
    params.elicitation_id = elicitation_id;

    RequestMeta meta;
    auto vers = handler_->NegotiatedProtocolVersion();
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
                if (auto* c = jv.Find("code"); c && c->IsInt() && c->GetInt() < 0) {
                    auto msg = jv.Find("message");
                    error = std::make_exception_ptr(McpError(
                        static_cast<McpErrorCode>(c->GetInt()),
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
                if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), p)) return;
                SubscribeRequestParams params;
                if (req.params) params = DeserializeResourceRequestParams(*req.params);
                SubscriptionFilter filter;
                filter.resource_subscriptions.push_back(params.uri);
                handler_->AddSubscription(Subscription{params.uri, std::move(filter)});
                EmptyResult r;
                p.set_value(SerializeEmptyResult(r));
            });

        handler_->SetRequestHandler(methods::kUnsubscribeResource,
            [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), p)) return;
                UnsubscribeRequestParams params;
                if (req.params) params = DeserializeResourceRequestParams(*req.params);
                handler_->RemoveSubscription(params.uri);
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
        [this](const JsonRpcRequest&, std::promise<JsonValue> p) {
            if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), p)) return;
            PingResult r;
            p.set_value(SerializeEmptyResult(r));
        });

    handler_->SetNotificationHandler(notifications::kInitialized,
        [this](const JsonRpcNotification&) {
            initialized_ = true;
            if (options_.on_initialized) {
                try { options_.on_initialized(); } catch (...) {
                    MCP_LOG(Error, "on_initialized callback threw an exception");
                }
            }
        });

    handler_->SetRequestHandler(methods::kSetLoggingLevel,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), p)) return;
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
            if (handler_->IsJuly2026OrLater()) return;
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
        [this](const JsonRpcRequest&, std::promise<JsonValue> p) {
            if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), p)) return;
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
                if (!options_.enable_tasks_extension && !options_.TasksImplicitlyEnabled()) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound,
                                 "tasks extension is disabled")));
                    return;
                }
                if (!options_.enable_tasks_extension) {
                    MCP_LOG(Warning, std::string(kTasksImplicitWarning));
                }
                const bool modern =
                    IsModernProtocolVersion(handler_->NegotiatedProtocolVersion());
                if (modern && !TasksExtensionDeclared(GetClientCapabilities(),
                        handler_->ExtractIncomingMeta(req).client_capabilities)) {
                    p.set_exception(std::make_exception_ptr(MissingTasksExtensionError()));
                    return;
                }
                if (modern) {
                    if (auto mcp_name = TryGetMcpName(req)) {
                        try {
                            auto codec = MakeWireCodec(handler_->NegotiatedProtocolVersion());
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
                if (!task) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::InvalidParams,
                                 "task not found: " + params.task_id)));
                    return;
                }
                p.set_value(MakeGetTaskResultJson(*task, true, modern));
            });

        handler_->SetRequestHandler(methods::kUpdateTask,
            [this, store](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (!options_.enable_tasks_extension && !options_.TasksImplicitlyEnabled()) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound,
                                 "tasks extension is disabled")));
                    return;
                }
                if (!options_.enable_tasks_extension) {
                    MCP_LOG(Warning, std::string(kTasksImplicitWarning));
                }
                const bool modern =
                    IsModernProtocolVersion(handler_->NegotiatedProtocolVersion());
                if (modern && !TasksExtensionDeclared(GetClientCapabilities(),
                        handler_->ExtractIncomingMeta(req).client_capabilities)) {
                    p.set_exception(std::make_exception_ptr(MissingTasksExtensionError()));
                    return;
                }
                if (modern) {
                    if (auto mcp_name = TryGetMcpName(req)) {
                        try {
                            auto codec = MakeWireCodec(handler_->NegotiatedProtocolVersion());
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
                if (params.input_responses) {
                    if (params.result) {
                        p.set_exception(std::make_exception_ptr(
                            McpError(McpErrorCode::InvalidParams,
                                     "tasks/update must not carry both result and inputResponses")));
                        return;
                    }
                    auto existing = store->GetTask(params.task_id);
                    if (!existing) {
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
                    SendTaskNotification(*handler_,
                        notifications::kTaskWorking,
                        params.task_id, TaskStatus::Working, modern);
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
                            store->ParkTaskForInput(params.task_id,
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
                        std::move(resume_log), resume_flag);
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
                            store->SetTaskStatus(params.task_id, TaskStatus::Cancelled);
                            retire_resumed_task();
                        } else if (resume_result.input_required) {
                            std::optional<InputRequests> fresh =
                                resume_result.input_required->input_requests;
                            auto remaining = ComputePendingInputKeys(
                                fresh, resumed_task->input_responses);
                            InputRequests next_park = remaining.empty()
                                ? *fresh
                                : FilterInputRequests(fresh, remaining);
                            store->ParkTaskForInput(params.task_id, next_park);
                        } else {
                            store->UpdateTask(params.task_id,
                                SerializeCallToolResult(resume_result));
                            store->SetTaskStatus(params.task_id, TaskStatus::Completed);
                            retire_resumed_task();
                        }
                    } catch (const McpError& e) {
                        if (resume_flag->load()) {
                            store->SetTaskStatus(params.task_id, TaskStatus::Cancelled);
                        } else {
                            JsonValue error_payload(JsonValue::object_tag);
                            error_payload[detail::kCode] =
                                JsonValue(static_cast<int64_t>(e.Code()));
                            error_payload[detail::kMessage] =
                                JsonValue(std::string(e.ErrorMessage()));
                            if (e.Data()) error_payload[detail::kData] = *e.Data();
                            store->UpdateTask(params.task_id, error_payload);
                            store->SetTaskStatus(params.task_id, TaskStatus::Failed);
                        }
                        retire_resumed_task();
                    } catch (const std::exception& e) {
                        if (resume_flag->load()) {
                            store->SetTaskStatus(params.task_id, TaskStatus::Cancelled);
                        } else {
                            JsonValue error_payload(JsonValue::object_tag);
                            error_payload[detail::kCode] = JsonValue(
                                static_cast<int64_t>(McpErrorCode::InternalError));
                            error_payload[detail::kMessage] =
                                JsonValue(std::string(e.what()));
                            store->UpdateTask(params.task_id, error_payload);
                            store->SetTaskStatus(params.task_id, TaskStatus::Failed);
                        }
                        retire_resumed_task();
                    } catch (...) {
                        if (resume_flag->load()) {
                            store->SetTaskStatus(params.task_id, TaskStatus::Cancelled);
                        } else {
                            JsonValue error_payload(JsonValue::object_tag);
                            error_payload[detail::kCode] = JsonValue(
                                static_cast<int64_t>(McpErrorCode::InternalError));
                            error_payload[detail::kMessage] =
                                JsonValue(std::string("tool execution failed"));
                            store->UpdateTask(params.task_id, error_payload);
                            store->SetTaskStatus(params.task_id, TaskStatus::Failed);
                        }
                        retire_resumed_task();
                    }
                    UpdateTaskResult r;
                    JsonValue ack = SerializeEmptyResult(r);
                    ack[detail::kResultType] = JsonValue(detail::kCompleteValue);
                    p.set_value(std::move(ack));
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
                SendTaskNotification(*handler_,
                    params.result ? notifications::kTaskCompleted : notifications::kTaskWorking,
                    params.task_id,
                    params.result ? TaskStatus::Completed : TaskStatus::Working,
                    modern);
                UpdateTaskResult r;
                JsonValue ack = SerializeEmptyResult(r);
                ack[detail::kResultType] = JsonValue(detail::kCompleteValue);
                p.set_value(std::move(ack));
            });

        handler_->SetRequestHandler(methods::kCancelTask,
            [this, store](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (!options_.enable_tasks_extension && !options_.TasksImplicitlyEnabled()) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound,
                                 "tasks extension is disabled")));
                    return;
                }
                if (!options_.enable_tasks_extension) {
                    MCP_LOG(Warning, std::string(kTasksImplicitWarning));
                }
                const bool modern =
                    IsModernProtocolVersion(handler_->NegotiatedProtocolVersion());
                if (modern && !TasksExtensionDeclared(GetClientCapabilities(),
                        handler_->ExtractIncomingMeta(req).client_capabilities)) {
                    p.set_exception(std::make_exception_ptr(MissingTasksExtensionError()));
                    return;
                }
                if (modern) {
                    if (auto mcp_name = TryGetMcpName(req)) {
                        try {
                            auto codec = MakeWireCodec(handler_->NegotiatedProtocolVersion());
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
                {
                    std::lock_guard<std::mutex> lock(running_task_flags_mutex_);
                    auto flag_it = running_task_cancel_flags_.find(params.task_id);
                    if (flag_it != running_task_cancel_flags_.end()) {
                        flag_it->second->store(true);
                        running_task_cancel_flags_.erase(flag_it);
                    }
                }
                auto existing = store->GetTask(params.task_id);
                if (!existing) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::InvalidParams,
                                 "task not found: " + params.task_id)));
                    return;
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
                SendTaskNotification(*handler_, notifications::kTaskCancelled,
                    params.task_id, TaskStatus::Cancelled, modern);
                CancelTaskResult r;
                JsonValue ack = SerializeEmptyResult(r);
                ack[detail::kResultType] = JsonValue(detail::kCompleteValue);
                p.set_value(std::move(ack));
            });

        handler_->SetRequestHandler(methods::kGetTaskPayload,
            [this, store](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (IsModernProtocolVersion(handler_->NegotiatedProtocolVersion())) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound,
                            "tasks/result is only available in 2025 and earlier protocol versions")));
                    return;
                }
                GetTaskRequestParams params;
                if (req.params) params = DeserializeGetTaskRequestParams(*req.params);
                auto task = store->GetTask(params.task_id);
                if (!task) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::InvalidParams,
                                 "task not found: " + params.task_id)));
                    return;
                }
                p.set_value(MakeGetTaskResultJson(*task, false, false));
            });

        handler_->SetRequestHandler(methods::kListTasks,
            [this, store](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (IsModernProtocolVersion(handler_->NegotiatedProtocolVersion())) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound,
                            "tasks/list is only available in 2025 and earlier protocol versions")));
                    return;
                }
                (void)req;
                JsonValue result(JsonValue::object_tag);
                JsonValue tasks_arr(JsonValue::array_tag);
                auto all_tasks = store->GetAllTasks();
                for (const auto& t : all_tasks) {
                    JsonValue entry(JsonValue::object_tag);
                    entry[detail::kTaskId] = t.task_id;
                    entry["status"] = TaskStatusToWireString(t.status);
                    if (t.result) entry[detail::kResult] = *t.result;
                    tasks_arr.PushBack(std::move(entry));
                }
                result["tasks"] = std::move(tasks_arr);
                p.set_value(std::move(result));
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
    if (options_.enable_tasks_extension || options_.TasksImplicitlyEnabled()) {
        if (!options_.enable_tasks_extension && options_.task_store) {
            MCP_LOG(Warning, std::string(kTasksImplicitWarning));
        }
        if (!capabilities_.extensions)
            capabilities_.extensions = std::map<std::string, JsonValue>{};
        (*capabilities_.extensions)[std::string(kTasksExtensionId)] =
            SerializeTasksExtensionSettings(TasksExtensionSettings{});
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

JsonValue McpServer::BuildToolsJson() {
    ListToolsResult result;
    for (const auto& [name, tool_ptr] : tools_) {
        result.tools.push_back(tool_ptr->ProtocolTool());
    }
    auto hint = ResolveCacheHint("tools/list");
    if (hint.ttl_ms || hint.cache_scope) result.cache_hint = hint;
    return SerializeListToolsResult(result);
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
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;
    if (!options_.enable_progressive_discovery) {
        {
            std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
            if (cached_tools_json_) {
                promise.set_value(*cached_tools_json_);
                return;
            }
        }
        {
            std::unique_lock<std::shared_mutex> registry_lock(registry_mutex_);
            if (!cached_tools_json_) {
                cached_tools_json_ = BuildToolsJson();
            }
            promise.set_value(*cached_tools_json_);
        }
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
    auto hint = ResolveCacheHint("tools/list");
    if (hint.ttl_ms || hint.cache_scope) result.cache_hint = hint;
    promise.set_value(SerializeListToolsResult(result));
}

void McpServer::HandleCallTool(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;

    CallToolRequestParams params;
    if (req.params) {
        params = DeserializeCallToolRequestParams(*req.params);
    }
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

    auto req_log_level = ParseRequestLogLevel(req);
    auto log_fn = [this, req_log_level](LoggingLevel level, std::string_view data) {
        SendLoggingMessage(level, data, req_log_level);
    };
    auto cancel_flag = handler_->GetIncomingCancellationFlag(req.id);
    if (!cancel_flag) {
        cancel_flag = std::make_shared<std::atomic<bool>>(false);
    }
    auto ctx = RequestContext<CallToolRequestParams>(
        *this, req, std::move(params), std::move(log_fn), cancel_flag);

    auto tool = it->second;

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
        IsModernProtocolVersion(handler_->NegotiatedProtocolVersion());
    std::optional<ClientCapabilities> per_request_caps;
    if (ctx.Params().meta && ctx.Params().meta->client_capabilities) {
        per_request_caps = ctx.Params().meta->client_capabilities;
    }
    const bool tasks_declared = !modern_era
        || TasksExtensionDeclared(GetClientCapabilities(), per_request_caps);

    if (task_declared && options_.task_store && !tasks_declared
        && tool_def.execution->task_support == TaskSupport::Required) {
        throw MissingTasksExtensionError();
    }

    if (task_declared && options_.task_store && tasks_declared &&
        (options_.enable_tasks_extension || options_.TasksImplicitlyEnabled())) {
        if (!options_.enable_tasks_extension) {
            MCP_LOG(Warning, std::string(kTasksImplicitWarning));
        }
        const bool modern = modern_era;
        const bool required_task =
            tool_def.execution->task_support == TaskSupport::Required;

        if (modern && required_task) {
            registry_lock.unlock();
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
            } catch (const UrlElicitationInputRequired& e) {
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
                    && !SamplingDeclared(GetClientCapabilities(), per_request_caps)) {
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
            const std::string task_id = "task-" + std::to_string(next_task_id_++);
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
                store->CreateTask(task_id);
                store->BindTaskTool(task_id, tool_def.name, ctx.Params().arguments);
                if (cancel_flag->load()) {
                    store->SetTaskStatus(task_id, TaskStatus::Cancelled);
                    terminal_status = TaskStatus::Cancelled;
                } else if (first_failed) {
                    store->UpdateTask(task_id, first_error);
                    store->SetTaskStatus(task_id, TaskStatus::Failed);
                    terminal_status = TaskStatus::Failed;
                } else {
                    store->UpdateTask(task_id, SerializeCallToolResult(first_result));
                    store->SetTaskStatus(task_id, TaskStatus::Completed);
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
        const std::string task_id = "task-" + std::to_string(next_task_id_++);
        {
            std::lock_guard<std::mutex> lock(running_task_flags_mutex_);
            running_task_cancel_flags_[task_id] = cancel_flag;
        }
        store->CreateTask(task_id);
        store->BindTaskTool(task_id, tool_def.name, ctx.Params().arguments);
        store->SetTaskStatus(task_id, TaskStatus::Working);
        SendTaskNotification(*handler_, notifications::kTaskStatus,
            task_id, TaskStatus::Working, modern);

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
            [this, tool, ctx = std::move(ctx), store, task_id, cancel_flag, modern]() mutable {
                auto retire_task = [this, &task_id]() {
                    std::lock_guard<std::mutex> lock(running_task_flags_mutex_);
                    running_task_cancel_flags_.erase(task_id);
                };
                auto finish = [&](TaskStatus status, bool notify) {
                    store->SetTaskStatus(task_id, status);
                    if (notify) {
                        SendTaskNotification(*handler_, notifications::kTaskStatus,
                            task_id, status, modern);
                    }
                    retire_task();
                };
                auto fail_with = [&](int64_t code, std::string message,
                        std::optional<JsonValue> data) {
                    JsonValue error_payload(JsonValue::object_tag);
                    error_payload[detail::kCode] = JsonValue(code);
                    error_payload[detail::kMessage] = JsonValue(std::move(message));
                    if (data) error_payload[detail::kData] = std::move(*data);
                    store->UpdateTask(task_id, error_payload);
                    finish(TaskStatus::Failed, true);
                };
                auto result_promise = std::make_shared<std::promise<CallToolResult>>();
                auto result_future = result_promise->get_future();
                std::string background_arguments_error;
                if (!ValidateToolCallArguments(tool->ProtocolTool(),
                        ctx.Params().arguments, background_arguments_error)) {
                    store->UpdateTask(task_id, SerializeCallToolResult(
                        MakeToolIsErrorResult(std::string("invalid arguments: ") +
                            background_arguments_error)));
                    finish(TaskStatus::Completed, true);
                    return;
                }
                tool->InvokeAsync(ctx, std::move(*result_promise));
                CallToolResult pre_result;
                try {
                    pre_result = result_future.get();
                } catch (const UrlElicitationInputRequired& e) {
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
                    store->UpdateTask(task_id, SerializeCallToolResult(result));
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
         stored_caps = GetClientCapabilities()]() mutable {
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
            } catch (const UrlElicitationInputRequired& e) {
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
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;
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
    auto hint = ResolveCacheHint("resources/list");
    if (hint.ttl_ms || hint.cache_scope) result.cache_hint = hint;
    promise.set_value(SerializeListResourcesResult(result));
}

void McpServer::HandleListResourceTemplates(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;
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
    auto hint = ResolveCacheHint("resources/templates/list");
    if (hint.ttl_ms || hint.cache_scope) result.cache_hint = hint;
    promise.set_value(SerializeListResourceTemplatesResult(result));
}

void McpServer::HandleReadResource(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;
    ReadResourceRequestParams params;
    if (req.params) {
        params = DeserializeResourceRequestParams(*req.params);
    }

    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    for (const auto& entry : resources_) {
        if (entry.is_template) continue;
        if (entry.uri_pattern == params.uri) {
            try {
                auto result = entry.handler(params.uri);
                if (result.input_required) {
                    if (options_.request_state_key) {
                        JsonValue payload =
                            DecodeStatePayloadForMint(result.input_required->request_state);
                        payload["iat"] = JsonValue(
                            static_cast<int64_t>(std::time(nullptr)));
                        result.input_required->request_state =
                            detail::MintRequestState(*options_.request_state_key, payload);
                    }
                    promise.set_value(SerializeReadResourceResult(result));
                    return;
                }
                auto hint = ResolveCacheHint("resources/read");
                if (hint.ttl_ms || hint.cache_scope) result.cache_hint = hint;
                promise.set_value(SerializeReadResourceResult(result));
                return;
            } catch (...) {
                promise.set_exception(std::current_exception());
                return;
            }
        }
    }

    for (const auto& entry : resources_) {
        if (!entry.is_template) continue;
        std::string error;
        auto tmpl = detail::UriTemplate::Parse(entry.uri_pattern, error);
        if (!tmpl) continue;
        auto variables = tmpl->Match(params.uri);
        if (!variables) continue;
        try {
            auto result = entry.template_handler(params.uri, *variables);
            if (result.input_required) {
                if (options_.request_state_key) {
                    JsonValue payload =
                        DecodeStatePayloadForMint(result.input_required->request_state);
                    payload["iat"] = JsonValue(
                        static_cast<int64_t>(std::time(nullptr)));
                    result.input_required->request_state =
                        detail::MintRequestState(*options_.request_state_key, payload);
                }
                promise.set_value(SerializeReadResourceResult(result));
                return;
            }
            auto hint = ResolveCacheHint("resources/read");
            if (hint.ttl_ms || hint.cache_scope) result.cache_hint = hint;
            promise.set_value(SerializeReadResourceResult(result));
            return;
        } catch (...) {
            promise.set_exception(std::current_exception());
            return;
        }
    }

    JsonValue data(JsonValue::object_tag);
    data["uri"] = JsonValue(params.uri);
    promise.set_exception(std::make_exception_ptr(
        McpError(McpErrorCode::InvalidParams,
                 "resource not found: " + params.uri, std::move(data))));
}

void McpServer::HandleListPrompts(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;
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
    auto hint = ResolveCacheHint("prompts/list");
    if (hint.ttl_ms || hint.cache_scope) result.cache_hint = hint;
    promise.set_value(SerializeListPromptsResult(result));
}

void McpServer::HandleGetPrompt(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;
    GetPromptRequestParams params;
    if (req.params) {
        params = DeserializeGetPromptRequestParams(*req.params);
    }

    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    for (const auto& entry : prompts_) {
        if (entry.name == params.name) {
            try {
                auto result = entry.handler(params.name, params.arguments);
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
                return;
            } catch (...) {
                promise.set_exception(std::current_exception());
                return;
            }
        }
    }

    promise.set_exception(std::make_exception_ptr(
        McpError(McpErrorCode::InvalidParams,
                 "prompt not found: " + params.name)));
}

void McpServer::HandleListSkills(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;
    if (!options_.enable_skills_extension) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::MethodNotFound,
                     "skills extension is disabled")));
        return;
    }
    if (!IsModernProtocolVersion(handler_->NegotiatedProtocolVersion())) {
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
    auto hint = ResolveCacheHint("skills/list");
    result.ttl_ms = hint.ttl_ms;
    result.cache_scope = hint.cache_scope;
    promise.set_value(SerializeSkillsListResult(result));
}

void McpServer::HandleGetSkill(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;
    if (!options_.enable_skills_extension) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::MethodNotFound,
                     "skills extension is disabled")));
        return;
    }
    if (!IsModernProtocolVersion(handler_->NegotiatedProtocolVersion())) {
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
            auto hint = ResolveCacheHint("skills/get");
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
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;
    if (!options_.enable_skills_extension) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::MethodNotFound,
                     "skills extension is disabled")));
        return;
    }
    if (!IsModernProtocolVersion(handler_->NegotiatedProtocolVersion())) {
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
    auto hint = ResolveCacheHint("resources/directory/read");
    result.ttl_ms = hint.ttl_ms;
    result.cache_scope = hint.cache_scope;
    promise.set_value(SerializeReadDirectoryResult(result));
}

void McpServer::SetCompletionHandler(CompletionHandler handler) {
    completion_handler_ = std::move(handler);
}

void McpServer::HandleComplete(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;
    CompleteRequestParams params;
    if (req.params) params = DeserializeCompleteRequestParams(*req.params);

    if (completion_handler_) {
        try {
            auto result = completion_handler_(params);
            promise.set_value(SerializeCompleteResult(result));
            return;
        } catch (...) {
            promise.set_exception(std::current_exception());
            return;
        }
    }

    promise.set_exception(std::make_exception_ptr(
        McpError(McpErrorCode::MethodNotFound, "No completion handler registered")));
}

void McpServer::HandleDiscover(
    const JsonRpcRequest&, std::promise<JsonValue> promise)
{
    initialized_ = true;
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    if (options_.protocol_version) {
        handler_->SetNegotiatedProtocolVersion(*options_.protocol_version);
    } else {
        handler_->SetNegotiatedProtocolVersion(kLatestProtocolVersion);
    }
    DiscoverResult result;
    result.supported_versions = EffectiveSupportedVersions();
    result.capabilities = capabilities_;
    if (options_.server_info) {
        result.server_info = Implementation{
            options_.server_info->name,
            options_.server_info->version,
            {}
        };
    } else {
        result.server_info = Implementation{"mcp-server", std::string(kSdkVersion), {}};
    }
    JsonValue discover_meta(JsonValue::object_tag);
    discover_meta[detail::kMetaServerInfoKey] =
        SerializeImplementation(result.server_info);
    result.meta = std::move(discover_meta);
    if (options_.server_instructions) {
        result.instructions = options_.server_instructions;
    }
    auto hint = ResolveCacheHint("server/discover");
    if (hint.ttl_ms || hint.cache_scope) result.cache_hint = hint;
    promise.set_value(SerializeDiscoverResult(result));
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
        client_capabilities_ = std::make_shared<const ClientCapabilities>(params.capabilities);
        client_info_ = std::make_shared<const Implementation>(params.client_info);
    }

    std::shared_ptr<const Implementation> client_info;
    {
        std::lock_guard<std::mutex> lock(client_info_mutex_);
        client_info = client_info_;
    }
    if (options_.on_client_connected && client_info) {
        options_.on_client_connected(*client_info);
    }

    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    if (options_.protocol_version) {
        handler_->SetNegotiatedProtocolVersion(*options_.protocol_version);
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
        handler_->SetNegotiatedProtocolVersion(selected);
    }

    InitializeResult result;
    result.protocol_version = std::string(handler_->NegotiatedProtocolVersion());
    result.capabilities = capabilities_;
    if (options_.server_info) {
        result.server_info = Implementation{
            options_.server_info->name,
            options_.server_info->version,
            {}
        };
    } else {
        result.server_info = Implementation{"mcp-server", std::string(kSdkVersion), {}};
    }
    if (options_.server_instructions) {
        result.instructions = options_.server_instructions;
    }
    promise.set_value(SerializeInitializeResult(result));
}

void McpServer::HandleSubscriptionsListen(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!IsModernProtocolVersion(handler_->NegotiatedProtocolVersion())) {
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

    SubscriptionEntry entry;
    entry.id = std::to_string(next_subscription_id_++);
    entry.filter = params.notifications;
    entry.created_at = std::chrono::steady_clock::now();

    if (meta.subscription_id) {
        entry.session_id = *meta.subscription_id;
    }

    std::string ack_subscription_id =
        entry.session_id.empty() ? entry.id : entry.session_id;
    handler_->AddSubscriptionEntry(std::move(entry));

    SendSubscriptionsAcknowledged(params.notifications, ack_subscription_id);

    JsonValue result = JsonValue(JsonValue::object_tag);
    promise.set_value(std::move(result));
}

void McpServer::SendSubscriptionsAcknowledged(
    const SubscriptionFilter& honored, std::string_view subscription_id)
{
    JsonRpcNotification notif;
    notif.method = std::string(notifications::kSubscriptionsAcknowledged);
    notif.params = SerializeSubscriptionsAcknowledgedNotificationParams(
        SubscriptionsAcknowledgedNotificationParams{honored});

    JsonValue meta(JsonValue::object_tag);
    meta[detail::kMetaProtocolVersionKey] =
        JsonValue(handler_->NegotiatedProtocolVersion());
    meta[detail::kMetaSubscriptionIdKey] = JsonValue(std::string(subscription_id));
    notif.meta = std::move(meta);

    handler_->SendMessage(JsonRpcMessage{std::move(notif)});
}

std::shared_ptr<const ClientCapabilities> McpServer::GetClientCapabilities() const {
    std::lock_guard<std::mutex> lock(client_info_mutex_);
    return client_capabilities_;
}

std::shared_ptr<const Implementation> McpServer::GetClientInfo() const {
    std::lock_guard<std::mutex> lock(client_info_mutex_);
    return client_info_;
}

std::string McpServer::GetNegotiatedProtocolVersion() const {
    return handler_->NegotiatedProtocolVersion();
}

const ServerCapabilities& McpServer::GetCapabilities() const {
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    return capabilities_;
}

bool McpServer::IsMrtrSupported() const {
    if (is_stateless_) return false;
    auto caps = GetClientCapabilities();
    return caps && caps->elicitation.has_value();
}

} // namespace mcp
