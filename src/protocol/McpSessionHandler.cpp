#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/protocol/MessageChannel.hpp>
#include <mcp/McpError.hpp>
#include <mcp/Methods.hpp>
#include <asio/post.hpp>

#include <chrono>
#include <cstdint>
#include <system_error>

namespace mcp {

// ═══════════════════════════════════════════════════════════════════════
// Construction
// ═══════════════════════════════════════════════════════════════════════
McpSessionHandler::McpSessionHandler(
    std::shared_ptr<ITransport> transport,
    std::unique_ptr<WireCodec> codec,
    bool is_server,
    std::shared_ptr<FilterPipeline> incoming_filters,
    std::shared_ptr<FilterPipeline> outgoing_filters)
    : transport_(std::move(transport))
    , codec_(std::move(codec))
    , is_server_(is_server)
    , incoming_filters_(std::move(incoming_filters))
    , outgoing_filters_(std::move(outgoing_filters))
    , io_ctx_(transport_->GetMessageChannel().IoContext())
{
}

McpSessionHandler::~McpSessionHandler() {
    Close();
}

// ═══════════════════════════════════════════════════════════════════════
// Lifecycle
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::Start() {
    if (running_) return;
    running_ = true;
    asio::post(io_ctx_, [self = shared_from_this()]() { self->MessageLoop(); });
}

void McpSessionHandler::Close() {
    if (closed_) return;
    closed_ = true;
    running_ = false;

    // Fail all pending requests
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        nlohmann::json err;
        err["code"] = static_cast<int32_t>(McpErrorCode::ConnectionClosed);
        err["message"] = "connection closed";
        for (auto& [id, pending] : pending_) {
            if (pending) {
                try { pending->callback(err); } catch (...) {}
            }
        }
        pending_.clear();
    }

    transport_->Close();
}

// ═══════════════════════════════════════════════════════════════════════
// Message loop
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::MessageLoop() {
    if (closed_) return;

    auto& channel = transport_->GetMessageChannel();

    channel.AsyncReceive(
        [self = shared_from_this()](asio::error_code ec, JsonRpcMessage msg) {
            if (ec || self->closed_) {
                if (!self->closed_) self->Close();
                return;
            }

            // Apply incoming filters if present
            if (self->incoming_filters_) {
                self->incoming_filters_->Execute(msg,
                    [self](const JsonRpcMessage& filtered_msg) {
                        if (!self->closed_) self->DispatchMessage(filtered_msg);
                    });
            } else {
                self->DispatchMessage(msg);
            }

            // Continue loop
            if (!self->closed_) {
                asio::post(self->io_ctx_,
                    [self]() { self->MessageLoop(); });
            }
        });
}

// ═══════════════════════════════════════════════════════════════════════
// Dispatch
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::DispatchMessage(const JsonRpcMessage& msg) {
    if (IsRequest(msg))
        OnRequest(*AsRequest(msg));
    else if (IsResponse(msg))
        OnResponse(*AsResponse(msg));
    else if (IsError(msg))
        OnError(*AsError(msg));
    else if (IsNotification(msg))
        OnNotification(*AsNotification(msg));
}

