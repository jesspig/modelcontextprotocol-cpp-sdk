#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/Methods.hpp>
#include <mcp/Log.hpp>

// TODO(libhv): full rewrite with libhv

#include <sstream>

namespace mcp {

#define K_MAX_MESSAGE_SIZE (8 * 1024 * 1024)

StreamableHttpServerTransport::StreamableHttpServerTransport(
    StreamableHttpServerOptions options)
    : TransportBase()
    , options_(std::move(options))
    , http_server_(std::make_unique<HttpServer>(options_.port))
    , event_store_(options_.event_store
        ? options_.event_store
        : std::make_shared<EventStore>())
{
    session_id_ = "srv-" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());

    channel_ = std::make_unique<MessageChannel>(64);

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
    if (channel_) channel_->Close();
    SetDisconnected();
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
            if (auto* n = params->Find("name")) body_name = n->GetString();
            if (body_name.empty())
                if (auto* u = params->Find("uri")) body_name = u->GetString();
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
    JsonValue body_jv;
    try {
        if (req.body.size() > K_MAX_MESSAGE_SIZE) {
            resp.status_code = 413;
            resp.status_text = "Payload Too Large";
            resp.body = R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"Message size exceeds maximum allowed size"}})";
            resp.headers["content-type"] = "application/json";
            return;
        }
        body_jv = JsonValue::Parse(req.body);
        if (body_jv.IsNull()) throw std::runtime_error("parse failed");
        msg = DeserializeMessage(req.body);
    } catch (...) {
        MCP_LOG(Warning, "request body parse failed");
        resp.status_code = 400;
        resp.status_text = "Bad Request";
        resp.body = R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"Parse error"}})";
        resp.headers["content-type"] = "application/json";
        return;
    }

    // Validate MCP headers match body
    std::string header_error;
    if (!ValidateMcpHeaders(mcp_method, mcp_name, body_jv, header_error)) {
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

    // Extract Mcp-Param-* headers from request (case-insensitive) and store in meta
    if (auto* req_ptr = std::get_if<JsonRpcRequest>(&msg)) {
        JsonValue::Object meta_headers_obj;
        for (const auto& [key, val] : req.headers) {
            std::string key_lower = key;
            for (auto& c : key_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const std::string prefix = "mcp-param-";
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

    // Extract request ID before msg is moved (stateless correlation)
    std::optional<RequestId> req_id;
    if (needs_response && options_.stateless) {
        if (auto* r = std::get_if<JsonRpcRequest>(&msg)) {
            req_id = r->id;
        }
    }

    if (needs_response) {
        if (channel_ && channel_->IsOpen()) {
            if (options_.stateless && req_id) {
                // Stateless mode: wait for response synchronously
                auto id_str = RequestIdToString(*req_id);
                auto promise = std::make_shared<std::promise<JsonRpcMessage>>();
                auto future = promise->get_future();
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    pending_responses_[id_str] = promise;
                }
                channel_->Send(std::move(msg));

                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                // TODO(libhv): replace io_ctx_.run_one() with proper event loop
                while (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                    if (std::chrono::steady_clock::now() >= deadline) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }

                if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    auto response = future.get();
                    resp.body = SerializeMessage(response);
                    resp.status_code = 200;
                    resp.status_text = "OK";
                    resp.headers["content-type"] = "application/json";
                } else {
                    {
                        std::lock_guard<std::mutex> lock(pending_mutex_);
                        pending_responses_.erase(id_str);
                    }
                    resp.status_code = 500;
                    resp.status_text = "Internal Server Error";
                    resp.body = R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"Request timeout"}})";
                    resp.headers["content-type"] = "application/json";
                }
            } else {
                channel_->Send(std::move(msg));
                resp.status_code = 202;
                resp.status_text = "Accepted";
                resp.body = R"({"jsonrpc":"2.0","result":{"resultType":"complete"}})";
                resp.headers["content-type"] = "application/json";
            }
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
            auto resp_jv = JsonValue::Parse(resp.body);
            if (!resp_jv.IsNull()) {
                auto* meta = resp_jv.Find("_meta");
                if (meta && meta->IsObject()) {
                    auto* xhc = meta->Find("x-mcp-header");
                    if (xhc && xhc->IsObject()) {
                        for (const auto& [hk, hv] : *xhc) {
                            resp.headers["mcp-param-" + hk] = hv.IsString() ? hv.GetString() : hv.Dump();
                        }
                    }
                }
            }
        } catch (...) { MCP_LOG(Warning, "response meta parse failed"); }
    }
}

// ── Handle GET (SSE stream) ──
void StreamableHttpServerTransport::HandleGet(
    const HttpRequest& /*req*/, HttpResponse& resp)
{
    // SSE stream response
    resp.is_sse = true;
    resp.headers["content-type"] = "text/event-stream";
    resp.headers["cache-control"] = "no-cache";

    // Defer endpoint event — SendResponse registers the SSE client after
    // HandleGet returns, so a deferred BroadcastSse reaches the new client.
    std::string endpoint_event = "event: endpoint\ndata: " +
        options_.endpoint + "\n\n";
    auto self = std::static_pointer_cast<StreamableHttpServerTransport>(shared_from_this());
    // TODO(libhv): replace asio::post with direct invocation or thread pool
    if (self->http_server_) {
        self->http_server_->BroadcastSse(endpoint_event);
    }
}

// ── Send message (server-initiated notification via SSE) ──
void StreamableHttpServerTransport::SendMessageAsync(JsonRpcMessage message) {
    // Stateless mode: check for pending request-response correlation
    if (options_.stateless) {
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
    }

    // Normal path: store event and broadcast via SSE
    auto event_data = BuildSseEvent(message);
    if (!options_.stateless) {
        event_store_->Append(session_id_, event_data);
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
    const JsonRpcMessage& msg)
{
    std::string data = "event: message\ndata: " + SerializeMessage(msg) + "\n\n";
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
