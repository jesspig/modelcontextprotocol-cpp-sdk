#include <mcp/protocol/Protocol.hpp>
#include <mcp/McpError.hpp>

#include <asio/post.hpp>

namespace mcp {

// ====================================================================
// Construction / Destruction
// ====================================================================
Protocol::Protocol(
    asio::io_context& io_ctx,
    std::unique_ptr<Transport> transport)
    : io_ctx_(io_ctx)
    , transport_(std::move(transport))
    , codec_(MakeWireCodec("2025-11-25"))
{
    asio::post(io_ctx_, [this]() {
        if (transport_) transport_->Start();
    });
}

Protocol::~Protocol() {
    if (!closed_) Close();
}

// ====================================================================
// Lifecycle
// ====================================================================
void Protocol::Start() {
    if (started_) return;
    started_ = true;
    asio::post(io_ctx_, [this]() { MessageLoop(); });
}

void Protocol::Close() {
    closed_ = true;

    for (auto& [id, pending] : pending_) {
        if (pending) {
            try {
                nlohmann::json err;
                err["code"] = static_cast<int32_t>(McpErrorCode::ConnectionClosed);
                err["message"] = "connection closed";
                pending->callback(std::move(err));
            } catch (...) {}
        }
    }
    pending_.clear();

    if (transport_) transport_->Close();
}

// ====================================================================
// Message loop (polling on the transport channel)
// ====================================================================
void Protocol::MessageLoop() {
    if (closed_) return;

    // Try to read from the transport channel
    auto& channel = transport_->GetMessageChannel();

    channel.async_receive(
        [this](asio::error_code ec, JsonRpcMessage msg) {
            if (ec || closed_) {
                if (!closed_) Close();
                return;
            }

            DispatchMessage(msg);

            // Continue the loop
            if (!closed_) {
                asio::post(io_ctx_, [this]() { MessageLoop(); });
            }
        });
}

// ====================================================================
// Message dispatch
// ====================================================================
void Protocol::DispatchMessage(const JsonRpcMessage& msg) {
    if (IsRequest(msg))
        OnRequest(*AsRequest(msg));
    else if (IsResponse(msg))
        OnResponse(*AsResponse(msg));
    else if (IsError(msg))
        OnError(*AsError(msg));
    else if (IsNotification(msg))
        OnNotification(*AsNotification(msg));
}

// ── Handle incoming request ──
void Protocol::OnRequest(const JsonRpcRequest& req) {
    auto it = request_handlers_.find(req.method);
    if (it == request_handlers_.end()) {
        JsonRpcErrorResponse err_resp;
        err_resp.id = req.id;
        err_resp.error.code = McpErrorCode::MethodNotFound;
        err_resp.error.message = "method not found: " + req.method;
        transport_->SendMessageAsync(JsonRpcMessage{std::move(err_resp)});
        return;
    }

    // Get a promise for the handler result
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future = promise->get_future();

    // Dispatch handler
    it->second(req, std::move(*promise));

    // When the handler completes, send the response
    std::thread([this, req, future = std::move(future)]() mutable {
        try {
            auto result = future.get();
            if (closed_) return;

            JsonRpcResponse resp;
            resp.id = req.id;
            resp.result = codec_->EncodeResult(req.method, result);
            transport_->SendMessageAsync(JsonRpcMessage{std::move(resp)});
        } catch (const McpError& e) {
            if (closed_) return;
            JsonRpcErrorResponse err_resp;
            err_resp.id = req.id;
            err_resp.error.code = e.Code();
            err_resp.error.message = e.what();
            transport_->SendMessageAsync(JsonRpcMessage{std::move(err_resp)});
        } catch (...) {
            if (closed_) return;
            JsonRpcErrorResponse err_resp;
            err_resp.id = req.id;
            err_resp.error.code = McpErrorCode::InternalError;
            err_resp.error.message = "internal handler error";
            transport_->SendMessageAsync(JsonRpcMessage{std::move(err_resp)});
        }
    }).detach();
}

// ── Extract int64 from RequestId ──
int64_t RequestIdToInt64(const RequestId& rid) {
    if (std::holds_alternative<int64_t>(rid))
        return std::get<int64_t>(rid);
    // For string IDs, hash for lookup (rare fallback)
    return static_cast<int64_t>(std::hash<std::string>{}(std::get<std::string>(rid)));
}

// ── Handle response ──
void Protocol::OnResponse(const JsonRpcResponse& resp) {
    auto id = RequestIdToInt64(resp.id);
    auto it = pending_.find(id);
    if (it != pending_.end()) {
        if (it->second->timer) it->second->timer->cancel();
        it->second->callback(resp.result);
        pending_.erase(it);
    }
}

// ── Handle error ──
void Protocol::OnError(const JsonRpcErrorResponse& err) {
    auto id = RequestIdToInt64(err.id);
    auto it = pending_.find(id);
    if (it != pending_.end()) {
        if (it->second->timer) it->second->timer->cancel();
        nlohmann::json error_json;
        error_json["code"] = static_cast<int32_t>(err.error.code);
        error_json["message"] = err.error.message;
        if (err.error.data) error_json["data"] = *err.error.data;
        it->second->callback(std::move(error_json));
        pending_.erase(it);
    }
}

// ── Handle notification ──
void Protocol::OnNotification(const JsonRpcNotification& notif) {
    auto it = notif_handlers_.find(notif.method);
    if (it != notif_handlers_.end()) {
        it->second(notif);
    }
}

// ====================================================================
// Send request
// ====================================================================
std::future<nlohmann::json> Protocol::SendRequest(
    std::string_view method,
    nlohmann::json params,
    const RequestMeta& meta,
    std::chrono::milliseconds timeout)
{
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future = promise->get_future();

    auto id = next_request_id_++;

    JsonRpcRequest req;
    req.id = RequestId{id};
    req.method = std::string(method);
    req.params = std::move(params);

    // Stamp _meta for 2026 era
    if (meta.protocol_version >= "2026-07-28") {
        auto meta_json = nlohmann::json::object();
        meta_json["io.modelcontextprotocol/protocolVersion"] =
            meta.protocol_version;
        if (meta.client_info)
            meta_json["io.modelcontextprotocol/clientInfo"] = *meta.client_info;
        if (meta.client_capabilities)
            meta_json["io.modelcontextprotocol/clientCapabilities"] =
                *meta.client_capabilities;
        req.meta = std::move(meta_json);
    }

    // Register pending request
    auto pending = std::make_shared<PendingRequest>();
    pending->callback = [promise](nlohmann::json result) {
        promise->set_value(std::move(result));
    };

    if (timeout.count() > 0) {
        pending->timer = std::make_shared<asio::steady_timer>(io_ctx_);
        pending->timer->expires_after(timeout);
        pending->timer->async_wait(
            [this, id, promise](asio::error_code ec) {
                if (ec) return;
                auto it = pending_.find(id);
                if (it != pending_.end()) {
                    nlohmann::json err;
                    err["code"] = static_cast<int32_t>(McpErrorCode::RequestTimeout);
                    err["message"] = "request timed out";
                    it->second->callback(std::move(err));
                    pending_.erase(it);
                }
            });
    }

    pending_[id] = pending;

    // Send on wire
    transport_->SendMessageAsync(JsonRpcMessage{std::move(req)});

    return future;
}

// ====================================================================
// Send notification
// ====================================================================
void Protocol::SendNotification(
    std::string_view method, nlohmann::json params)
{
    JsonRpcNotification notif;
    notif.method = std::string(method);
    if (!params.is_null() && !params.empty()) {
        notif.params = std::move(params);
    }
    transport_->SendMessageAsync(JsonRpcMessage{std::move(notif)});
}

// ====================================================================
// Handler registration
// ====================================================================
void Protocol::SetRequestHandler(
    std::string_view method, RequestHandler handler)
{
    request_handlers_[std::string(method)] = std::move(handler);
}

void Protocol::SetNotificationHandler(
    std::string_view method, NotificationHandler handler)
{
    notif_handlers_[std::string(method)] = std::move(handler);
}

void Protocol::RemoveRequestHandler(std::string_view method) {
    request_handlers_.erase(std::string(method));
}

void Protocol::RemoveNotificationHandler(std::string_view method) {
    notif_handlers_.erase(std::string(method));
}

// ====================================================================
// Accessors
// ====================================================================
std::string_view Protocol::NegotiatedProtocolVersion() const {
    return negotiated_version_;
}

void Protocol::SetNegotiatedProtocolVersion(std::string_view version) {
    negotiated_version_ = std::string(version);
    codec_ = MakeWireCodec(version);
}

std::unique_ptr<WireCodec>& Protocol::Codec() { return codec_; }
const std::unique_ptr<WireCodec>& Protocol::Codec() const { return codec_; }
Transport& Protocol::GetTransport() { return *transport_; }
asio::io_context& Protocol::IoContext() { return io_ctx_; }

} // namespace mcp
