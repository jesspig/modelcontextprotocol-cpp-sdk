#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/protocol/MessageChannel.hpp>
#include <mcp/McpError.hpp>
#include <mcp/Methods.hpp>
#include <mcp/Log.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <system_error>
#include <thread>

namespace mcp {

namespace {

JsonValue ImplToJson(const Implementation& v) {
    JsonValue j(JsonValue::object_tag);
    j["name"] = JsonValue(v.name);
    j["version"] = JsonValue(v.version);
    if (v.title)       j["title"] = JsonValue(*v.title);
    if (v.description) j["description"] = JsonValue(*v.description);
    if (v.website_url) j["websiteUrl"] = JsonValue(*v.website_url);
    return j;
}
Implementation ImplFromJson(const JsonValue& j) {
    Implementation v;
    v.name = j.At("name").GetString();
    v.version = j.At("version").GetString();
    if (auto* t = j.Find("title"))       v.title = t->GetString();
    if (auto* d = j.Find("description")) v.description = d->GetString();
    if (auto* w = j.Find("websiteUrl"))  v.website_url = w->GetString();
    return v;
}
JsonValue CapsToJson(const ClientCapabilities& v) {
    JsonValue j(JsonValue::object_tag);
    if (v.roots)     { JsonValue sub(JsonValue::object_tag); j["roots"] = std::move(sub); }
    if (v.sampling)  { JsonValue sub(JsonValue::object_tag); j["sampling"] = std::move(sub); }
    return j;
}
ClientCapabilities CapsFromJson(const JsonValue& j) {
    ClientCapabilities v;
    if (j.Find("roots"))    v.roots = RootsCapability{};
    if (j.Find("sampling")) v.sampling = SamplingCapability{};
    return v;
}
JsonValue LogLevelToJson(LoggingLevel l) {
    static const char* names[] = {"debug","info","notice","warning","error","critical","alert","emergency"};
    auto i = static_cast<int>(l);
    return JsonValue((i >= 0 && i < 8) ? names[i] : "debug");
}
LoggingLevel LogLevelFromJson(const JsonValue& j) {
    auto s = j.GetString();
    static const char* names[] = {"debug","info","notice","warning","error","critical","alert","emergency"};
    for (int i = 0; i < 8; ++i) { if (s == names[i]) return static_cast<LoggingLevel>(i); }
    return LoggingLevel::Debug;
}

} // anonymous namespace

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
{
}

McpSessionHandler::~McpSessionHandler() {
    Close();
}

// ═══════════════════════════════════════════════════════════════════════
// Lifecycle
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::Start() {
    if (running_.load()) return;
    running_.store(true);

    auto self = shared_from_this();
    message_loop_thread_ = std::thread([self]() { self->MessageLoop(); });
    timeout_thread_ = std::thread([this]() {
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            CheckTimeouts();
        }
    });
}

void McpSessionHandler::Close() {
    if (closed_.load()) return;
    closed_.store(true);
    running_.store(false);

    // Wake up message loop by closing the channel
    transport_->GetMessageChannel().Close();

    if (message_loop_thread_.joinable()) message_loop_thread_.join();
    if (timeout_thread_.joinable()) timeout_thread_.join();

    // Fail all pending requests
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        JsonValue err(JsonValue::object_tag);
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
    auto& channel = transport_->GetMessageChannel();
    while (!closed_.load()) {
        std::error_code ec;
        JsonRpcMessage msg;
        channel.AsyncReceive([&](std::error_code recv_ec, JsonRpcMessage recv_msg) {
            ec = recv_ec;
            msg = std::move(recv_msg);
        });
        if (ec || closed_.load()) break;
        CheckTimeouts();
        if (incoming_filters_) {
            incoming_filters_->Execute(msg,
                [this](const JsonRpcMessage& filtered_msg) {
                    if (!closed_.load()) DispatchMessage(filtered_msg);
                });
        } else {
            DispatchMessage(msg);
        }
    }
}

