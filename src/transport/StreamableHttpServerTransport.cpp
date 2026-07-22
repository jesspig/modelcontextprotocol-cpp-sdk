#include <mcp/transport/StreamableHttpServerTransport.hpp>

#include <mcp/JsonRpc.hpp>
#include <mcp/Log.hpp>

#include <sstream>
#include <thread>

namespace mcp {

// ── Constructor ──
StreamableHttpServerTransport::StreamableHttpServerTransport(
    StreamableHttpServerOptions options)
    : options_(std::move(options))
{
    if (options_.event_store) {
        event_store_ = options_.event_store;
    } else {
        event_store_ = std::make_shared<EventStore>();
    }
}

StreamableHttpServerTransport::~StreamableHttpServerTransport() = default;

// ── Start ──
void StreamableHttpServerTransport::Start() {
    http_server_ = std::make_unique<HttpServer>(options_.port);

    if (options_.enable_legacy_sse) {
        http_server_->SetHandler("GET", options_.endpoint,
            [this](const HttpRequest& req, HttpResponse& resp) {
                HandleGet(req, resp);
            });
    }

    http_server_->SetHandler("POST", options_.endpoint,
        [this](const HttpRequest& req, HttpResponse& resp) {
            HandlePost(req, resp);
        });

    http_server_->Start();
    SetConnected();
    MCP_LOG(Info, "StreamableHttpServerTransport started on port "
        << options_.port << options_.endpoint);
}

// ── Close ──
void StreamableHttpServerTransport::Close() {
    if (http_server_) {
        http_server_->Stop();
        http_server_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (auto& [_, p] : pending_responses_) {
            try {
                p->set_value(JsonRpcMessage{});
            } catch (...) {}
        }
        pending_responses_.clear();
    }
    channel_->Close();
    SetDisconnected();
}

// ── SendMessageAsync ──
void StreamableHttpServerTransport::SendMessageAsync(JsonRpcMessage message) {
    // Stateless mode: fulfill pending promise if this is a correlated response
    std::string id_str;
    bool is_response = false;
    if (auto* resp = std::get_if<JsonRpcResponse>(&message)) {
        id_str = RequestIdToString(resp->id);
        is_response = true;
    } else if (auto* err = std::get_if<JsonRpcErrorResponse>(&message)) {
        if (err->id) {
            id_str = RequestIdToString(*err->id);
            is_response = true;
        }
    }

    if (is_response) {
        std::shared_ptr<std::promise<JsonRpcMessage>> p;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_responses_.find(id_str);
            if (it != pending_responses_.end()) {
                p = std::move(it->second);
                pending_responses_.erase(it);
            }
        }
        if (p) {
            p->set_value(std::move(message));
            return;
        }
    }

    // Legacy mode: broadcast via SSE
    if (options_.enable_legacy_sse && http_server_) {
        auto event = BuildSseEvent(message);
        http_server_->BroadcastSse(event);
    }
}

// ── HandleGet (SSE handler for legacy mode) ──
void StreamableHttpServerTransport::HandleGet(
    const HttpRequest& /*req*/, HttpResponse& resp)
{
    if (options_.stateless) {
        resp.status_code = 400;
        resp.body = "Stateless mode does not support SSE streaming";
        return;
    }

    resp.is_sse = true;
    resp.headers["Cache-Control"] = "no-cache";
    resp.headers["Content-Type"] = "text/event-stream";
    resp.headers["Connection"] = "keep-alive";
}

// ── HandlePost (JSON-RPC handler) ──
void StreamableHttpServerTransport::HandlePost(
    const HttpRequest& req, HttpResponse& resp)
{
    // Parse the JSON-RPC message
    JsonRpcMessage msg;
    try {
        msg = DeserializeMessage(req.body);
    } catch (const std::exception& e) {
        resp.status_code = 400;
        resp.body = R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"Parse error"},"id":null})";
        resp.headers["Content-Type"] = "application/json";
        MCP_LOG(Error, "Failed to parse JSON-RPC message: " << e.what());
        return;
    }

    // Validate MCP headers
    auto method_header = GetMcpHeader(req, "Mcp-Method");
    auto name_header = GetMcpHeader(req, "Mcp-Name");
    std::string header_error;
    if (!ValidateMcpHeaders(method_header, name_header, JsonValue::ObjectTag{}, header_error)) {
        resp.status_code = 400;
        JsonValue err_obj(JsonValue::object_tag);
        err_obj["jsonrpc"] = "2.0";
        auto err_data(JsonValue::object_tag);
        err_data["code"] = static_cast<int64_t>(McpErrorCode::HeaderMismatch);
        err_data["message"] = header_error;
        err_obj["error"] = std::move(err_data);
        err_obj["id"] = JsonValue(nullptr);
        resp.body = err_obj.Dump();
        resp.headers["Content-Type"] = "application/json";
        return;
    }

    // Notification: dispatch and return 202 Accepted
    if (IsNotification(msg)) {
        WriteMessage(std::move(msg));
        resp.status_code = 202;
        resp.headers["Content-Type"] = "application/json";
        resp.body = R"({"jsonrpc":"2.0","result":{"_meta":{"accepted":true}}})";
        return;
    }

    // Request with id: correlate response via promise
    RequestId* req_id = nullptr;
    if (auto* req_var = std::get_if<JsonRpcRequest>(&msg)) {
        req_id = &req_var->id;
    }

    if (req_id) {
        auto id_str = RequestIdToString(*req_id);
        auto p = std::make_shared<std::promise<JsonRpcMessage>>();
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_responses_[id_str] = p;
        }

        WriteMessage(std::move(msg));

        // Wait for response with timeout
        auto fut = p->get_future();
        if (fut.wait_for(std::chrono::seconds(30)) == std::future_status::ready) {
            auto response_msg = fut.get();
            resp.body = SerializeMessage(response_msg);
            resp.headers["Content-Type"] = "application/json";
        } else {
            resp.status_code = 504;
            resp.body = R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"Request timeout"},"id":null})";
            resp.headers["Content-Type"] = "application/json";

            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_responses_.erase(id_str);
        }
    }
}

// ── BuildSseEvent ──
std::string StreamableHttpServerTransport::BuildSseEvent(const JsonRpcMessage& msg) {
    auto json_str = SerializeMessage(msg);
    std::ostringstream oss;
    oss << "event: message\ndata: " << json_str << "\n\n";
    return oss.str();
}

// ── GetMcpHeader ──
std::string StreamableHttpServerTransport::GetMcpHeader(
    const HttpRequest& req, std::string_view header_name) const
{
    auto it = req.headers.find(std::string(header_name));
    if (it != req.headers.end()) return it->second;
    // Also check lowercase variant
    auto lower = std::string(header_name);
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    it = req.headers.find(lower);
    if (it != req.headers.end()) return it->second;
    return {};
}

// ── ValidateMcpHeaders ──
bool StreamableHttpServerTransport::ValidateMcpHeaders(
    const std::string& method_header,
    const std::string& /*name_header*/,
    const JsonValue& /*body*/,
    std::string& error_out)
{
    if (method_header.empty()) {
        error_out = "Missing Mcp-Method header";
        return false;
    }
    return true;
}

// ── RequestIdToString ──
std::string StreamableHttpServerTransport::RequestIdToString(const RequestId& id) {
    return std::visit([](auto&& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, int64_t>) {
            return std::to_string(arg);
        } else {
            return arg;
        }
    }, id);
}

} // namespace mcp