// ═══════════════════════════════════════════════════════════════════════
// Request handling
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::OnRequest(const JsonRpcRequest& req) {
    auto it = request_handlers_.find(req.method);
    if (it == request_handlers_.end()) {
        JsonRpcErrorResponse err_resp;
        err_resp.id = req.id;
        err_resp.error.code = McpErrorCode::MethodNotFound;
        err_resp.error.message = "method not found: " + req.method;
        transport_->SendMessageAsync(JsonRpcMessage{std::move(err_resp)});
        return;
    }

    // ── Capability verification ──
    auto required_cap = RequiredClientCapability(req.method);
    if (required_cap) {
        bool has_capability = false;

        // 2026-era: per-request _meta
        auto meta = ExtractIncomingMeta(req);
        if (meta.client_capabilities) {
            const auto& caps = *meta.client_capabilities;
            if (*required_cap == "sampling" && caps.sampling) has_capability = true;
            if (*required_cap == "roots" && caps.roots) has_capability = true;
        }

        // 2025-era: stored from initialize
        if (!has_capability && client_capabilities_) {
            const auto& caps = *client_capabilities_;
            if (*required_cap == "sampling" && caps.sampling) has_capability = true;
            if (*required_cap == "roots" && caps.roots) has_capability = true;
        }

        if (!has_capability) {
            JsonRpcErrorResponse err_resp;
            err_resp.id = req.id;
            err_resp.error.code = McpErrorCode::MissingRequiredClientCapability;
            err_resp.error.message = "missing required client capability: " + *required_cap;
            nlohmann::json data = nlohmann::json::object();
            data["requiredCapabilities"] = nlohmann::json::array({*required_cap});
            err_resp.error.data = std::move(data);
            transport_->SendMessageAsync(JsonRpcMessage{std::move(err_resp)});
            return;
        }
    }

    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future = promise->get_future();

    it->second(req, std::move(*promise));

    auto self = shared_from_this();
    asio::post(io_ctx_, [self, req, future = std::move(future)]() mutable {
        try {
            auto result = future.get();
            if (self->closed_) return;

            // Apply outgoing filters for the response
            JsonRpcResponse resp;
            resp.id = req.id;
            resp.result = self->codec_->EncodeResult(req.method, result);

            JsonRpcMessage response_msg{std::move(resp)};

            if (self->outgoing_filters_) {
                self->outgoing_filters_->Execute(response_msg,
                    [self](const JsonRpcMessage& filtered) {
                        if (!self->closed_) self->transport_->SendMessageAsync(filtered);
                    });
            } else {
                self->transport_->SendMessageAsync(response_msg);
            }
        } catch (const McpError& e) {
            if (self->closed_) return;
            JsonRpcErrorResponse err_resp;
            err_resp.id = req.id;
            err_resp.error.code = e.Code();
            err_resp.error.message = e.what();
            self->transport_->SendMessageAsync(JsonRpcMessage{std::move(err_resp)});
        } catch (const std::exception& e) {
            if (self->closed_) return;
            JsonRpcErrorResponse err_resp;
            err_resp.id = req.id;
            err_resp.error.code = McpErrorCode::InternalError;
            err_resp.error.message = std::string("internal error: ") + e.what();
            self->transport_->SendMessageAsync(JsonRpcMessage{std::move(err_resp)});
        }
    });
}

// ═══════════════════════════════════════════════════════════════════════
// Response/Error handling (correlate with pending requests)
// ═══════════════════════════════════════════════════════════════════════
int64_t McpSessionHandler::GetNumericId(const RequestId& rid) {
    if (std::holds_alternative<int64_t>(rid))
        return std::get<int64_t>(rid);
    return static_cast<int64_t>(std::hash<std::string>{}(std::get<std::string>(rid)));
}

void McpSessionHandler::OnResponse(const JsonRpcResponse& resp) {
    auto id = GetNumericId(resp.id);
    std::lock_guard<std::mutex> lock(pending_mutex_);
    auto it = pending_.find(id);
    if (it != pending_.end()) {
        if (it->second && it->second->timer)
            it->second->timer->cancel();
        if (it->second)
            it->second->callback(resp.result);
        pending_.erase(it);
    }
}