void McpSessionHandler::CheckTimeouts() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(pending_mutex_);
    for (auto it = pending_.begin(); it != pending_.end(); ) {
        if (now >= it->second->deadline) {
            JsonValue err(JsonValue::object_tag);
            err["code"] = static_cast<int32_t>(McpErrorCode::RequestTimeout);
            err["message"] = "request timed out";
            try { it->second->callback(err); } catch (...) {}
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
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
    if (on_request_cb_) {
        try { on_request_cb_(req.method, req); } catch (...) {}
    }

    std::shared_lock<std::shared_mutex> lock(handler_mutex_);
    auto it = request_handlers_.find(req.method);
    if (it == request_handlers_.end()) {
        SendErrorResponse(req.id, McpErrorCode::MethodNotFound, "method not found: " + req.method);
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
            JsonValue data(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            arr.PushBack(JsonValue(*required_cap));
            data["requiredCapabilities"] = std::move(arr);
            SendErrorResponse(req.id, McpErrorCode::MissingRequiredClientCapability, "missing required client capability: " + *required_cap, std::move(data));
            return;
        }
    }

    // ── Request state verification (HMAC/AEAD) ──
    if (request_state_verifier_ && req.params) {
        auto* rs = req.params->Find("requestState");
        if (rs && rs->IsString()) {
            if (!request_state_verifier_(rs->GetString())) {
                SendErrorResponse(req.id, McpErrorCode::InvalidParams, "invalid requestState");
                return;
            }
        }
    }

    auto promise = std::make_shared<std::promise<JsonValue>>();
    auto future = promise->get_future();

    try {
        it->second(req, std::move(*promise));
    } catch (const std::exception& e) {
        SendErrorResponse(req.id, McpErrorCode::InternalError, std::string("handler error: ") + e.what());
        return;
    }

    auto self = shared_from_this();
    [[maybe_unused]] auto _ = std::async(std::launch::async, [self, req, future = std::move(future)]() mutable {
        try {
            auto result = future.get();
            if (self->closed_.load()) return;

            JsonRpcResponse resp;
            resp.id = req.id;
            resp.result = self->codec_->EncodeResult(req.method, result);

            JsonRpcMessage response_msg{std::move(resp)};

            if (self->outgoing_filters_) {
                self->outgoing_filters_->Execute(response_msg,
                    [self](const JsonRpcMessage& filtered) {
                        if (!self->closed_.load()) self->transport_->SendMessageAsync(filtered);
                    });
            } else {
                self->transport_->SendMessageAsync(response_msg);
            }
        } catch (const McpError& e) {
            if (self->closed_.load()) return;
            JsonRpcErrorResponse err_resp;
            err_resp.id = req.id;
            err_resp.error.code = e.Code();
            err_resp.error.message = e.what();
            self->transport_->SendMessageAsync(JsonRpcMessage{std::move(err_resp)});
        } catch (const std::exception& e) {
            if (self->closed_.load()) return;
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
std::string McpSessionHandler::GetRequestIdKey(const RequestId& rid) {
    if (std::holds_alternative<int64_t>(rid))
        return std::to_string(std::get<int64_t>(rid));
    return std::get<std::string>(rid);
}

void McpSessionHandler::OnResponse(const JsonRpcResponse& resp) {
    if (on_response_cb_) {
        try { on_response_cb_(resp); } catch (...) {}
    }

    auto id = GetRequestIdKey(resp.id);
    std::lock_guard<std::mutex> lock(pending_mutex_);
    auto it = pending_.find(id);
    if (it != pending_.end()) {
        if (it->second)
            it->second->callback(resp.result);
        pending_.erase(it);
    }
}

void McpSessionHandler::OnError(const JsonRpcErrorResponse& err) {
    if (on_error_cb_) {
        try { on_error_cb_(err); } catch (...) {}
    }

    if (!err.id) return;
    auto id = GetRequestIdKey(*err.id);
    std::lock_guard<std::mutex> lock(pending_mutex_);
    auto it = pending_.find(id);
    if (it != pending_.end()) {
        if (it->second) {
            JsonValue error_json(JsonValue::object_tag);
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
    if (on_notification_cb_) {
        try { on_notification_cb_(notif); } catch (...) {}
    }

    // Handle protocol-level notifications first
    if (notif.method == notifications::kCancelled) {
        HandleCancelled(notif);
        return;
    }

    std::shared_lock<std::shared_mutex> lock(handler_mutex_);
    auto it = notif_handlers_.find(notif.method);
    if (it != notif_handlers_.end()) {
        it->second(notif);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Send request (with timeout)
// ═══════════════════════════════════════════════════════════════════════
std::future<JsonValue> McpSessionHandler::SendRequest(
    std::string_view method,
    JsonValue params,
    const RequestMeta& meta,
    std::chrono::milliseconds timeout)
{
    auto promise = std::make_shared<std::promise<JsonValue>>();
    auto future = promise->get_future();

    auto id = next_request_id_++;

    JsonRpcRequest req;
    req.id = RequestId{id};
    req.method = std::string(method);
    req.params = std::move(params);

    // Ensure params is an object for meta stamping
    if (req.params->IsNull()) *req.params = JsonValue(JsonValue::object_tag);

    // Stamp _meta for 2026 era
    StampOutgoingMeta(*req.params, meta);

    // Register pending request
    auto pending = std::make_shared<PendingRequest>();
    pending->callback = [promise](JsonValue result) {
        promise->set_value(std::move(result));
    };
    pending->deadline = std::chrono::steady_clock::now() + timeout;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_[std::to_string(id)] = pending;
    }

    // Apply outgoing filters for the request
    JsonRpcMessage request_msg{std::move(req)};
    if (outgoing_filters_) {
        auto self = shared_from_this();
        outgoing_filters_->Execute(request_msg,
            [self](const JsonRpcMessage& filtered) {
                if (!self->closed_.load()) self->transport_->SendMessageAsync(filtered);
            });
    } else {
        transport_->SendMessageAsync(request_msg);
    }

    return future;
}

// ═══════════════════════════════════════════════════════════════════════
// Send notification
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::SendNotification(std::string_view method, JsonValue params) {
    JsonRpcNotification notif;
    notif.method = std::string(method);
    if (!params.IsNull() && !params.Empty()) {
        notif.params = std::move(params);
    }

    JsonRpcMessage msg{std::move(notif)};
    if (outgoing_filters_) {
        auto self = shared_from_this();
        outgoing_filters_->Execute(msg,
            [self](const JsonRpcMessage& filtered) {
                if (!self->closed_.load()) self->transport_->SendMessageAsync(filtered);
            });
    } else {
        transport_->SendMessageAsync(msg);
    }
}

void McpSessionHandler::SendMessage(JsonRpcMessage message) {
    if (outgoing_filters_) {
        auto self = shared_from_this();
        outgoing_filters_->Execute(message,
            [self](const JsonRpcMessage& filtered) {
                if (!self->closed_.load()) self->transport_->SendMessageAsync(filtered);
            });
    } else {
        transport_->SendMessageAsync(std::move(message));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Meta helpers
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::StampOutgoingMeta(JsonValue& body, const RequestMeta& meta) {
    if (meta.protocol_version < "2026-07-28") {
        LogContext ctx;
        ctx.method = "stamp-outgoing";
        MCP_LOG_CTX(Trace, ctx, "skipped outgoing meta stamping: proto=" + meta.protocol_version);
        return;
    }

    JsonValue meta_obj(JsonValue::object_tag);
    meta_obj["io.modelcontextprotocol/protocolVersion"] = JsonValue(meta.protocol_version);
    if (meta.client_info)
        meta_obj["io.modelcontextprotocol/clientInfo"] = ImplToJson(*meta.client_info);
        if (meta.client_capabilities)
            meta_obj["io.modelcontextprotocol/clientCapabilities"] = CapsToJson(*meta.client_capabilities);
        if (meta.log_level)
            meta_obj["io.modelcontextprotocol/logLevel"] = LogLevelToJson(*meta.log_level);
    body["_meta"] = std::move(meta_obj);

    LogContext ctx;
    ctx.method = "stamp-outgoing";
    MCP_LOG_CTX(Trace, ctx, "stamped outgoing _meta: proto=" + meta.protocol_version);
}

IncomingRequestMeta McpSessionHandler::ExtractIncomingMeta(const JsonRpcRequest& req) {
    auto rid_key = GetRequestIdKey(req.id);
    LogContext ctx;
    ctx.request_id = rid_key;
    ctx.method = req.method;

    IncomingRequestMeta meta;
    if (!req.meta) {
        MCP_LOG_CTX(Trace, ctx, "no _meta in request");
        return meta;
    }

    const auto& j = *req.meta;

    auto get_str = [&](const std::string& key) -> std::string {
        auto* it = j.Find(key);
        return it ? it->GetString() : "";
    };

    meta.protocol_version = get_str("io.modelcontextprotocol/protocolVersion");
    if (auto* it = j.Find("io.modelcontextprotocol/clientInfo"))
        meta.client_info = ImplFromJson(*it);
        if (auto it = j.Find("io.modelcontextprotocol/clientCapabilities"); it)
            meta.client_capabilities = CapsFromJson(*it);
        if (auto it = j.Find("io.modelcontextprotocol/logLevel"); it)
            meta.log_level = LogLevelFromJson(*it);
    if (auto* pt = j.Find("progressToken")) {
        if (pt->IsString())
            meta.progress_token = pt->GetString();
        else
            meta.progress_token = pt->GetInt();
    }
    if (auto* sid = j.Find("io.modelcontextprotocol/subscriptionId"))
        meta.subscription_id = sid->GetString();

    MCP_LOG_CTX(Trace, ctx, "extracted incoming meta: proto=" + meta.protocol_version +
        " has_client_info=" + (meta.client_info ? "yes" : "no"));
    return meta;
}


// ═══════════════════════════════════════════════════════════════════════
// Subscriptions
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::AddSubscription(Subscription sub) {
    SubscriptionEntry entry;
    entry.id = std::move(sub.id);
    entry.filter = std::move(sub.granted);
    entry.created_at = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    subscriptions_[entry.id] = std::move(entry);
}

void McpSessionHandler::AddSubscriptionEntry(SubscriptionEntry entry) {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    subscriptions_[entry.id] = std::move(entry);
}

void McpSessionHandler::RemoveSubscription(std::string_view id) {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    subscriptions_.erase(std::string(id));
}

void McpSessionHandler::NotifySubscribers(
    std::string_view notification_type,
    JsonValue params,
    std::optional<std::string> resource_uri)
{
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    if (subscriptions_.empty()) return;

    for (const auto& [id, entry] : subscriptions_) {
        bool should_notify = false;

        if (notification_type == notifications::kToolListChanged) {
            should_notify = entry.filter.tools_list_changed.value_or(false);
        } else if (notification_type == notifications::kPromptListChanged) {
            should_notify = entry.filter.prompts_list_changed.value_or(false);
        } else if (notification_type == notifications::kResourceListChanged) {
            should_notify = entry.filter.resources_list_changed.value_or(false);
        } else if (notification_type == notifications::kResourceUpdated) {
            if (resource_uri && !entry.filter.resource_subscriptions.empty()) {
                for (const auto& uri : entry.filter.resource_subscriptions) {
                    if (uri == *resource_uri) {
                        should_notify = true;
                        break;
                    }
                }
            }
        }

        if (!should_notify) continue;

        JsonRpcNotification notif;
        notif.method = std::string(notification_type);
        notif.params = params;

        JsonValue meta(JsonValue::object_tag);
        meta["io.modelcontextprotocol/subscriptionId"] = JsonValue(id);
        notif.meta = std::move(meta);

        transport_->SendMessageAsync(JsonRpcMessage{std::move(notif)});
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Error response helper
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::SendErrorResponse(const RequestId& id, McpErrorCode code, std::string_view message, std::optional<JsonValue> data) {
    JsonRpcErrorResponse err_resp;
    err_resp.id = id;
    err_resp.error.code = code;
    err_resp.error.message = std::string(message);
    if (data) err_resp.error.data = std::move(*data);
    transport_->SendMessageAsync(JsonRpcMessage{std::move(err_resp)});
}

// ═══════════════════════════════════════════════════════════════════════
// Cancel handling
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::HandleCancelled(const JsonRpcNotification& notif) {
    if (!notif.params) return;

    auto* req_id_val = notif.params->Find("requestId");
    if (!req_id_val) return;

    std::string reason;
    auto* reason_val = notif.params->Find("reason");
    if (reason_val && reason_val->IsString())
        reason = reason_val->GetString();

    std::string target_id_key;
    if (req_id_val->IsInt()) {
        target_id_key = std::to_string(req_id_val->GetInt());
    } else if (req_id_val->IsString()) {
        target_id_key = req_id_val->GetString();
    } else {
        return;
    }

    std::lock_guard<std::mutex> lock(pending_mutex_);
    auto it = pending_.find(target_id_key);
    if (it != pending_.end() && it->second) {
        JsonValue err(JsonValue::object_tag);
        err["code"] = static_cast<int32_t>(McpErrorCode::RequestCancelled);
        err["message"] = "request cancelled";
        if (!reason.empty()) {
            JsonValue data(JsonValue::object_tag);
            data["reason"] = JsonValue(reason);
            err["data"] = std::move(data);
        }
        it->second->callback(std::move(err));
        pending_.erase(it);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Handler registration
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::SetRequestHandler(std::string_view method, RequestHandler handler) {
    std::unique_lock<std::shared_mutex> lock(handler_mutex_);
    request_handlers_[std::string(method)] = std::move(handler);
}

void McpSessionHandler::SetNotificationHandler(std::string_view method, NotificationHandler handler) {
    std::unique_lock<std::shared_mutex> lock(handler_mutex_);
    notif_handlers_[std::string(method)] = std::move(handler);
}

void McpSessionHandler::RemoveRequestHandler(std::string_view method) {
    std::unique_lock<std::shared_mutex> lock(handler_mutex_);
    request_handlers_.erase(std::string(method));
}

void McpSessionHandler::RemoveNotificationHandler(std::string_view method) {
    std::unique_lock<std::shared_mutex> lock(handler_mutex_);
    notif_handlers_.erase(std::string(method));
}

void McpSessionHandler::SetRequestStateVerifier(std::function<bool(std::string_view)> verifier) {
    request_state_verifier_ = std::move(verifier);
}

void McpSessionHandler::SetOnRequestCallback(std::function<void(std::string_view, const JsonRpcRequest&)> cb) {
    on_request_cb_ = std::move(cb);
}

void McpSessionHandler::SetOnResponseCallback(std::function<void(const JsonRpcResponse&)> cb) {
    on_response_cb_ = std::move(cb);
}

void McpSessionHandler::SetOnErrorCallback(std::function<void(const JsonRpcErrorResponse&)> cb) {
    on_error_cb_ = std::move(cb);
}

void McpSessionHandler::SetOnNotificationCallback(std::function<void(const JsonRpcNotification&)> cb) {
    on_notification_cb_ = std::move(cb);
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
