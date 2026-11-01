#include <mcp/detail/StringUtils.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <mcp/ErrorCodes.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/Methods.hpp>
#include <mcp/Log.hpp>
#include <mcp/McpError.hpp>
#include <mcp/ProtocolVersion.hpp>
#include <mcp/ext/DPoP.hpp>

#include <transport/detail/net/NetIoUtil.hpp>

#include <detail/JsonFields.hpp>

#include "McpParamHeaders.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifdef GetObject
#pragma push_macro("GetObject")
#undef GetObject
#define MCP_CPP_POP_GETOBJECT_SERVER 1
#endif
#endif

namespace mcp {

namespace {
constexpr std::chrono::seconds kStatelessTimeout(30);
constexpr std::chrono::milliseconds kSseListenerWaitStep(50);
constexpr std::chrono::milliseconds kSseListenerWaitBudget(2000);
const char* kMcpParamHeaderPrefix = "mcp-param-";
constexpr size_t kMaxStatelessInflight = 8;
constexpr char kSseSessionKey[] = "__session";
constexpr char kSseStreamKey[] = "__stream";
constexpr char kSseStreamGet[] = "get";
constexpr char kSseStreamListen[] = "listen";

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string MakeSessionId() {
    static std::atomic<uint64_t> counter{0};
    auto stamp = static_cast<uint64_t>(NowMs());
    auto seq = counter.fetch_add(1);
    std::random_device rd;
    std::mt19937_64 engine(stamp ^ seq ^ (static_cast<uint64_t>(rd()) << 32) ^ rd());
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out = "sess-";
    out.reserve(37);
    for (int i = 0; i < 16; ++i) {
        auto byte = static_cast<uint8_t>(engine() & 0xFF);
        out.push_back(kHexDigits[byte >> 4]);
        out.push_back(kHexDigits[byte & 0x0F]);
    }
    return out;
}

const std::string& MessageSessionId(const JsonRpcMessage& message) {
    static const std::string kEmpty;
    if (const auto* req = std::get_if<JsonRpcRequest>(&message))
        return req->session_id;
    if (const auto* notif = std::get_if<JsonRpcNotification>(&message))
        return notif->session_id;
    if (const auto* resp = std::get_if<JsonRpcResponse>(&message))
        return resp->session_id;
    if (const auto* err = std::get_if<JsonRpcErrorResponse>(&message))
        return err->session_id;
    return kEmpty;
}

std::string& MutableMessageSessionId(JsonRpcMessage& message) {
    if (auto* req = std::get_if<JsonRpcRequest>(&message)) return req->session_id;
    if (auto* notif = std::get_if<JsonRpcNotification>(&message)) return notif->session_id;
    if (auto* resp = std::get_if<JsonRpcResponse>(&message)) return resp->session_id;
    return std::get<JsonRpcErrorResponse>(message).session_id;
}

struct StatelessInflightGuard {
    explicit StatelessInflightGuard(std::atomic<size_t>& counter)
        : counter(counter) {}
    ~StatelessInflightGuard() { counter.fetch_sub(1); }
private:
    std::atomic<size_t>& counter;
};

std::string SseEscapeData(std::string_view data) {
    std::string out;
    out.reserve(data.size());
    for (char c : data) {
        if (c == '\n') out += "\ndata: ";
        else out += c;
    }
    return out;
}

std::optional<std::string> RequestStreamTokenFromObject(const JsonValue& obj) {
    auto* pt = obj.Find(detail::kProgressToken);
    if (!pt) return std::nullopt;
    if (pt->IsString()) return pt->GetString();
    if (pt->IsInt()) return std::to_string(pt->GetInt());
    return std::nullopt;
}

std::optional<int> MapRequestErrorHttpStatus(int code) {
    switch (code) {
        case static_cast<int>(McpErrorCode::ParseError):
        case static_cast<int>(McpErrorCode::InvalidRequest):
        case static_cast<int>(McpErrorCode::InvalidParams):
        case static_cast<int>(McpErrorCode::HeaderMismatch):
        case static_cast<int>(McpErrorCode::MissingRequiredClientCapability):
        case static_cast<int>(McpErrorCode::UnsupportedProtocolVersion):
            return 400;
        case static_cast<int>(McpErrorCode::MethodNotFound):
            return 404;
        default:
            return std::nullopt;
    }
}

std::optional<RequestId> ProbeRequestId(std::string_view body) {
    try {
        JsonValue parsed = JsonValue::Parse(body);
        auto* id = parsed.Find(detail::kId);
        if (!id || id->IsNull()) return std::nullopt;
        if (id->IsInt()) return RequestId{id->GetInt()};
        if (id->IsString()) return RequestId{id->GetString()};
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<RequestId> MessageRequestId(const JsonRpcMessage& msg) {
    if (auto* req = std::get_if<JsonRpcRequest>(&msg)) return req->id;
    return std::nullopt;
}

bool IsRemovedStatelessMethod(std::string_view method) {
    return method == methods::kInitialize || method == methods::kPing ||
           method == methods::kSetLoggingLevel ||
           method == methods::kSubscribeResource ||
           method == methods::kUnsubscribeResource;
}

std::optional<std::string> DeclaredMetaVersion(const JsonRpcRequest& req) {
    if (req.meta && req.meta->IsObject()) {
        auto* pv = req.meta->Find(detail::kMetaProtocolVersionKey);
        if (pv && pv->IsString() && !pv->GetString().empty())
            return pv->GetString();
    }
    return std::nullopt;
}

bool HasRequiredStatelessMeta(const JsonRpcRequest& req) {
    if (!req.meta || !req.meta->IsObject()) return false;
    auto* pv = req.meta->Find(detail::kMetaProtocolVersionKey);
    if (!pv || !pv->IsString() || pv->GetString().empty()) return false;
    auto* cc = req.meta->Find(detail::kMetaClientCapabilitiesKey);
    if (!cc || !cc->IsObject()) return false;
    return true;
}

constexpr const char* kWellKnownMetadataPath = "/.well-known/oauth-protected-resource";
constexpr size_t kBearerSchemeLength = 7;

bool IsBearerAuthorization(const std::string& value) {
    static constexpr const char* kScheme = "bearer ";
    if (value.size() < kBearerSchemeLength) return false;
    for (size_t i = 0; i < kBearerSchemeLength; ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) != kScheme[i]) {
            return false;
        }
    }
    return true;
}

std::string JoinWithSpaces(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) {
        if (!out.empty()) out += ' ';
        out += item;
    }
    return out;
}

std::string MetadataPathFromUrl(const std::string& url) {
    auto scheme_end = url.find("://");
    auto path_start = scheme_end == std::string::npos
        ? url.find('/')
        : url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) return kWellKnownMetadataPath;
    auto query = url.find('?', path_start);
    auto path = query == std::string::npos
        ? url.substr(path_start)
        : url.substr(path_start, query - path_start);
    return path.empty() ? std::string(kWellKnownMetadataPath) : path;
}

std::string NormalizeResourceUrl(std::string_view url) {
    std::string out(url);
    if (auto hash = out.find('#'); hash != std::string::npos) out.resize(hash);
    if (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

std::string ResourceUrlFromMetadataUrl(const std::string& metadata_url) {
    auto base = metadata_url;
    auto query = base.find('?');
    if (query != std::string::npos) base.resize(query);
    const std::string suffix = kWellKnownMetadataPath;
    if (base.size() > suffix.size() &&
        base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
        base.resize(base.size() - suffix.size());
    }
    return base;
}

void RespondUnauthorized(HttpResponse& resp, const std::string& metadata_url,
                         bool invalid_token,
                         const std::vector<std::string>& scopes) {
    std::string challenge =
        "Bearer resource_metadata=\"" + metadata_url + "\"";
    if (invalid_token) challenge += ", error=\"invalid_token\"";
    if (!scopes.empty()) challenge += ", scope=\"" + JoinWithSpaces(scopes) + "\"";
    resp.status_code = 401;
    resp.status_text = "Unauthorized";
    resp.headers["www-authenticate"] = std::move(challenge);
    resp.body =
        R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"unauthorized"}})";
    resp.headers["content-type"] = "application/json";
}

