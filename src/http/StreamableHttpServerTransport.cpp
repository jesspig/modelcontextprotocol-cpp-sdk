#include <mcp/detail/StringUtils.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <mcp/ErrorCodes.hpp>
#include <mcp/Methods.hpp>
#include <mcp/Log.hpp>
#include <mcp/McpError.hpp>
#include <mcp/ProtocolVersion.hpp>

#include <transport/detail/net/NetIoUtil.hpp>

#include <detail/JsonFields.hpp>

#include "McpParamHeaders.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
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
} // namespace

StreamableHttpServerTransport::StreamableHttpServerTransport(
    StreamableHttpServerOptions options)
    : TransportBase()
    , options_(std::move(options))
    , http_server_(std::make_unique<HttpServer>(options_.port,
          [keep_alive_ms = options_.sse_keep_alive_ms, host = options_.host] {
              HttpServerOptions http_options;
              http_options.sse_keep_alive_ms = keep_alive_ms;
              http_options.bind_host = host;
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

    session_id_ = "srv-" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());

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
        [this](const HttpRequest&, HttpResponse& resp) {
            if (options_.stateless) {
                resp.status_code = 405;
                resp.status_text = "Method Not Allowed";
                resp.body = R"({"jsonrpc":"2.0","error":{"code":-32601,"message":"method not found"}})";
                resp.headers["content-type"] = "application/json";
                return;
            }
            if (session_store_) session_store_->Remove(ActiveSessionId());
            if (channel_) channel_->Close();
            SetDisconnected();
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
}

StreamableHttpServerTransport::~StreamableHttpServerTransport() {
    Close();
}

void StreamableHttpServerTransport::Start() {
    if (http_server_) http_server_->Start();
}

void StreamableHttpServerTransport::Close() {
    if (http_server_) http_server_->Stop();
    if (!options_.stateless && event_store_) event_store_->Clear(ActiveSessionId());
    if (channel_) channel_->Close();
    SetDisconnected();
}

bool StreamableHttpServerTransport::AuthorizeRequest(
    const HttpRequest& req, HttpResponse& resp)
{
    if (!options_.bearer_auth) return true;
    const auto& config = *options_.bearer_auth;

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

std::string StreamableHttpServerTransport::ActiveSessionId() const {
    std::lock_guard<std::mutex> lock(session_state_mutex_);
    return session_id_;
}

void StreamableHttpServerTransport::AdoptSession(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(session_state_mutex_);
    session_id_ = session_id;
}

bool StreamableHttpServerTransport::EnsureSession(
    const HttpRequest& req, HttpResponse& resp)
{
    if (options_.stateless) return true;
    if (!session_store_) return true;
    auto session_id = GetMcpHeader(req, "mcp-session-id");
    if (!session_id || session_id->empty()) return true;
    if (session_store_->Load(*session_id)) {
        if (*session_id != ActiveSessionId()) AdoptSession(*session_id);
        return true;
    }
    resp.status_code = 404;
    resp.status_text = "Not Found";
    resp.body = R"({"jsonrpc":"2.0","error":{"code":-32009,"message":"Session expired"}})";
    resp.headers["content-type"] = "application/json";
    return false;
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
    if (!EnsureSession(req, resp)) return;

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
        if (req.body.size() > detail::kMaxHttpBodyBytes) {
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

    if (proto_ver.has_value() && !proto_ver->empty()) {
        resp.headers["mcp-protocol-version"] = proto_ver.value();
    } else {
        resp.headers["mcp-protocol-version"] =
            std::string(kDefaultNegotiatedProtocolVersion);
    }

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

    bool needs_response = IsRequest(msg);

    std::optional<RequestId> req_id;
    bool is_initialize = false;
    if (needs_response) {
        if (auto* r = std::get_if<JsonRpcRequest>(&msg)) {
            req_id = r->id;
            is_initialize = (r->method == methods::kInitialize);
        }
    }

    if (is_initialize && session_store_ && !options_.stateless) {
        SessionRecord record;
        record.protocol_version = std::string(kDefaultNegotiatedProtocolVersion);
        record.created_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (!session_store_->TrySave(ActiveSessionId(), record)) {
            resp.status_code = 503;
            resp.status_text = "Service Unavailable";
            resp.body = R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"session store is full"}})";
            resp.headers["content-type"] = "application/json";
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
        std::string hold_version(kDefaultNegotiatedProtocolVersion);
        if (proto_ver && !proto_ver->empty()) hold_version = *proto_ver;
        bool hold_open = IsModernProtocolVersion(hold_version);
        std::optional<std::string> hold_token;
        if (hold_open) {
            if (auto* hold_req = std::get_if<JsonRpcRequest>(&msg))
                hold_token = RequestTokenKey(*hold_req);
        }
        auto promise = std::make_shared<std::promise<JsonRpcMessage>>();
        auto future = promise->get_future();
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_responses_[id_str] = promise;
            if (hold_open) {
                hold_open_requests_.insert(id_str);
                if (hold_token) progress_token_routes_[*hold_token] = id_str;
            }
        }
        if (!channel_->TrySend(std::move(msg))) {
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                pending_responses_.erase(id_str);
                EraseRequestStreamStateLocked(id_str);
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
                pending_responses_.erase(id_str);
                EraseRequestStreamStateLocked(id_str);
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
                auto fit = request_stream_frames_.find(id_str);
                if (fit != request_stream_frames_.end())
                    hold_frames = std::move(fit->second);
                EraseRequestStreamStateLocked(id_str);
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
        if (is_initialize && session_store_ && !options_.stateless) {
            resp.headers["mcp-session-id"] = ActiveSessionId();
        }
        resp.status_code = 200;
        resp.status_text = "OK";
        resp.is_sse = true;
        resp.sse_close_after_write = true;
        resp.headers["content-type"] = "text/event-stream";
        resp.headers["cache-control"] = "no-cache";
        resp.headers["x-accel-buffering"] = "no";
        resp.body.clear();
        for (auto& hold_frame : hold_frames) resp.body += hold_frame;
        resp.body += "event: message\ndata: " +
                    SseEscapeData(SerializeMessage(std::move(response))) + "\n\n";
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
    if (!EnsureSession(req, resp)) return;

    auto proto_ver = GetMcpHeader(req, "mcp-protocol-version");
    if (proto_ver.has_value() && !proto_ver->empty() &&
        !IsSupportedProtocolVersion(*proto_ver)) {
        RespondUnsupportedVersion(resp, *proto_ver, std::nullopt);
        return;
    }
    if (proto_ver.has_value() && !proto_ver->empty()) {
        resp.headers["mcp-protocol-version"] = proto_ver.value();
    } else {
        resp.headers["mcp-protocol-version"] =
            std::string(kDefaultNegotiatedProtocolVersion);
    }

    resp.is_sse = true;
    resp.headers["content-type"] = "text/event-stream";
    resp.headers["cache-control"] = "no-cache";

    resp.body = "event: endpoint\ndata: " + SseEscapeData(options_.endpoint) + "\n\n";

    if (!options_.stateless) {
        auto last_id = GetMcpHeader(req, "last-event-id");
        if (last_id && !last_id->empty()) {
            try {
                auto from = std::stoull(*last_id);
                for (const auto& [ev_id, ev_data] :
                        event_store_->GetEventsSince(ActiveSessionId(), from)) {
                    resp.body += "id: " + std::to_string(ev_id) + "\n" + ev_data;
                }
            } catch (const std::exception&) {
                MCP_LOG(Warning, "invalid Last-Event-ID header; ignoring");
            }
        }
    }
}

void StreamableHttpServerTransport::SendMessageAsync(JsonRpcMessage message) {
    if (auto* resp = std::get_if<JsonRpcResponse>(&message)) {
        auto id_str = RequestIdToString(resp->id);
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_responses_.find(id_str);
        if (it != pending_responses_.end()) {
            it->second->set_value(std::move(message));
            pending_responses_.erase(it);
            return;
        }
    } else if (auto* err = std::get_if<JsonRpcErrorResponse>(&message)) {
        if (err->id) {
            auto id_str = RequestIdToString(*err->id);
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_responses_.find(id_str);
            if (it != pending_responses_.end()) {
                it->second->set_value(std::move(message));
                pending_responses_.erase(it);
                return;
            }
        }
    }

    if (TryBufferRequestStreamNotification(message)) return;

    if (IsRequest(message) && http_server_ &&
        http_server_->SseClientCount() == 0) {
        std::string wait_method;
        if (const auto* pending = std::get_if<JsonRpcRequest>(&message))
            wait_method = pending->method;
        auto waited = std::chrono::milliseconds(0);
        while (http_server_->SseClientCount() == 0) {
            if (GetState() == TransportState::Disconnected)
                break;
            if (waited >= kSseListenerWaitBudget) {
                MCP_LOG(Warning, "server-initiated request '" + wait_method +
                    "' broadcast with no SSE listener");
                break;
            }
            std::this_thread::sleep_for(kSseListenerWaitStep);
            waited += kSseListenerWaitStep;
        }
    }

    auto event_data = BuildSseEvent(std::move(message));
    if (!options_.stateless) {
        auto event_id = event_store_->Append(ActiveSessionId(), event_data);
        event_data = "id: " + std::to_string(event_id) + "\n" + event_data;
    }
    if (http_server_) {
        http_server_->BroadcastSse(event_data);
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
    return data;
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
    if (notif->method == notifications::kProgress) {
        auto key = NotificationTokenKey(*notif);
        if (!key) return false;
        auto route = progress_token_routes_.find(*key);
        if (route == progress_token_routes_.end()) return false;
        if (hold_open_requests_.count(route->second) == 0) return false;
        request_stream_frames_[route->second].push_back(
            BuildSseEvent(JsonRpcMessage(message)));
        return true;
    }
    if (notif->method == notifications::kMessage ||
        notif->method == notifications::kSubscriptionsAcknowledged) {
        for (const auto& id_str : hold_open_requests_)
            request_stream_frames_[id_str].push_back(
                BuildSseEvent(JsonRpcMessage(message)));
        return true;
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
