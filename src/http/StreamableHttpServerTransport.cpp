#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/Methods.hpp>

#include <asio/post.hpp>

#include <sstream>

namespace mcp {

// JSON parse safety limits
#define K_MAX_MESSAGE_SIZE (8 * 1024 * 1024)  // 8MB
// K_MAX_JSON_DEPTH removed — nlohmann-json v3.11.3 parse() accepts 4 args max

StreamableHttpServerTransport::StreamableHttpServerTransport(
    asio::io_context& io_ctx,
    StreamableHttpServerOptions options)
    : TransportBase(io_ctx)
    , options_(std::move(options))
    , http_server_(std::make_unique<HttpServer>(io_ctx, options_.port))
    , event_store_(options_.event_store
        ? options_.event_store
        : std::make_shared<EventStore>())
{
    session_id_ = "srv-" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());

    channel_ = std::make_unique<MessageChannel>(io_ctx, 64);

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
}

StreamableHttpServerTransport::~StreamableHttpServerTransport() {
    Close();
}

void StreamableHttpServerTransport::Start() {
    if (http_server_) http_server_->Start();
}

void StreamableHttpServerTransport::Close() {
    if (http_server_) http_server_->Stop();
}

// ── ValidateMcpHeaders ──
bool StreamableHttpServerTransport::ValidateMcpHeaders(
    const std::string& method_header,
    const std::string& name_header,
    const nlohmann::json& body,
    std::string& error_out)
{
    // Validate Mcp-Method header matches body method field
    auto body_method = body.value("method", "");
    if (!method_header.empty() && !body_method.empty() &&
        method_header != body_method)
    {
        error_out = "Mcp-Method header '" + method_header +
                    "' does not match body method '" + body_method + "'";
        return false;
    }

    // Validate Mcp-Name header matches body params name
    if (!name_header.empty()) {
        auto params_it = body.find("params");
        std::string body_name;
        if (params_it != body.end() && params_it->is_object()) {
            // Check params.name (tools/call, prompts/get) and params.uri (resources/read)
            body_name = params_it->value("name", "");
            if (body_name.empty())
                body_name = params_it->value("uri", "");
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
    // Extract MCP headers
    auto proto_ver = GetMcpHeader(req, "mcp-protocol-version");
    auto mcp_method = GetMcpHeader(req, "mcp-method");
    auto mcp_name = GetMcpHeader(req, "mcp-name");

    // Parse JSON-RPC message from body
    JsonRpcMessage msg;
    nlohmann::json body_json;
    try {
        if (req.body.size() > K_MAX_MESSAGE_SIZE) {
            resp.status_code = 413;
            resp.status_text = "Payload Too Large";
            resp.body = R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"Message size exceeds maximum allowed size"}})";
            resp.headers["content-type"] = "application/json";
            return;
        }
        body_json = nlohmann::json::parse(req.body, nullptr, false, false);
        msg = body_json.get<JsonRpcMessage>();
    } catch (...) {
        resp.status_code = 400;
        resp.status_text = "Bad Request";
        resp.body = R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"Parse error"}})";
        resp.headers["content-type"] = "application/json";
        return;
    }

    // Validate MCP headers match body
    std::string header_error;
    if (!ValidateMcpHeaders(mcp_method, mcp_name, body_json, header_error)) {
        resp.status_code = 400;
        resp.status_text = "Bad Request";
        nlohmann::json err;
        err["jsonrpc"] = "2.0";
        err["error"]["code"] = static_cast<int32_t>(McpErrorCode::HeaderMismatch);
        err["error"]["message"] = header_error;
        resp.body = err.dump();
        resp.headers["content-type"] = "application/json";
        return;
    }

    // Set MCP protocol version in response
    if (!proto_ver.empty()) {
        resp.headers["mcp-protocol-version"] = proto_ver;
    }

    // Echo Mcp-Method and Mcp-Name headers in response (SEP-2243)
    if (!mcp_method.empty()) {
        resp.headers["mcp-method"] = mcp_method;
    }
    if (!mcp_name.empty()) {
        resp.headers["mcp-name"] = mcp_name;
    }

    // Extract Mcp-Param-* headers from request and store in meta
    if (auto* req_ptr = std::get_if<JsonRpcRequest>(&msg)) {
        nlohmann::json meta_headers;
        for (auto& [key, val] : req.headers) {
            const std::string prefix = "mcp-param-";
            if (key.compare(0, prefix.size(), prefix) == 0) {
                auto param_name = key.substr(prefix.size());
                // Restore original casing convention for the header name
                meta_headers[param_name] = val;
            }
        }
        if (!meta_headers.empty()) {
            if (!req_ptr->meta) req_ptr->meta = nlohmann::json::object();
            (*req_ptr->meta)["x-mcp-headers"] = std::move(meta_headers);
        }
    }

    // Check if this is a request (needs response) or notification (no response)
    bool needs_response = IsRequest(msg);

    if (needs_response) {
        // Send message to protocol and wait for response via channel
        if (channel_ && channel_->IsOpen()) {
            channel_->Send(std::move(msg));
            resp.status_code = 202;
            resp.status_text = "Accepted";
            resp.body = R"({"jsonrpc":"2.0","result":{"resultType":"complete"}})";
            resp.headers["content-type"] = "application/json";
        }
    } else {
        // Notification: fire-and-forget
        if (channel_ && channel_->IsOpen()) {
            channel_->Send(std::move(msg));
        }
        resp.status_code = 202;
        resp.status_text = "Accepted";
        resp.body = "{}";
        resp.headers["content-type"] = "application/json";
    }

    // Check response body for x-mcp-header annotations → Mcp-Param-* headers
    if (!resp.body.empty() && resp.body.size() <= K_MAX_MESSAGE_SIZE) {
        try {
            auto resp_json = nlohmann::json::parse(resp.body, nullptr, false, false);
            auto meta_it = resp_json.find("_meta");
            if (meta_it != resp_json.end() && meta_it->is_object()) {
                auto xhc_it = meta_it->find("x-mcp-header");
                if (xhc_it != meta_it->end() && xhc_it->is_object()) {
                    for (auto& [hk, hv] : xhc_it->items()) {
                        resp.headers["mcp-param-" + hk] = hv.is_string()
                            ? hv.get<std::string>() : hv.dump();
                    }
                }
            }
        } catch (...) {}
    }
}