void RespondInsufficientScope(HttpResponse& resp,
                              const std::vector<std::string>& required_scopes) {
    resp.status_code = 403;
    resp.status_text = "Forbidden";
    resp.headers["www-authenticate"] =
        "Bearer error=\"insufficient_scope\", scope=\"" +
        JoinWithSpaces(required_scopes) + "\"";
    resp.body =
        R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"insufficient_scope"}})";
    resp.headers["content-type"] = "application/json";
}

constexpr size_t kDPoPSchemeLength = 5;

bool IsDPoPAuthorization(const std::string& value) {
    static constexpr const char* kScheme = "dpop ";
    if (value.size() < kDPoPSchemeLength) return false;
    for (size_t i = 0; i < kDPoPSchemeLength; ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) != kScheme[i]) {
            return false;
        }
    }
    return true;
}

ext::dpop::JtiReplayCache& DPoPReplayCache() {
    static ext::dpop::JtiReplayCache cache(300);
    return cache;
}

std::mutex& DPoPServerNonceMutex() {
    static std::mutex mutex;
    return mutex;
}

std::string& DPoPServerNonceValue() {
    static std::string nonce;
    return nonce;
}

std::string DPoPCurrentServerNonce() {
    std::lock_guard<std::mutex> lock(DPoPServerNonceMutex());
    std::string& nonce = DPoPServerNonceValue();
    if (nonce.empty()) nonce = ext::dpop::GenerateNonce();
    return nonce;
}

void DPoPFailUnauthorized(HttpResponse& resp, const std::string& metadata_url,
                          const std::vector<std::string>& scopes) {
    RespondUnauthorized(resp, metadata_url, true, scopes);
    resp.headers["dpop-nonce"] = DPoPCurrentServerNonce();
}

void RespondStatelessError(HttpResponse& resp, int status, const char* status_text,
                           McpErrorCode code, const std::string& message,
                           std::optional<RequestId> id, std::optional<JsonValue> data) {
    resp.status_code = status;
    resp.status_text = status_text;
    JsonValue::Object err_obj;
    err_obj["jsonrpc"] = JsonValue("2.0");
    if (id) err_obj["id"] = RequestIdToJson(*id);
    else err_obj["id"] = JsonValue(nullptr);
    JsonValue::Object err_err;
    err_err["code"] = JsonValue(static_cast<int64_t>(code));
    err_err["message"] = JsonValue(message);
    if (data) err_err["data"] = std::move(*data);
    err_obj["error"] = JsonValue(std::move(err_err));
    resp.body = JsonValue(std::move(err_obj)).Dump();
    resp.headers["content-type"] = "application/json";
}

void RespondHeaderMismatch(HttpResponse& resp, const std::string& message,
                           std::optional<RequestId> id) {
    RespondStatelessError(resp, 400, "Bad Request", McpErrorCode::HeaderMismatch,
                          message, std::move(id), std::nullopt);
}

bool IsTaskMethod(std::string_view method) {
    return method == methods::kGetTask || method == methods::kUpdateTask ||
           method == methods::kCancelTask;
}

bool IsTaskStreamNotification(std::string_view method) {
    return method == notifications::kTaskStatus ||
           method == notifications::kTaskWorking ||
           method == notifications::kTaskCompleted ||
           method == notifications::kTaskFailed ||
           method == notifications::kTaskCancelled ||
           method == notifications::kTaskInputRequired;
}

std::optional<std::string> UpdatedResourceUri(const JsonRpcNotification& notif) {
    if (!notif.params || !notif.params->IsObject()) return std::nullopt;
    auto* uri = notif.params->Find(detail::kUri);
    if (!uri || !uri->IsString()) return std::nullopt;
    return uri->GetString();
}

bool RequiresMcpNameHeader(std::string_view method) {
    return method == methods::kCallTool || method == methods::kReadResource ||
           method == methods::kGetPrompt || IsTaskMethod(method);
}

bool IsSupportedProtocolVersion(std::string_view version) {
    for (auto known : kProtocolVersions) {
        if (version == known) return true;
    }
    return false;
}

void RespondUnsupportedVersion(HttpResponse& resp, const std::string& version,
                               std::optional<RequestId> id) {
    JsonValue supported(JsonValue::array_tag);
    for (auto known : kProtocolVersions) supported.PushBack(JsonValue(std::string(known)));
    JsonValue data(JsonValue::object_tag);
    data["supported"] = std::move(supported);
    data["requested"] = JsonValue(version);
    RespondStatelessError(resp, 400, "Bad Request",
                          McpErrorCode::UnsupportedProtocolVersion,
                          "Unsupported protocol version '" + version + "'",
                          std::move(id), std::move(data));
}

std::string NormalizeEtagValue(std::string_view v) {
    size_t b = 0;
    while (b < v.size() && (v[b] == ' ' || v[b] == '\t')) ++b;
    size_t e = v.size();
    while (e > b && (v[e - 1] == ' ' || v[e - 1] == '\t')) --e;
    std::string_view s = v.substr(b, e - b);
    if (s.size() >= 2 && (s[0] == 'W' || s[0] == 'w') && s[1] == '/') s.remove_prefix(2);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return std::string(s);
}

std::optional<std::string> ResultEtagValue(const JsonRpcMessage& message) {
    const auto* r = std::get_if<JsonRpcResponse>(&message);
    if (!r || !r->result.IsObject()) return std::nullopt;
    auto* et = r->result.Find("etag");
    if (!et || !et->IsString() || et->GetString().empty()) return std::nullopt;
    return et->GetString();
}
} // namespace

StreamableHttpServerTransport::ListenFilter
StreamableHttpServerTransport::ParseListenFilter(const JsonRpcRequest& req) {
    ListenFilter filter;
    if (!req.params || !req.params->IsObject()) return filter;
    auto* section = req.params->Find(detail::kNotifications);
    if (!section || !section->IsObject()) return filter;
    auto* tools = section->Find("toolsListChanged");
    filter.tools = tools && tools->IsBool() && tools->GetBool();
    auto* prompts = section->Find("promptsListChanged");
    filter.prompts = prompts && prompts->IsBool() && prompts->GetBool();
    auto* resources = section->Find("resourcesListChanged");
    filter.resources = resources && resources->IsBool() && resources->GetBool();
    auto* subs = section->Find("resourceSubscriptions");
    if (subs && subs->IsArray()) {
        for (const auto& entry : subs->GetArray()) {
            if (entry.IsString()) filter.resource_uris.push_back(entry.GetString());
        }
    }
    return filter;
}

std::string StreamableHttpServerTransport::EncodeListenTag(
    const ListenFilter& filter, std::string_view session_id) {
    JsonValue::Object tag;
    tag[kSseStreamKey] = JsonValue(std::string(kSseStreamListen));
    if (!session_id.empty()) tag[kSseSessionKey] = JsonValue(std::string(session_id));
    tag["toolsListChanged"] = JsonValue(filter.tools);
    tag["promptsListChanged"] = JsonValue(filter.prompts);
    tag["resourcesListChanged"] = JsonValue(filter.resources);
    if (!filter.resource_uris.empty()) {
        JsonValue::Array uris;
        for (const auto& uri : filter.resource_uris)
            uris.push_back(JsonValue(uri));
        tag["resourceSubscriptions"] = JsonValue(std::move(uris));
    }
    return JsonValue(std::move(tag)).Dump();
}

std::string StreamableHttpServerTransport::EncodeSseStreamTag(
    std::string_view session_id) {
    JsonValue::Object tag;
    tag[kSseStreamKey] = JsonValue(std::string(kSseStreamGet));
    if (!session_id.empty()) tag[kSseSessionKey] = JsonValue(std::string(session_id));
    return JsonValue(std::move(tag)).Dump();
}