void McpSessionHandler::OnError(const JsonRpcErrorResponse& err) {
    auto id = GetNumericId(err.id);
    std::lock_guard<std::mutex> lock(pending_mutex_);
    auto it = pending_.find(id);
    if (it != pending_.end()) {
        if (it->second && it->second->timer)
            it->second->timer->cancel();
        if (it->second) {
            nlohmann::json error_json;
            error_json["code"] = static_cast<int32_t>(err.error.code);
            error_json["message"] = err.error.message;
            if (err.error.data) error_json["data"] = *err.error.data;
            it->second->callback(std::move(error_json));
        }
        pending_.erase(it);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Notification handling
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::OnNotification(const JsonRpcNotification& notif) {
    // Handle protocol-level notifications first
    if (notif.method == notifications::kCancelled) {
        HandleCancelled(notif);
        return;
    }

    auto it = notif_handlers_.find(notif.method);
    if (it != notif_handlers_.end()) {
        it->second(notif);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Send request (with timeout)
// ═══════════════════════════════════════════════════════════════════════
std::future<nlohmann::json> McpSessionHandler::SendRequest(
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

    // Ensure params exists for meta stamping
    if (!req.params) req.params = nlohmann::json::object();

    // Stamp _meta for 2026 era
    StampOutgoingMeta(*req.params, meta);

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
                std::lock_guard<std::mutex> lock(pending_mutex_);
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

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_[id] = pending;
    }

    // Apply outgoing filters for the request
    JsonRpcMessage request_msg{std::move(req)};
    if (outgoing_filters_) {
        outgoing_filters_->Execute(request_msg,
            [this](const JsonRpcMessage& filtered) {
                if (!closed_) transport_->SendMessageAsync(filtered);
            });
    } else {
        transport_->SendMessageAsync(request_msg);
    }

    return future;
}

// ═══════════════════════════════════════════════════════════════════════
// Send notification
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::SendNotification(std::string_view method, nlohmann::json params) {
    JsonRpcNotification notif;
    notif.method = std::string(method);
    if (!params.is_null() && !params.empty()) {
        notif.params = std::move(params);
    }

    JsonRpcMessage msg{std::move(notif)};
    if (outgoing_filters_) {
        outgoing_filters_->Execute(msg,
            [this](const JsonRpcMessage& filtered) {
                if (!closed_) transport_->SendMessageAsync(filtered);
            });
    } else {
        transport_->SendMessageAsync(msg);
    }
}

void McpSessionHandler::SendMessage(JsonRpcMessage message) {
    if (outgoing_filters_) {
        outgoing_filters_->Execute(message,
            [this](const JsonRpcMessage& filtered) {
                if (!closed_) transport_->SendMessageAsync(filtered);
            });
    } else {
        transport_->SendMessageAsync(std::move(message));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Meta helpers
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::StampOutgoingMeta(nlohmann::json& body, const RequestMeta& meta) {
    if (meta.protocol_version < "2026-07-28")
        return;

    auto& meta_obj = body["_meta"];
    if (meta_obj.is_null()) {
        meta_obj = nlohmann::json::object();
    }
    meta_obj["io.modelcontextprotocol/protocolVersion"] = meta.protocol_version;
    if (meta.client_info)
        meta_obj["io.modelcontextprotocol/clientInfo"] = *meta.client_info;
    if (meta.client_capabilities)
        meta_obj["io.modelcontextprotocol/clientCapabilities"] = *meta.client_capabilities;
    if (meta.log_level)
        meta_obj["io.modelcontextprotocol/logLevel"] = *meta.log_level;
}

IncomingRequestMeta McpSessionHandler::ExtractIncomingMeta(const JsonRpcRequest& req) {
    IncomingRequestMeta meta;
    if (!req.meta) return meta;

    const auto& j = *req.meta;

    auto get_str = [&](const std::string& key) -> std::string {
        auto it = j.find(key);
        return (it != j.end()) ? it->get<std::string>() : "";
    };

    meta.protocol_version = get_str("io.modelcontextprotocol/protocolVersion");
    if (auto it = j.find("io.modelcontextprotocol/clientInfo"); it != j.end())
        meta.client_info = it->get<Implementation>();
    if (auto it = j.find("io.modelcontextprotocol/clientCapabilities"); it != j.end())
        meta.client_capabilities = it->get<ClientCapabilities>();
    if (auto it = j.find("io.modelcontextprotocol/logLevel"); it != j.end())
        meta.log_level = it->get<LoggingLevel>();
    if (auto it = j.find("progressToken"); it != j.end()) {
        const auto& val = *it;
        if (val.is_string())
            meta.progress_token = val.get<std::string>();
        else
            meta.progress_token = val.get<int64_t>();
    }
    if (auto it = j.find("io.modelcontextprotocol/subscriptionId"); it != j.end())
        meta.subscription_id = it->get<std::string>();

    return meta;
}

void McpSessionHandler::PopulateContextFromMeta(JsonRpcRequest& req) {
    // In C#: reads _meta and projects protocolVersion/clientInfo/clientCapabilities/logLevel into request.Context
    // For C++: we update the req.meta with extracted values
    // This is handled by ExtractIncomingMeta - kept for API compatibility
}

// ═══════════════════════════════════════════════════════════════════════
// Subscriptions
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::AddSubscription(Subscription sub) {
    subscriptions_[sub.id] = std::move(sub);
}

void McpSessionHandler::RemoveSubscription(std::string_view id) {
    subscriptions_.erase(std::string(id));
}

void McpSessionHandler::NotifySubscribers(std::string_view notification, nlohmann::json params) {
    if (subscriptions_.empty()) return;

    for (const auto& [id, sub] : subscriptions_) {
        JsonRpcNotification notif;
        notif.method = std::string(notification);
        notif.params = params;

        nlohmann::json meta = nlohmann::json::object();
        meta["io.modelcontextprotocol/subscriptionId"] = id;
        notif.meta = std::move(meta);

        transport_->SendMessageAsync(JsonRpcMessage{std::move(notif)});
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Cancel handling
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::HandleCancelled(const JsonRpcNotification& notif) {
    if (!notif.params) return;

    auto& p = *notif.params;
    auto req_id_it = p.find("requestId");
    if (req_id_it == p.end()) return;

    std::string reason;
    auto reason_it = p.find("reason");
    if (reason_it != p.end() && reason_it->is_string())
        reason = reason_it->get<std::string>();

    // Resolve the request ID (int64 or string)
    int64_t target_id = 0;
    if (req_id_it->is_number_integer()) {
        target_id = req_id_it->get<int64_t>();
    } else if (req_id_it->is_string()) {
        target_id = static_cast<int64_t>(
            std::hash<std::string>{}(req_id_it->get<std::string>()));
    } else {
        return;
    }

    std::lock_guard<std::mutex> lock(pending_mutex_);
    auto it = pending_.find(target_id);
    if (it != pending_.end() && it->second) {
        if (it->second->timer) it->second->timer->cancel();
        nlohmann::json err;
        err["code"] = static_cast<int32_t>(McpErrorCode::RequestCancelled);
        err["message"] = "request cancelled";
        if (!reason.empty())
            err["data"]["reason"] = reason;
        it->second->callback(std::move(err));
        pending_.erase(it);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Handler registration
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::SetRequestHandler(std::string_view method, RequestHandler handler) {
    request_handlers_[std::string(method)] = std::move(handler);
}

void McpSessionHandler::SetNotificationHandler(std::string_view method, NotificationHandler handler) {
    notif_handlers_[std::string(method)] = std::move(handler);
}

void McpSessionHandler::RemoveRequestHandler(std::string_view method) {
    request_handlers_.erase(std::string(method));
}

void McpSessionHandler::RemoveNotificationHandler(std::string_view method) {
    notif_handlers_.erase(std::string(method));
}

// ═══════════════════════════════════════════════════════════════════════
// Capability validation
// ═══════════════════════════════════════════════════════════════════════
std::optional<std::string> McpSessionHandler::RequiredClientCapability(std::string_view method) {
    if (method == methods::kCreateMessage) return std::string("sampling");
    if (method == methods::kListRoots) return std::string("roots");
    return std::nullopt;
}

void McpSessionHandler::SetClientCapabilities(ClientCapabilities caps) {
    client_capabilities_ = std::move(caps);
}

// ═══════════════════════════════════════════════════════════════════════
// Version
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::SetNegotiatedProtocolVersion(std::string_view version) {
    negotiated_version_ = std::string(version);
    codec_ = MakeWireCodec(version);
}

} // namespace mcp