// ── Handle GET (SSE stream) ──
void StreamableHttpServerTransport::HandleGet(
    const HttpRequest& req, HttpResponse& resp)
{
    // SSE stream response
    resp.is_sse = true;
    resp.headers["content-type"] = "text/event-stream";
    resp.headers["cache-control"] = "no-cache";

    // Send initial endpoint event
    std::string endpoint_event = "event: endpoint\ndata: " +
        options_.endpoint + "\n\n";
    (void)endpoint_event; // Will be sent via SSE client mechanism
}

// ── Send message (server-initiated notification via SSE) ──
void StreamableHttpServerTransport::SendMessageAsync(JsonRpcMessage message) {
    // Store event
    auto event_data = BuildSseEvent(message);
    if (!options_.stateless) {
        event_store_->Append(session_id_, event_data);
    }

    // Broadcast to SSE clients
    if (http_server_) {
        http_server_->BroadcastSse(event_data);
    }
}

// ── Build SSE event ──
std::string StreamableHttpServerTransport::BuildSseEvent(
    const JsonRpcMessage& msg)
{
    nlohmann::json j = msg;
    std::string data = "event: message\ndata: " + j.dump() + "\n\n";
    return data;
}

// ── Header helper ──
std::string StreamableHttpServerTransport::GetMcpHeader(
    const HttpRequest& req, std::string_view header_name) const
{
    auto key = std::string(header_name);
    for (auto& c : key) c = std::tolower(c);
    auto it = req.headers.find(key);
    if (it != req.headers.end()) return it->second;
    return {};
}

} // namespace mcp