bool StreamableHttpServerTransport::TagMatchesSession(
    const std::string& tag, std::string_view session_id) {
    if (session_id.empty()) return true;
    if (tag.empty()) return false;
    JsonValue tag_doc;
    try {
        tag_doc = JsonValue::Parse(tag);
    } catch (...) {
        return false;
    }
    if (!tag_doc.IsObject()) return false;
    auto* value = tag_doc.Find(kSseSessionKey);
    return value && value->IsString() && value->GetString() == session_id;
}

bool StreamableHttpServerTransport::IsListenFilteredNotification(
    const std::string& method) {
    return method == notifications::kToolListChanged ||
           method == notifications::kPromptListChanged ||
           method == notifications::kResourceListChanged ||
           method == notifications::kResourceUpdated;
}

bool StreamableHttpServerTransport::ListenTagAccepts(
    const std::string& tag, const std::string& method,
    const std::optional<std::string>& resource_uri) {
    if (tag.empty()) return true;
    JsonValue tag_doc;
    try {
        tag_doc = JsonValue::Parse(tag);
    } catch (...) {
        return true;
    }
    if (!tag_doc.IsObject()) return true;
    auto* stream = tag_doc.Find(kSseStreamKey);
    if (stream && stream->IsString() && stream->GetString() == kSseStreamGet)
        return true;
    auto flag = [&tag_doc](const char* key) {
        auto* value = tag_doc.Find(key);
        return value && value->IsBool() && value->GetBool();
    };
    if (method == notifications::kToolListChanged)
        return flag("toolsListChanged");
    if (method == notifications::kPromptListChanged)
        return flag("promptsListChanged");
    if (method == notifications::kResourceListChanged)
        return flag("resourcesListChanged");
    if (method == notifications::kResourceUpdated) {
        if (!resource_uri) return false;
        auto* subs = tag_doc.Find("resourceSubscriptions");
        if (!subs || !subs->IsArray()) return false;
        for (const auto& entry : subs->GetArray()) {
            if (entry.IsString() && entry.GetString() == *resource_uri)
                return true;
        }
        return false;
    }
    return true;
}

StreamableHttpServerTransport::StreamableHttpServerTransport(
    StreamableHttpServerOptions options)
    : TransportBase()
    , options_(std::move(options))
    , http_server_(std::make_unique<HttpServer>(options_.port,
          [keep_alive_ms = options_.sse_keep_alive_ms, host = options_.host,
            max_body = options_.max_request_body_bytes,
            allowed_hosts = options_.allowed_hosts,
            allowed_origins = options_.allowed_origins] {
              HttpServerOptions http_options;
              http_options.sse_keep_alive_ms = keep_alive_ms;
              http_options.bind_host = host;
              http_options.max_request_body_bytes = max_body;
              http_options.allowed_hosts = allowed_hosts;
              http_options.allowed_origins = allowed_origins;
              return http_options;
          }()))
    , event_store_(options_.event_store
        ? options_.event_store
        : std::make_shared<EventStore>())
    , session_store_(options_.session_store)
{
    if (options_.bearer_auth && !options_.bearer_auth->verify) {
        throw McpError(McpErrorCode::InvalidRequest,
            "bearer_auth.verify is required when bearer auth is enabled");
    }

    http_server_->SetHandler("POST", options_.endpoint,
        [this](const HttpRequest& req, HttpResponse& resp) {
            HandlePost(req, resp);
        });

    if (options_.enable_legacy_sse) {
        http_server_->SetHandler("GET", options_.endpoint,
            [this](const HttpRequest& req, HttpResponse& resp) {
                HandleGet(req, resp);
            });
    }

    http_server_->SetHandler("DELETE", options_.endpoint,
        [this](const HttpRequest& req, HttpResponse& resp) {
            if (options_.stateless) {
                resp.status_code = 405;
                resp.status_text = "Method Not Allowed";
                resp.body = R"({"jsonrpc":"2.0","error":{"code":-32601,"message":"method not found"}})";
                resp.headers["content-type"] = "application/json";
                return;
            }
            auto session_id = GetMcpHeader(req, "mcp-session-id");
            if (session_id && !session_id->empty() && !HasSession(*session_id)) {
                resp.status_code = 404;
                resp.status_text = "Not Found";
                resp.body = R"({"jsonrpc":"2.0","error":{"code":-32009,"message":"Session expired"}})";
                resp.headers["content-type"] = "application/json";
                return;
            }
            if (session_id && !session_id->empty()) EraseSession(*session_id);
            resp.status_code = 200;
            resp.status_text = "OK";
            resp.body = "{}";
            resp.headers["content-type"] = "application/json";
        });

    if (options_.bearer_auth && options_.bearer_auth->serve_metadata_endpoint) {
        http_server_->SetHandler("GET",
            MetadataPathFromUrl(options_.bearer_auth->resource_metadata_url),
            [this](const HttpRequest&, HttpResponse& resp) {
                HandleMetadataRequest(resp);
            });
    }

    if (options_.enable_server_card) {
        http_server_->SetHandler("GET", "/server-card",
            [this](const HttpRequest&, HttpResponse& resp) {
                HandleServerCardRequest(resp);
            });
    }
}

StreamableHttpServerTransport::~StreamableHttpServerTransport() {
    Close();
}

void StreamableHttpServerTransport::Start() {
    if (http_server_) http_server_->Start();
}

void StreamableHttpServerTransport::Close() {
    if (http_server_) http_server_->Stop();
    std::vector<std::string> session_ids;
    {
        std::lock_guard<std::mutex> lock(session_state_mutex_);
        for (const auto& [id, state] : sessions_) session_ids.push_back(id);
        sessions_.clear();
    }
    if (event_store_) {
        for (const auto& id : session_ids) event_store_->Clear(id);
    }
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_responses_.clear();
    }
    if (channel_) channel_->Close();
    SetDisconnected();
}

