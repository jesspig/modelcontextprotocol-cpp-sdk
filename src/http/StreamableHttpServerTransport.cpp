// StreamableHttpServerTransport.cpp - Streamable HTTP server transport implementation

#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <mcp/ErrorCodes.hpp>
#include <mcp/Methods.hpp>
#include <mcp/Log.hpp>
#include <mcp/McpError.hpp>
#include <mcp/ProtocolVersion.hpp>

#include <transport/detail/net/NetIoUtil.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <stdexcept>

#ifdef _WIN32
// Windows.h defines a GetObject macro that clashes with JsonValue::GetObject
// when translation units are merged (Unity build).
#pragma push_macro("GetObject")
#ifdef GetObject
#undef GetObject
#endif
#endif

namespace mcp {

namespace {
constexpr std::chrono::seconds kStatelessTimeout(30);
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
                         bool invalid_token) {
    std::string challenge =
        "Bearer resource_metadata=\"" + metadata_url + "\"";
    if (invalid_token) challenge += ", error=\"invalid_token\"";
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

    if (session_store_ && !options_.stateless) {
        SessionRecord record;
        record.protocol_version = std::string(kDefaultNegotiatedProtocolVersion);
        record.created_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        session_store_->Save(session_id_, record);
    }

    // Wire HTTP handlers
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

    // Session termination (RFC 9110 DELETE). Stateless mode has no session:
    // reject with 405. Closing the channel ends the session's message loop.
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

// ── Bearer auth (RFC 6750/9728) ──
bool StreamableHttpServerTransport::AuthorizeRequest(
    const HttpRequest& req, HttpResponse& resp)
{
    if (!options_.bearer_auth) return true;
    const auto& config = *options_.bearer_auth;

    auto authorization = GetMcpHeader(req, "authorization");
    if (!authorization || !IsBearerAuthorization(*authorization)) {
        RespondUnauthorized(resp, config.resource_metadata_url, false);
        return false;
    }
    auto token = authorization->substr(kBearerSchemeLength);
    detail::net::TrimInPlace(token);

    auto result = config.verify(token);
    if (!result.ok) {
        RespondUnauthorized(resp, config.resource_metadata_url, true);
        return false;
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

// ── Session adoption (external SessionStore) ──
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
    if (options_.stateless || !session_store_) return true;
    auto session_id = GetMcpHeader(req, "mcp-session-id");
    if (!session_id || session_id->empty()) return true;
    if (*session_id == ActiveSessionId() && channel_ && channel_->IsOpen()) {
        return true;
    }
    if (session_store_->Load(*session_id)) {
        AdoptSession(*session_id);
        return true;
    }
    resp.status_code = 404;
    resp.status_text = "Not Found";
    resp.body = R"({"jsonrpc":"2.0","error":{"code":-32009,"message":"Session expired"}})";
    resp.headers["content-type"] = "application/json";
    return false;
}

// ── ValidateMcpHeaders ──
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
        auto* params = body.Find("params");
        std::string body_name;
        if (params && params->IsObject()) {
            if (auto* n = params->Find("name"); n && n->IsString()) body_name = n->GetString();
            if (body_name.empty())
                if (auto* u = params->Find("uri"); u && u->IsString()) body_name = u->GetString();
        }
        if (!body_name.empty() && name_header != body_name) {
            error_out = "Mcp-Name header '" + name_header +
                        "' does not match body params name '" + body_name + "'";
            return false;
        }
    }

    return true;
}

// ── Handle POST ──
void StreamableHttpServerTransport::HandlePost(
    const HttpRequest& req, HttpResponse& resp)
{
    if (!AuthorizeRequest(req, resp)) return;
    if (!EnsureSession(req, resp)) return;

    // Extract MCP headers
    auto proto_ver = GetMcpHeader(req, "mcp-protocol-version");
    auto mcp_method = GetMcpHeader(req, "mcp-method");
    auto mcp_name = GetMcpHeader(req, "mcp-name");

    // Parse JSON-RPC message from body (single parse inside DeserializeMessage)
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

    // Reuse the parsed message for header validation instead of re-parsing the body
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

    // Validate MCP headers match body
    std::string header_error;
    if (!ValidateMcpHeaders(mcp_method.value_or(""), mcp_name.value_or(""), body_jv, header_error)) {
        resp.status_code = 400;
        resp.status_text = "Bad Request";
        JsonValue::Object err_obj;
        err_obj["jsonrpc"] = JsonValue("2.0");
        {
            JsonValue::Object err_err;
            err_err["code"] = JsonValue(static_cast<int64_t>(McpErrorCode::HeaderMismatch));
            err_err["message"] = JsonValue(header_error);
            err_obj["error"] = JsonValue(std::move(err_err));
        }
        resp.body = JsonValue(std::move(err_obj)).Dump();
        resp.headers["content-type"] = "application/json";
        return;
    }

    // Set MCP protocol version in response
    if (proto_ver.has_value()) {
        resp.headers["mcp-protocol-version"] = proto_ver.value();
    }

    // Echo Mcp-Method and Mcp-Name headers in response (SEP-2243)
    if (mcp_method.has_value()) {
        resp.headers["mcp-method"] = mcp_method.value();
    }
    if (mcp_name.has_value()) {
        resp.headers["mcp-name"] = mcp_name.value();
    }

    // Extract Mcp-Param-* headers from request (case-insensitive) and store in meta
    if (auto* req_ptr = std::get_if<JsonRpcRequest>(&msg)) {
        JsonValue::Object meta_headers_obj;
        for (const auto& [key, val] : req.headers) {
            std::string key_lower = key;
            for (auto& c : key_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
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
    }

    // Check if this is a request (needs response) or notification (no response)
    bool needs_response = IsRequest(msg);

    // Extract request ID before msg is moved (request/response correlation)
    std::optional<RequestId> req_id;
    bool is_initialize = false;
    if (needs_response) {
        if (auto* r = std::get_if<JsonRpcRequest>(&msg)) {
            req_id = r->id;
            is_initialize = (r->method == methods::kInitialize);
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
        // Wait for the response synchronously; the pending entry is resolved
        // by SendMessageAsync when the matching response arrives.
        auto id_str = RequestIdToString(*req_id);
        auto promise = std::make_shared<std::promise<JsonRpcMessage>>();
        auto future = promise->get_future();
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_responses_[id_str] = promise;
        }
        if (!channel_->TrySend(std::move(msg))) {
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                pending_responses_.erase(id_str);
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
        // Protocol errors map to conventional HTTP status codes
        // (-32020/-32021/-32022/-32602 -> 400, -32601 -> 404); all other
        // responses are delivered as an SSE stream event.
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
        // Mirror x-mcp-header annotations from the result meta into
        // Mcp-Param-* response headers (SEP-2243).
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
        resp.body = "event: message\ndata: " +
                    SseEscapeData(SerializeMessage(std::move(response))) + "\n\n";
    } else {
        // Notification: fire-and-forget
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

// ── Handle GET (SSE stream) ──
void StreamableHttpServerTransport::HandleGet(
    const HttpRequest& req, HttpResponse& resp)
{
    if (!AuthorizeRequest(req, resp)) return;
    if (!EnsureSession(req, resp)) return;

    resp.is_sse = true;
    resp.headers["content-type"] = "text/event-stream";
    resp.headers["cache-control"] = "no-cache";

    // The body carries the endpoint event; HttpServer writes it to the SSE
    // stream explicitly after flushing the headers.
    resp.body = "event: endpoint\ndata: " + SseEscapeData(options_.endpoint) + "\n\n";

    // Resume: replay missed events after the client's Last-Event-ID
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

// ── Send message (server-initiated notification via SSE) ──
void StreamableHttpServerTransport::SendMessageAsync(JsonRpcMessage message) {
    // A response matching an in-flight request resolves its pending promise;
    // the response is delivered on the POST response stream, not broadcast.
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

    // Normal path: store event and broadcast via SSE
    auto event_data = BuildSseEvent(std::move(message));
    if (!options_.stateless) {
        auto event_id = event_store_->Append(ActiveSessionId(), event_data);
        event_data = "id: " + std::to_string(event_id) + "\n" + event_data;
    }
    if (http_server_) {
        http_server_->BroadcastSse(event_data);
    }
}

// ── Convert RequestId to string for map key ──
std::string StreamableHttpServerTransport::RequestIdToString(const RequestId& id) {
    if (std::holds_alternative<int64_t>(id)) {
        return std::to_string(std::get<int64_t>(id));
    }
    return std::get<std::string>(id);
}

// ── Build SSE event ──
std::string StreamableHttpServerTransport::BuildSseEvent(
    JsonRpcMessage msg)
{
    std::string data = "event: message\ndata: " + SseEscapeData(SerializeMessage(std::move(msg))) + "\n\n";
    return data;
}

// ── Header helper ──
std::optional<std::string> StreamableHttpServerTransport::GetMcpHeader(
    const HttpRequest& req, std::string_view header_name) const
{
    auto key = std::string(header_name);
    for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto it = req.headers.find(key);
    if (it != req.headers.end()) return std::optional<std::string>(it->second);
    return std::nullopt;
}

} // namespace mcp