bool StreamableHttpServerTransport::AuthorizeRequest(
    const HttpRequest& req, HttpResponse& resp)
{
    if (!options_.bearer_auth) return true;
    const auto& config = *options_.bearer_auth;

    if (config.enable_dpop) {
        auto dpop_auth = GetMcpHeader(req, "authorization");
        if (!dpop_auth || !IsDPoPAuthorization(*dpop_auth)) {
            DPoPFailUnauthorized(resp, config.resource_metadata_url,
                                 config.scopes_supported);
            return false;
        }
        auto token = dpop_auth->substr(kDPoPSchemeLength);
        detail::net::TrimInPlace(token);
        if (token.empty()) {
            DPoPFailUnauthorized(resp, config.resource_metadata_url,
                                 config.scopes_supported);
            return false;
        }
        auto result = config.verify(token);
        if (!result.ok) {
            DPoPFailUnauthorized(resp, config.resource_metadata_url,
                                 config.scopes_supported);
            return false;
        }
        auto proof = GetMcpHeader(req, "dpop");
        bool proof_ok = false;
        if (proof && !proof->empty()) {
            auto host = GetMcpHeader(req, "host");
            std::string req_path = req.path;
            if (auto query = req_path.find('?'); query != std::string::npos)
                req_path.resize(query);
            if (host && !host->empty() && !req_path.empty()) {
                ext::dpop::DPoPVerifyOptions verify_options;
                verify_options.htm = req.method;
                verify_options.htu = "http://" + *host + req_path;
                verify_options.alternate_htu = "https://" + *host + req_path;
                verify_options.access_token = token;
                verify_options.expected_nonce = DPoPCurrentServerNonce();
                try {
                    proof_ok = ext::dpop::VerifyProof(*proof, verify_options,
                                                      DPoPReplayCache());
                } catch (...) {
                    proof_ok = false;
                }
            }
        }
        if (!proof_ok) {
            DPoPFailUnauthorized(resp, config.resource_metadata_url,
                                 config.scopes_supported);
            return false;
        }
        if (config.validate_token_resource && !config.expected_resource.empty()) {
            bool audience_ok = result.resource && !result.resource->empty() &&
                NormalizeResourceUrl(*result.resource) ==
                    NormalizeResourceUrl(config.expected_resource);
            if (!audience_ok) {
                DPoPFailUnauthorized(resp, config.resource_metadata_url,
                                     config.scopes_supported);
                return false;
            }
        }
        for (const auto& required : config.required_scopes) {
            if (std::find(result.scopes.begin(), result.scopes.end(), required)
                == result.scopes.end()) {
                RespondInsufficientScope(resp, config.required_scopes);
                resp.headers["dpop-nonce"] = DPoPCurrentServerNonce();
                return false;
            }
        }
        resp.headers["dpop-nonce"] = DPoPCurrentServerNonce();
        return true;
    }

    auto authorization = GetMcpHeader(req, "authorization");
    if (!authorization || !IsBearerAuthorization(*authorization)) {
        RespondUnauthorized(resp, config.resource_metadata_url, false,
                            config.scopes_supported);
        return false;
    }
    auto token = authorization->substr(kBearerSchemeLength);
    detail::net::TrimInPlace(token);

    auto result = config.verify(token);
    if (!result.ok) {
        RespondUnauthorized(resp, config.resource_metadata_url, true,
                            config.scopes_supported);
        return false;
    }
    if (config.validate_token_resource && !config.expected_resource.empty()) {
        bool audience_ok = result.resource && !result.resource->empty() &&
            NormalizeResourceUrl(*result.resource) ==
                NormalizeResourceUrl(config.expected_resource);
        if (!audience_ok) {
            RespondUnauthorized(resp, config.resource_metadata_url, true,
                                config.scopes_supported);
            return false;
        }
    }
    for (const auto& required : config.required_scopes) {
        if (std::find(result.scopes.begin(), result.scopes.end(), required)
            == result.scopes.end()) {
            RespondInsufficientScope(resp, config.required_scopes);
            return false;
        }
    }
    return true;
}

void StreamableHttpServerTransport::HandleMetadataRequest(HttpResponse& resp) {
    const auto& config = *options_.bearer_auth;
    JsonValue::Object doc;
    doc["resource"] =
        JsonValue(ResourceUrlFromMetadataUrl(config.resource_metadata_url));
    if (!config.authorization_servers.empty()) {
        JsonValue::Array servers;
        for (const auto& server : config.authorization_servers)
            servers.push_back(JsonValue(server));
        doc["authorization_servers"] = JsonValue(std::move(servers));
    }
    if (!config.scopes_supported.empty()) {
        JsonValue::Array scopes;
        for (const auto& scope : config.scopes_supported)
            scopes.push_back(JsonValue(scope));
        doc["scopes_supported"] = JsonValue(std::move(scopes));
    }
    JsonValue::Array methods;
    methods.push_back(JsonValue("header"));
    doc["bearer_methods_supported"] = JsonValue(std::move(methods));
    resp.status_code = 200;
    resp.status_text = "OK";
    resp.body = JsonValue(std::move(doc)).Dump();
    resp.headers["content-type"] = "application/json";
}

ext::ServerCard StreamableHttpServerTransport::BuildDefaultServerCard() const {
    ext::ServerCard card;
    card.name = options_.server_name;
    card.version = options_.server_version;
    ext::ServerCardEndpoint endpoint;
    endpoint.url = "http://" + (options_.host.empty() ? std::string("localhost") : options_.host) +
        ":" + std::to_string(options_.port) + options_.endpoint;
    card.endpoints.push_back(std::move(endpoint));
    for (auto version : kProtocolVersions) {
        card.supported_protocol_versions.emplace_back(version);
    }
    return card;
}

void StreamableHttpServerTransport::HandleServerCardRequest(HttpResponse& resp) {
    try {
        ext::ServerCard card = options_.server_card.value_or(BuildDefaultServerCard());
        resp.status_code = 200;
        resp.status_text = "OK";
        resp.body = ext::SerializeCard(card).Dump();
    } catch (const McpError& e) {
        resp.status_code = 400;
        resp.status_text = "Bad Request";
        JsonValue::Object err;
        err["code"] = JsonValue(static_cast<int64_t>(e.Code()));
        err["message"] = JsonValue(std::string(e.ErrorMessage()));
        resp.body = JsonValue(std::move(err)).Dump();
    }
    resp.headers["content-type"] = "application/json";
    resp.headers["access-control-allow-origin"] = "*";
    resp.headers["cache-control"] = "public,max-age=60";
}

std::string StreamableHttpServerTransport::PendingKey(
    std::string_view session_id, const std::string& request_id) {
    return std::to_string(session_id.size()) + ":" +
           std::string(session_id) + request_id;
}

bool StreamableHttpServerTransport::ResolveSession(
    const HttpRequest& req, bool is_initialize,
    const std::string& protocol_version,
    std::string& session_id, HttpResponse& resp)
{
    if (options_.stateless) {
        session_id.clear();
        return true;
    }
    auto header = GetMcpHeader(req, "mcp-session-id");
    if (header && !header->empty()) {
        if (!TouchSession(*header, is_initialize, protocol_version)) {
            resp.status_code = 404;
            resp.status_text = "Not Found";
            resp.body = R"({"jsonrpc":"2.0","error":{"code":-32009,"message":"Session expired"}})";
            resp.headers["content-type"] = "application/json";
            return false;
        }
        session_id = *header;
        return true;
    }
    if (!is_initialize) {
        session_id.clear();
        return true;
    }
    std::string version = protocol_version.empty()
        ? std::string(kDefaultNegotiatedProtocolVersion)
        : protocol_version;
    auto candidate = MakeSessionId();
    SessionRecord record;
    record.protocol_version = version;
    record.created_at_ms = NowMs();
    record.last_seen_ms = record.created_at_ms;
    if (session_store_ && !session_store_->TrySave(candidate, record)) {
        resp.status_code = 503;
        resp.status_text = "Service Unavailable";
        resp.body = R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"session store is full"}})";
        resp.headers["content-type"] = "application/json";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(session_state_mutex_);
        auto now = NowMs();
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (now - it->second.last_seen_ms > detail::kSessionIdleTimeoutMs)
                it = sessions_.erase(it);
            else
                ++it;
        }
        if (sessions_.size() >= detail::kMaxHttpSessions) {
            if (session_store_) session_store_->Remove(candidate);
            resp.status_code = 503;
            resp.status_text = "Service Unavailable";
            resp.body = R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"session capacity exceeded"}})";
            resp.headers["content-type"] = "application/json";
            return false;
        }
        sessions_[candidate] = SessionState{version, record.last_seen_ms};
    }
    session_id = candidate;
    return true;
}

bool StreamableHttpServerTransport::TouchSession(
    const std::string& session_id, bool is_initialize,
    const std::string& protocol_version)
{
    auto now = NowMs();
    if (session_store_) {
        auto record = session_store_->Load(session_id);
        if (!record) {
            std::lock_guard<std::mutex> lock(session_state_mutex_);
            sessions_.erase(session_id);
            return false;
        }
        std::lock_guard<std::mutex> lock(session_state_mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            SessionState state;
            state.protocol_version = record->protocol_version.empty()
                ? std::string(kDefaultNegotiatedProtocolVersion)
                : record->protocol_version;
            state.last_seen_ms = now;
            sessions_[session_id] = std::move(state);
        } else {
            it->second.last_seen_ms = now;
            if (is_initialize && !protocol_version.empty())
                it->second.protocol_version = protocol_version;
        }
        return true;
    }
    std::lock_guard<std::mutex> lock(session_state_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return false;
    if (now - it->second.last_seen_ms > detail::kSessionIdleTimeoutMs) {
        sessions_.erase(it);
        return false;
    }
    it->second.last_seen_ms = now;
    if (is_initialize && !protocol_version.empty())
        it->second.protocol_version = protocol_version;
    return true;
}

std::string StreamableHttpServerTransport::SessionProtocolVersion(
    const std::string& session_id) const {
    if (session_id.empty()) return std::string(kDefaultNegotiatedProtocolVersion);
    std::lock_guard<std::mutex> lock(session_state_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end() || it->second.protocol_version.empty())
        return std::string(kDefaultNegotiatedProtocolVersion);
    return it->second.protocol_version;
}

bool StreamableHttpServerTransport::HasSession(const std::string& session_id) {    if (session_store_ && session_store_->Load(session_id)) return true;
    std::lock_guard<std::mutex> lock(session_state_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return false;
    if (!session_store_ &&
        NowMs() - it->second.last_seen_ms > detail::kSessionIdleTimeoutMs) {
        sessions_.erase(it);
        return false;
    }
    return true;
}

void StreamableHttpServerTransport::EraseSession(const std::string& session_id) {
    {
        std::lock_guard<std::mutex> lock(session_state_mutex_);
        sessions_.erase(session_id);
    }
    if (session_store_) session_store_->Remove(session_id);
    if (event_store_) event_store_->Clear(session_id);

    const auto prefix = PendingKey(session_id, "");
    std::vector<PendingResponse> abandoned;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (auto it = pending_responses_.begin(); it != pending_responses_.end();) {
            if (it->first.compare(0, prefix.size(), prefix) == 0) {
                abandoned.push_back(std::move(it->second));
                it = pending_responses_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = request_stream_frames_.begin(); it != request_stream_frames_.end();) {
            if (it->first.compare(0, prefix.size(), prefix) == 0)
                it = request_stream_frames_.erase(it);
            else
                ++it;
        }
        for (auto it = hold_open_requests_.begin(); it != hold_open_requests_.end();) {
            if (it->compare(0, prefix.size(), prefix) == 0)
                it = hold_open_requests_.erase(it);
            else
                ++it;
        }
        for (auto it = listen_filters_.begin(); it != listen_filters_.end();) {
            if (it->first.compare(0, prefix.size(), prefix) == 0)
                it = listen_filters_.erase(it);
            else
                ++it;
        }
        for (auto it = progress_token_routes_.begin();
             it != progress_token_routes_.end();) {
            if (it->second.compare(0, prefix.size(), prefix) == 0)
                it = progress_token_routes_.erase(it);
            else
                ++it;
        }
    }
    for (auto& entry : abandoned) {
        JsonRpcErrorResponse error;
        error.id = entry.request_id;
        error.error.code = McpErrorCode::SessionExpired;
        error.error.message = "session closed";
        error.session_id = session_id;
        entry.promise->set_value(JsonRpcMessage{std::move(error)});
    }
}


bool StreamableHttpServerTransport::ValidateMcpHeaders(
    const std::string& method_header,
    const std::string& name_header,
    const JsonValue& body,
    std::string& error_out)
{
    auto* method_val = body.Find("method");
    std::string body_method = method_val && method_val->IsString() ? method_val->GetString() : "";
    if (!method_header.empty() && !body_method.empty() &&
        method_header != body_method)
    {
        error_out = "Mcp-Method header '" + method_header +
                    "' does not match body method '" + body_method + "'";
        return false;
    }

    if (!name_header.empty()) {
        std::string effective_name = name_header;
        if (auto decoded = http_detail::DecodeHeaderValue(name_header);
            decoded.has_value() && decoded->IsString()) {
            effective_name = decoded->GetString();
        }
        auto* params = body.Find("params");
        const JsonValue* params_obj =
            (params && params->IsObject()) ? params : nullptr;
        if (IsTaskMethod(body_method)) {
            const JsonValue* task_id =
                params_obj ? params_obj->Find(detail::kTaskId) : nullptr;
            if (!task_id || !task_id->IsString() || task_id->GetString().empty()) {
                error_out = "tasks request missing valid taskId";
                return false;
            }
            if (task_id->GetString() != effective_name) {
                error_out = "Mcp-Name header '" + effective_name +
                            "' does not match body params taskId '" +
                            task_id->GetString() + "'";
                return false;
            }
            return true;
        }
        std::string body_name;
        if (params && params->IsObject()) {
            if (auto* n = params->Find("name"); n && n->IsString()) body_name = n->GetString();
            if (body_name.empty())
                if (auto* u = params->Find("uri"); u && u->IsString()) body_name = u->GetString();
        }
        if (!body_name.empty() && effective_name != body_name) {
            error_out = "Mcp-Name header '" + effective_name +
                        "' does not match body params name '" + body_name + "'";
            return false;
        }
    }

    return true;
}

bool StreamableHttpServerTransport::ValidateParamHeaders(
    const HttpRequest& req, const JsonRpcRequest& request, std::string& error_out)
{
    if (!options_.resolve_param_annotations) return true;
    if (request.method != methods::kCallTool) return true;
    if (!request.params || !request.params->IsObject()) return true;
    const JsonValue* tool_name = request.params->Find("name");
    if (!tool_name || !tool_name->IsString()) return true;

    const auto annotations =
        options_.resolve_param_annotations(request.method, tool_name->GetString());
    if (annotations.empty()) return true;

    const JsonValue* arguments = request.params->Find("arguments");

    for (const auto& annotation : annotations) {
        const std::string expected_key =
            std::string(kMcpParamHeaderPrefix) + detail::ToLower(annotation.header_name);
        const std::string* raw_value = nullptr;
        for (const auto& [key, value] : req.headers) {
            if (detail::ToLower(key) == expected_key) {
                raw_value = &value;
                break;
            }
        }

        std::optional<JsonValue> body_value;
        if (arguments && arguments->IsObject()) {
            body_value = http_detail::ValueAtPath(*arguments, annotation.property_path);
        }
        const bool has_body_value = body_value.has_value() && !body_value->IsNull();

        if (!has_body_value) {
            if (raw_value) {
                error_out = "Mcp-Param-" + annotation.header_name +
                            " header present but the parameter is absent from arguments";
                return false;
            }
            continue;
        }

        if (!raw_value) {
            error_out = "missing Mcp-Param-" + annotation.header_name +
                        " header for tools/call argument";
            return false;
        }

        auto decoded = http_detail::DecodeHeaderValue(*raw_value);
        if (!decoded.has_value()) {
            error_out = "Mcp-Param-" + annotation.header_name + " header value is malformed";
            return false;
        }
        if (!http_detail::HeaderValueMatchesBody(*decoded, *body_value)) {
            error_out = "Mcp-Param-" + annotation.header_name +
                        " header value does not match the request body";
            return false;
        }
    }
    return true;
}

void StreamableHttpServerTransport::HandlePost(
    const HttpRequest& req, HttpResponse& resp)
{
    if (!AuthorizeRequest(req, resp)) return;

    auto proto_ver = GetMcpHeader(req, "mcp-protocol-version");
    if (proto_ver.has_value() && !proto_ver->empty() &&
        !IsSupportedProtocolVersion(*proto_ver)) {
        RespondUnsupportedVersion(resp, *proto_ver, ProbeRequestId(req.body));
        return;
    }
    auto mcp_method = GetMcpHeader(req, "mcp-method");
    auto mcp_name = GetMcpHeader(req, "mcp-name");

    JsonRpcMessage msg;
    try {
        std::size_t body_limit =
            options_.max_request_body_bytes.value_or(detail::kMaxHttpBodyBytes);
        if (req.body.size() > body_limit) {
            resp.status_code = 413;
            resp.status_text = "Payload Too Large";
            resp.body = R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"Message size exceeds maximum allowed size"}})";
            resp.headers["content-type"] = "application/json";
            return;
        }
        msg = DeserializeMessage(req.body);
    } catch (...) {
        MCP_LOG(Warning, "request body parse failed");
        resp.status_code = 400;
        resp.status_text = "Bad Request";
        resp.body = R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"Parse error"}})";
        resp.headers["content-type"] = "application/json";
        return;
    }

    JsonValue body_jv;
    if (auto* req_ptr = std::get_if<JsonRpcRequest>(&msg)) {
        JsonValue::Object body_obj;
        body_obj["method"] = JsonValue(req_ptr->method);
        if (req_ptr->params) body_obj["params"] = *req_ptr->params;
        body_jv = JsonValue(std::move(body_obj));
    } else if (auto* notif = std::get_if<JsonRpcNotification>(&msg)) {
        JsonValue::Object body_obj;
        body_obj["method"] = JsonValue(notif->method);
        if (notif->params) body_obj["params"] = *notif->params;
        body_jv = JsonValue(std::move(body_obj));
    }

    if (proto_ver.has_value() && IsModernProtocolVersion(*proto_ver)) {
        std::string body_method;
        if (auto* m = body_jv.Find("method"); m && m->IsString()) body_method = m->GetString();
        if (!mcp_method.has_value()) {
            RespondHeaderMismatch(resp, "missing required Mcp-Method header",
                                  MessageRequestId(msg));
            return;
        }
        if (RequiresMcpNameHeader(body_method) && !mcp_name.has_value()) {
            RespondHeaderMismatch(resp, "missing required Mcp-Name header",
                                  MessageRequestId(msg));
            return;
        }
    }

    std::string header_error;
    if (!ValidateMcpHeaders(mcp_method.value_or(""), mcp_name.value_or(""), body_jv, header_error)) {
        RespondHeaderMismatch(resp, header_error, MessageRequestId(msg));
        return;
    }

    bool needs_response = IsRequest(msg);
    std::optional<RequestId> req_id;
    bool is_initialize = false;
    if (needs_response) {
        if (auto* r = std::get_if<JsonRpcRequest>(&msg)) {
            req_id = r->id;
            is_initialize = (r->method == methods::kInitialize);
        }
    }

    std::string requested_version;
    if (auto* init_req = std::get_if<JsonRpcRequest>(&msg)) {
        if (is_initialize && init_req->params && init_req->params->IsObject()) {
            if (auto* pv = init_req->params->Find("protocolVersion");
                pv && pv->IsString()) {
                requested_version = pv->GetString();
            }
        }
    }
    if (requested_version.empty() && proto_ver && !proto_ver->empty())
        requested_version = *proto_ver;

    std::string session_id;
    if (!ResolveSession(req, is_initialize, requested_version,
                        session_id, resp)) {
        return;
    }
    MutableMessageSessionId(msg) = session_id;
    if (is_initialize && !session_id.empty())
        resp.headers["mcp-session-id"] = session_id;

    if (options_.stateless) {
        if (auto* post_req = std::get_if<JsonRpcRequest>(&msg)) {
            bool legacy_pinned = proto_ver.has_value() && !proto_ver->empty() &&
                                 !IsModernProtocolVersion(*proto_ver);
            if (!legacy_pinned) {
                if (IsRemovedStatelessMethod(post_req->method)) {
                    bool removed = true;
                    if (post_req->method == methods::kInitialize) {
                        removed = proto_ver.has_value() && !proto_ver->empty() &&
                                  IsModernProtocolVersion(*proto_ver);
                    }
                    if (removed) {
                        RespondStatelessError(resp, 404, "Not Found",
                                              McpErrorCode::MethodNotFound,
                                              "method not found: " + post_req->method,
                                              post_req->id, std::nullopt);
                        return;
                    }
                }
                auto meta_version = DeclaredMetaVersion(*post_req);
                if (proto_ver.has_value() && !proto_ver->empty() &&
                    meta_version.has_value() && *proto_ver != *meta_version) {
                    RespondStatelessError(resp, 400, "Bad Request",
                                          McpErrorCode::HeaderMismatch,
                                          "Mcp-Protocol-Version header '" + *proto_ver +
                                          "' does not match request _meta protocolVersion '" +
                                          *meta_version + "'",
                                          post_req->id, std::nullopt);
                    return;
                }
                if (post_req->method == methods::kDiscover &&
                    !HasRequiredStatelessMeta(*post_req)) {
                    RespondStatelessError(resp, 400, "Bad Request",
                                          McpErrorCode::InvalidParams,
                                          "stateless requests must carry _meta with "
                                          "protocolVersion and clientCapabilities",
                                          post_req->id, std::nullopt);
                    return;
                }
            }
        }
    }

    std::string response_version = std::string(kDefaultNegotiatedProtocolVersion);
    if (proto_ver.has_value() && !proto_ver->empty()) {
        response_version = proto_ver.value();
    } else if (!session_id.empty()) {
        response_version = SessionProtocolVersion(session_id);
    }
    resp.headers["mcp-protocol-version"] = response_version;

    if (mcp_method.has_value()) {
        resp.headers["mcp-method"] = mcp_method.value();
    }
    if (mcp_name.has_value()) {
        resp.headers["mcp-name"] = mcp_name.value();
    }

    if (auto* req_ptr = std::get_if<JsonRpcRequest>(&msg)) {
        JsonValue::Object meta_headers_obj;
        for (const auto& [key, val] : req.headers) {
            const std::string key_lower = detail::ToLower(key);
            const std::string prefix = kMcpParamHeaderPrefix;
            if (key_lower.substr(0, prefix.size()) == prefix) {
                auto param_name = key.substr(prefix.size());
                meta_headers_obj[param_name] = JsonValue(val);
            }
        }
        if (!meta_headers_obj.empty()) {
            if (!req_ptr->meta) req_ptr->meta = JsonValue(JsonValue::object_tag);
            (*req_ptr->meta)["x-mcp-headers"] = JsonValue(std::move(meta_headers_obj));
        }

        std::string param_error;
        if (!ValidateParamHeaders(req, *req_ptr, param_error)) {
            RespondHeaderMismatch(resp, param_error, req_ptr->id);
            return;
        }
    }

    if (needs_response) {
        if (!(channel_ && channel_->IsOpen())) {
            resp.status_code = 503;
            resp.status_text = "Service Unavailable";
            resp.body = R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"server closed"}})";
            resp.headers["content-type"] = "application/json";
            return;
        }
        std::optional<StatelessInflightGuard> inflight_guard;
        if (options_.stateless) {
            if (stateless_inflight_.load() >= kMaxStatelessInflight) {
                resp.status_code = 503;
                resp.status_text = "Service Unavailable";
                resp.body = R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"server busy"}})";
                resp.headers["content-type"] = "application/json";
                return;
            }
            stateless_inflight_.fetch_add(1);
            inflight_guard.emplace(stateless_inflight_);
        }
        auto id_str = RequestIdToString(*req_id);
        const auto pending_key = PendingKey(session_id, id_str);
        std::string hold_version = response_version;
        bool hold_open = IsModernProtocolVersion(hold_version);
        std::optional<std::string> hold_token;
        if (hold_open) {
            if (auto* hold_req = std::get_if<JsonRpcRequest>(&msg))
                hold_token = RequestTokenKey(*hold_req);
        }
        bool hold_listen = false;
        ListenFilter listen_filter;
        std::string listen_tag;
        if (hold_open && options_.listen_hold_open) {
            if (auto* hold_req = std::get_if<JsonRpcRequest>(&msg)) {
                if (hold_req->method == methods::kSubscribe) {
                    hold_listen = true;
                    listen_filter = ParseListenFilter(*hold_req);
                    listen_tag = EncodeListenTag(listen_filter, session_id);
                }
            }
        }
        auto promise = std::make_shared<std::promise<JsonRpcMessage>>();
        auto future = promise->get_future();
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_responses_[pending_key] = PendingResponse{*req_id, promise};
            if (hold_open) {
                hold_open_requests_.insert(pending_key);
                if (hold_token) progress_token_routes_[*hold_token] = pending_key;
            }
            if (hold_listen) listen_filters_[pending_key] = std::move(listen_filter);
        }
        if (!channel_->TrySend(std::move(msg))) {
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                pending_responses_.erase(pending_key);
                EraseRequestStreamStateLocked(pending_key);
            }
            resp.status_code = 503;
            resp.status_text = "Service Unavailable";
            resp.body = R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"server closed"}})";
            resp.headers["content-type"] = "application/json";
            return;
        }

        auto deadline = std::chrono::steady_clock::now() + kStatelessTimeout;
        if (future.wait_until(deadline) != std::future_status::ready) {
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                pending_responses_.erase(pending_key);
                EraseRequestStreamStateLocked(pending_key);
            }
            resp.status_code = 504;
            resp.status_text = "Gateway Timeout";
            JsonValue::Object err_obj;
            err_obj["jsonrpc"] = JsonValue("2.0");
            {
                JsonValue::Object err_err;
                err_err["code"] = JsonValue(static_cast<int64_t>(-32000));
                err_err["message"] = JsonValue("Request timeout for request " + id_str);
                err_obj["error"] = JsonValue(std::move(err_err));
            }
            resp.body = JsonValue(std::move(err_obj)).Dump();
            resp.headers["content-type"] = "application/json";
            return;
        }
        auto response = future.get();
        std::vector<std::string> hold_frames;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            if (hold_open) {
                auto fit = request_stream_frames_.find(pending_key);
                if (fit != request_stream_frames_.end())
                    hold_frames = std::move(fit->second);
                EraseRequestStreamStateLocked(pending_key);
            }
        }
        if (const auto* err = std::get_if<JsonRpcErrorResponse>(&response)) {
            auto mapped = MapRequestErrorHttpStatus(static_cast<int>(err->error.code));
            if (mapped) {
                resp.status_code = *mapped;
                resp.status_text = (*mapped == 404) ? "Not Found" : "Bad Request";
                resp.body = SerializeMessage(std::move(response));
                resp.headers["content-type"] = "application/json";
                return;
            }
        }
        if (const auto* r = std::get_if<JsonRpcResponse>(&response)) {
            if (r->result.IsObject()) {
                if (auto* meta = r->result.Find("_meta"); meta && meta->IsObject()) {
                    if (auto* xhc = meta->Find("x-mcp-header"); xhc && xhc->IsObject()) {
                        for (const auto& [hk, hv] : xhc->GetObject()) {
                            resp.headers["mcp-param-" + hk] =
                                hv.IsString() ? hv.GetString() : hv.Dump();
                        }
                    }
                }
            }
        }
        if (IsEtagCacheEnabled()) {
            if (auto etag = ResultEtagValue(response)) {
                resp.headers["etag"] = *etag;
                if (auto inm = GetMcpHeader(req, "if-none-match")) {
                    if (NormalizeEtagValue(*inm) == NormalizeEtagValue(*etag)) {
                        resp.status_code = 304;
                        resp.status_text = "Not Modified";
                        resp.body.clear();
                        resp.is_sse = false;
                        return;
                    }
                }
            }
        }
        resp.status_code = 200;
        resp.status_text = "OK";
        resp.is_sse = true;
        resp.sse_close_after_write = !hold_listen;
        if (hold_listen) resp.sse_client_tag = std::move(listen_tag);
        resp.headers["content-type"] = "text/event-stream";
        resp.headers["cache-control"] = "no-cache";
        resp.headers["x-accel-buffering"] = "no";
        resp.body.clear();
        for (auto& hold_frame : hold_frames) resp.body += hold_frame;
        if (!hold_listen || !std::get_if<JsonRpcResponse>(&response)) {
            resp.body += "event: message\ndata: " +
                        SseEscapeData(SerializeMessage(std::move(response))) + "\n\n";
        }
        if (resp.body.size() > SseEventLimit()) resp.body.resize(SseEventLimit());
    } else {
        if (!(channel_ && channel_->IsOpen()) || !channel_->TrySend(std::move(msg))) {
            resp.status_code = 503;
            resp.status_text = "Service Unavailable";
            resp.body = R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"server closed"}})";
            resp.headers["content-type"] = "application/json";
            return;
        }
        resp.status_code = 202;
        resp.status_text = "Accepted";
        resp.body = "{}";
        resp.headers["content-type"] = "application/json";
    }
}

void StreamableHttpServerTransport::HandleGet(
    const HttpRequest& req, HttpResponse& resp)
{
    if (!AuthorizeRequest(req, resp)) return;

    std::string session_id;
    if (!ResolveSession(req, false, {}, session_id, resp)) return;

    auto proto_ver = GetMcpHeader(req, "mcp-protocol-version");
    if (proto_ver.has_value() && !proto_ver->empty() &&
        !IsSupportedProtocolVersion(*proto_ver)) {
        RespondUnsupportedVersion(resp, *proto_ver, std::nullopt);
        return;
    }
    std::string response_version = std::string(kDefaultNegotiatedProtocolVersion);
    if (proto_ver.has_value() && !proto_ver->empty()) {
        response_version = proto_ver.value();
    } else if (!session_id.empty()) {
        response_version = SessionProtocolVersion(session_id);
    }
    resp.headers["mcp-protocol-version"] = response_version;

    resp.is_sse = true;
    resp.headers["content-type"] = "text/event-stream";
    resp.headers["cache-control"] = "no-cache";
    if (!session_id.empty()) resp.sse_client_tag = EncodeSseStreamTag(session_id);

    resp.body = "event: endpoint\ndata: " + SseEscapeData(options_.endpoint) + "\n\n";

    if (!options_.stateless) {
        auto last_id = GetMcpHeader(req, "last-event-id");
        if (last_id && !last_id->empty()) {
            try {
                auto from = std::stoull(*last_id);
                for (const auto& [ev_id, ev_data] :
                        event_store_->GetEventsSince(session_id, from)) {
                    resp.body += "id: " + std::to_string(ev_id) + "\n" + ev_data;
                }
            } catch (const std::exception&) {
                MCP_LOG(Warning, "invalid Last-Event-ID header; ignoring");
            }
        }
    }
}

void StreamableHttpServerTransport::SendMessageAsync(JsonRpcMessage message) {
    const std::string message_session = MessageSessionId(message);

    const auto match_pending = [&](const RequestId& id) -> std::string {
        std::string id_str = RequestIdToString(id);
        auto key = PendingKey(message_session, id_str);
        if (pending_responses_.find(key) != pending_responses_.end()) return key;
        if (!message_session.empty()) return {};
        std::string matched_key;
        for (const auto& [candidate_key, entry] : pending_responses_) {
            if (candidate_key.size() < id_str.size()) continue;
            if (candidate_key.compare(candidate_key.size() - id_str.size(),
                                      id_str.size(), id_str) != 0) {
                continue;
            }
            if (!matched_key.empty()) return {};
            matched_key = candidate_key;
        }
        return matched_key;
    };

    if (auto* resp = std::get_if<JsonRpcResponse>(&message)) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto key = match_pending(resp->id);
        auto it = key.empty() ? pending_responses_.end()
                              : pending_responses_.find(key);
        if (it != pending_responses_.end()) {
            auto promise = std::move(it->second.promise);
            pending_responses_.erase(it);
            promise->set_value(std::move(message));
            return;
        }
    } else if (auto* err = std::get_if<JsonRpcErrorResponse>(&message)) {
        if (err->id) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto key = match_pending(*err->id);
            auto it = key.empty() ? pending_responses_.end()
                                  : pending_responses_.find(key);
            if (it != pending_responses_.end()) {
                auto promise = std::move(it->second.promise);
                pending_responses_.erase(it);
                promise->set_value(std::move(message));
                return;
            }
        }
    }

    if (TryBufferRequestStreamNotification(message)) return;

    const bool wait_for_listener = !options_.stateless || options_.enable_legacy_sse;
    if ((IsRequest(message) || IsNotification(message)) && wait_for_listener &&
        http_server_ && http_server_->SseClientCount() == 0) {
        std::string wait_method;
        if (const auto* pending = std::get_if<JsonRpcRequest>(&message))
            wait_method = pending->method;
        else if (const auto* outgoing = AsNotification(message))
            wait_method = outgoing->method;
        auto waited = std::chrono::milliseconds(0);
        while (http_server_->SseClientCount() == 0) {
            if (GetState() == TransportState::Disconnected)
                break;
            if (waited >= kSseListenerWaitBudget) {
                MCP_LOG(Warning, "server-initiated message '" + wait_method +
                    "' broadcast with no SSE listener");
                break;
            }
            std::this_thread::sleep_for(kSseListenerWaitStep);
            waited += kSseListenerWaitStep;
        }
    }

    std::string broadcast_method;
    std::optional<std::string> broadcast_resource;
    if (const auto* outgoing = AsNotification(message)) {
        broadcast_method = outgoing->method;
        if (broadcast_method == notifications::kResourceUpdated)
            broadcast_resource = UpdatedResourceUri(*outgoing);
    }
    auto event_data = BuildSseEvent(std::move(message));
    if (!options_.stateless) {
        auto event_id = event_store_->Append(message_session, event_data);
        event_data = "id: " + std::to_string(event_id) + "\n" + event_data;
        if (event_data.size() > SseEventLimit()) event_data.resize(SseEventLimit());
    }
    if (http_server_) {
        const bool filtered = !broadcast_method.empty() &&
            IsListenFilteredNotification(broadcast_method);
        if (!message_session.empty()) {
            http_server_->BroadcastSse(event_data,
                [session = message_session, filtered,
                 method = broadcast_method, resource = broadcast_resource](
                    const std::string& tag) {
                    if (!TagMatchesSession(tag, session)) return false;
                    if (!filtered) return true;
                    return ListenTagAccepts(tag, method, resource);
                });
        } else if (filtered) {
            http_server_->BroadcastSse(event_data,
                [method = broadcast_method, resource = broadcast_resource](
                    const std::string& tag) {
                    return ListenTagAccepts(tag, method, resource);
                });
        } else {
            http_server_->BroadcastSse(event_data);
        }
    }
}

std::string StreamableHttpServerTransport::RequestIdToString(const RequestId& id) {
    if (std::holds_alternative<int64_t>(id)) {
        return std::to_string(std::get<int64_t>(id));
    }
    return std::get<std::string>(id);
}

std::string StreamableHttpServerTransport::BuildSseEvent(
    JsonRpcMessage msg)
{
    std::string data = "event: message\ndata: " + SseEscapeData(SerializeMessage(std::move(msg))) + "\n\n";
    if (data.size() > SseEventLimit()) data.resize(SseEventLimit());
    return data;
}

std::size_t StreamableHttpServerTransport::SseEventLimit() const {
    return options_.max_sse_event_bytes.value_or(detail::kMaxSseEventBytes);
}

std::optional<std::string> StreamableHttpServerTransport::RequestTokenKey(
    const JsonRpcRequest& req)
{
    if (req.meta && req.meta->IsObject()) {
        if (auto key = RequestStreamTokenFromObject(*req.meta)) return key;
    }
    if (req.params && req.params->IsObject()) {
        if (auto* m = req.params->Find(detail::kMeta); m && m->IsObject()) {
            if (auto key = RequestStreamTokenFromObject(*m)) return key;
        }
    }
    return std::nullopt;
}

std::optional<std::string> StreamableHttpServerTransport::NotificationTokenKey(
    const JsonRpcNotification& notif)
{
    if (!notif.params || !notif.params->IsObject()) return std::nullopt;
    return RequestStreamTokenFromObject(*notif.params);
}

void StreamableHttpServerTransport::EraseRequestStreamStateLocked(
    const std::string& id_str)
{
    hold_open_requests_.erase(id_str);
    request_stream_frames_.erase(id_str);
    listen_filters_.erase(id_str);
    for (auto it = progress_token_routes_.begin();
         it != progress_token_routes_.end();) {
        if (it->second == id_str) it = progress_token_routes_.erase(it);
        else ++it;
    }
}

bool StreamableHttpServerTransport::TryBufferRequestStreamNotification(
    const JsonRpcMessage& message)
{
    auto* notif = AsNotification(message);
    if (!notif) return false;
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (hold_open_requests_.empty()) return false;
    const auto session_prefix = notif->session_id.empty()
        ? std::string{}
        : PendingKey(notif->session_id, "");
    const auto belongs = [&session_prefix](const std::string& key) {
        return session_prefix.empty() ||
               key.compare(0, session_prefix.size(), session_prefix) == 0;
    };
    if (notif->method == notifications::kProgress) {
        auto key = NotificationTokenKey(*notif);
        if (!key) return false;
        auto route = progress_token_routes_.find(*key);
        if (route == progress_token_routes_.end()) return false;
        if (!belongs(route->second)) return false;
        if (hold_open_requests_.count(route->second) == 0) return false;
        request_stream_frames_[route->second].push_back(
            BuildSseEvent(JsonRpcMessage(message)));
        return true;
    }
    if (notif->method == notifications::kMessage ||
        notif->method == notifications::kSubscriptionsAcknowledged) {
        for (const auto& id_str : hold_open_requests_) {
            if (!belongs(id_str)) continue;
            request_stream_frames_[id_str].push_back(
                BuildSseEvent(JsonRpcMessage(message)));
        }
        return true;
    }
    if (notif->method == notifications::kToolListChanged ||
        notif->method == notifications::kPromptListChanged ||
        notif->method == notifications::kResourceListChanged) {
        bool appended = false;
        for (const auto& [id_str, filter] : listen_filters_) {
            if (!belongs(id_str)) continue;
            if (hold_open_requests_.count(id_str) == 0) continue;
            bool match = false;
            if (notif->method == notifications::kToolListChanged) match = filter.tools;
            else if (notif->method == notifications::kPromptListChanged) match = filter.prompts;
            else match = filter.resources;
            if (!match) continue;
            request_stream_frames_[id_str].push_back(
                BuildSseEvent(JsonRpcMessage(message)));
            appended = true;
        }
        return appended;
    }
    if (notif->method == notifications::kResourceUpdated) {
        auto uri = UpdatedResourceUri(*notif);
        if (!uri) return false;
        bool appended = false;
        for (const auto& [id_str, filter] : listen_filters_) {
            if (!belongs(id_str)) continue;
            if (hold_open_requests_.count(id_str) == 0) continue;
            bool match = false;
            for (const auto& candidate : filter.resource_uris) {
                if (candidate == *uri) {
                    match = true;
                    break;
                }
            }
            if (!match) continue;
            request_stream_frames_[id_str].push_back(
                BuildSseEvent(JsonRpcMessage(message)));
            appended = true;
        }
        return appended;
    }
    if (IsTaskStreamNotification(notif->method)) {
        bool appended = false;
        for (const auto& entry : listen_filters_) {
            if (!belongs(entry.first)) continue;
            if (hold_open_requests_.count(entry.first) == 0) continue;
            request_stream_frames_[entry.first].push_back(
                BuildSseEvent(JsonRpcMessage(message)));
            appended = true;
        }
        return appended;
    }
    return false;
}

bool StreamableHttpServerTransport::BufferRequestStreamNotification(
    JsonRpcMessage message)
{
    return TryBufferRequestStreamNotification(message);
}

std::optional<std::string> StreamableHttpServerTransport::GetMcpHeader(
    const HttpRequest& req, std::string_view header_name) const
{
    auto key = detail::ToLower(header_name);
    auto it = req.headers.find(key);
    if (it != req.headers.end()) return std::optional<std::string>(it->second);
    return std::nullopt;
}

} // namespace mcp

#ifdef MCP_CPP_POP_GETOBJECT_SERVER
#pragma pop_macro("GetObject")
#undef MCP_CPP_POP_GETOBJECT_SERVER
#endif
