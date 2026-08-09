// McpSessionHandler.cpp
// Implementation of the JSON-RPC session handler
#include <detail/JsonFields.hpp>
#include <mcp/detail/ThreadUtils.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/protocol/MessageChannel.hpp>
#include <mcp/McpError.hpp>
#include <mcp/Methods.hpp>
#include <mcp/Log.hpp>
#include <mcp/Content.hpp>
#include <mcp/ProtocolVersion.hpp>

#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <system_error>
#include <thread>

namespace mcp {

// Forward declarations of core serialization helpers (defined in src/core)
JsonValue SerializeErrorData(const ErrorData& v);
JsonValue SerializeJsonRpcRequest(const JsonRpcRequest& v);
JsonValue SerializeJsonRpcNotification(const JsonRpcNotification& v);

namespace {

bool HasCapability(const ClientCapabilities& caps, const std::string& required) {
    if (required == "sampling") return caps.sampling.has_value();
    if (required == "roots") return caps.roots.has_value();
    return false;
}

template <typename Callable>
void InvokeSafely(Callable&& fn, std::string_view method_name) noexcept {
    try {
        fn();
    } catch (const std::exception& e) {
        LogContext ctx;
        ctx.method = method_name;
        MCP_LOG_CTX(Error, ctx, "callback threw: " + std::string(e.what()));
    } catch (...) {
        LogContext ctx;
        ctx.method = method_name;
        MCP_LOG_CTX(Error, ctx, "callback threw unknown exception");
    }
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════
// Construction
// ═══════════════════════════════════════════════════════════════════════
McpSessionHandler::McpSessionHandler(
    std::shared_ptr<ITransport> transport,
    std::unique_ptr<WireCodec> codec,
    std::shared_ptr<FilterPipeline> incoming_filters,
    std::shared_ptr<FilterPipeline> outgoing_filters)
    : transport_(std::move(transport))
    , codec_(std::move(codec))
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
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;

    auto self = shared_from_this();
    message_loop_thread_ = std::thread([self]() { self->MessageLoop(); });
    timeout_thread_ = std::thread([this]() {
        while (running_.load()) {
            std::this_thread::sleep_for(kTimeoutPollInterval);
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

    detail::JoinThreadSafely(message_loop_thread_);
    detail::JoinThreadSafely(timeout_thread_);

    // Fail all pending requests; callbacks fire outside the lock
    std::vector<std::shared_ptr<PendingRequest>> to_fire;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        to_fire.reserve(pending_.size());
        for (auto& [id, pending] : pending_) to_fire.push_back(std::move(pending));
        pending_.clear();
        progress_token_map_.clear();
    }
    for (auto& pending : to_fire) {
        if (pending) {
            InvokeSafely([&pending] {
                pending->callback(SerializeErrorData(
                    ErrorData{McpErrorCode::ConnectionClosed, "connection closed"}));
            }, "pending-callback");
        }
    }

    // Reap all async response tasks; every pending promise is satisfied above,
    // so each task completes once it observes closed_ and no future blocks here.
    {
        std::lock_guard<std::mutex> lock(response_mutex_);
        pending_responses_.clear();
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
    std::vector<std::shared_ptr<PendingRequest>> to_fire;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (auto it = pending_.begin(); it != pending_.end(); ) {
            if (now >= it->second->deadline) {
                to_fire.push_back(std::move(it->second));
                EraseProgressTokens(it->first);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& pending : to_fire) {
        if (pending) {
            InvokeSafely([&pending] {
                pending->callback(SerializeErrorData(
                    ErrorData{McpErrorCode::RequestTimeout, "request timed out"}));
            }, "timeout-callback");
        }
    }
    ReapCompletedResponses();
}

void McpSessionHandler::ResetTimeoutByProgressToken(const std::string& pt_key) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    auto it = progress_token_map_.find(pt_key);
    if (it != progress_token_map_.end()) {
        auto pit = pending_.find(it->second);
        if (pit != pending_.end()) {
            auto remaining = pit->second->deadline - std::chrono::steady_clock::now();
            if (remaining < kProgressTimeoutExtension) {
                pit->second->deadline = std::chrono::steady_clock::now() + kProgressTimeoutExtension;
            }
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
    std::shared_ptr<WireCodec> codec;
    {
        std::lock_guard<std::mutex> lock(codec_mutex_);
        codec = codec_;
    }
    auto validation = codec->ValidateRequest(req.method, SerializeJsonRpcRequest(req));
    // initialize is exempt: a modern server must still answer legacy handshakes
    if (validation == WireValidation::NotInEra && req.method != methods::kInitialize) {
        SendErrorResponse(req.id, McpErrorCode::MethodNotFound, "method not found: " + req.method);
        return;
    }
    if (validation == WireValidation::Invalid) {
        SendErrorResponse(req.id, McpErrorCode::InvalidRequest, "invalid request: " + req.method);
        return;
    }

    if (on_request_cb_) {
        InvokeSafely([&] { on_request_cb_(req.method, req); }, "on-request");
    }

    RequestHandler handler;
    {
        std::shared_lock<std::shared_mutex> lock(handler_mutex_);
        auto it = request_handlers_.find(req.method);
        if (it != request_handlers_.end()) {
            handler = it->second;
        }
    }
    if (!handler) {
        SendErrorResponse(req.id, McpErrorCode::MethodNotFound, "method not found: " + req.method);
        return;
    }

    // ── Capability verification ──
    auto required_cap = RequiredClientCapability(req.method);
    if (required_cap && !VerifyCapability(req, *required_cap)) return;

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
        handler(req, std::move(*promise));
    } catch (const McpError& e) {
        SendErrorResponse(req.id, e.Code(), std::string(e.what()));
        return;
    } catch (const std::exception& e) {
        SendErrorResponse(req.id, McpErrorCode::InternalError, std::string("handler error: ") + e.what());
        return;
    }

    SendResponseAsync(req, std::move(future));
}

bool McpSessionHandler::VerifyCapability(const JsonRpcRequest& req, const std::string& required) {
    auto meta = ExtractIncomingMeta(req);
    if (meta.client_capabilities && HasCapability(*meta.client_capabilities, required)) return true;
    if (client_capabilities_ && HasCapability(*client_capabilities_, required)) return true;

    JsonValue data(JsonValue::object_tag);
    JsonValue arr(JsonValue::array_tag);
    arr.PushBack(JsonValue(required));
    data["requiredCapabilities"] = std::move(arr);
    SendErrorResponse(req.id, McpErrorCode::MissingRequiredClientCapability,
        "missing required client capability: " + required, std::move(data));
    return false;
}

void McpSessionHandler::SendResponseAsync(const JsonRpcRequest& req, std::future<JsonValue> future) {
    auto self = shared_from_this();
    auto task = std::async(std::launch::async, [self, req, future = std::move(future)]() mutable {
        std::shared_ptr<WireCodec> codec;
        {
            std::lock_guard<std::mutex> lock(self->codec_mutex_);
            codec = self->codec_;
        }
        // Wait for the handler's promise, aborting once the session closes so
        // Close() never blocks on a promise that is never satisfied.
        while (future.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
            if (self->closed_.load()) return;
        }
        try {
            auto result = future.get();
            if (self->closed_.load()) return;

            JsonRpcResponse resp;
            resp.id = req.id;
            resp.result = codec->EncodeResult(req.method, result);
            self->SendMessage(JsonRpcMessage{std::move(resp)});
        } catch (const McpError& e) {
            if (self->closed_.load()) return;
            JsonRpcErrorResponse err_resp;
            err_resp.id = req.id;
            err_resp.error.code = static_cast<McpErrorCode>(
                codec->EncodeErrorCode(static_cast<int32_t>(e.Code())));
            err_resp.error.message = e.what();
            self->SendMessage(JsonRpcMessage{std::move(err_resp)});
        } catch (const std::exception& e) {
            if (self->closed_.load()) return;
            JsonRpcErrorResponse err_resp;
            err_resp.id = req.id;
            err_resp.error.code = static_cast<McpErrorCode>(
                codec->EncodeErrorCode(static_cast<int32_t>(McpErrorCode::InternalError)));
            err_resp.error.message = std::string("internal error: ") + e.what();
            self->SendMessage(JsonRpcMessage{std::move(err_resp)});
        }
    });

    std::lock_guard<std::mutex> lock(response_mutex_);
    pending_responses_.push_back(std::move(task));
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
        InvokeSafely([&] { on_response_cb_(resp); }, "on-response");
    }

    auto id = GetRequestIdKey(resp.id);
    std::shared_ptr<PendingRequest> pending;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_.find(id);
        if (it != pending_.end()) {
            pending = std::move(it->second);
            EraseProgressTokens(id);
            pending_.erase(it);
        }
    }
    if (pending) {
        InvokeSafely([&] { pending->callback(resp.result); }, "response-callback");
    }
}

void McpSessionHandler::OnError(const JsonRpcErrorResponse& err) {
    if (on_error_cb_) {
        InvokeSafely([&] { on_error_cb_(err); }, "on-error");
    }

    if (!err.id) return;
    auto id = GetRequestIdKey(*err.id);
    std::shared_ptr<PendingRequest> pending;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_.find(id);
        if (it != pending_.end()) {
            pending = std::move(it->second);
            EraseProgressTokens(id);
            pending_.erase(it);
        }
    }
    if (pending) {
        auto error_json = SerializeErrorData(err.error);
        InvokeSafely([&] { pending->callback(std::move(error_json)); }, "error-callback");
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Notification handling
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::OnNotification(const JsonRpcNotification& notif) {
    std::shared_ptr<WireCodec> codec;
    {
        std::lock_guard<std::mutex> lock(codec_mutex_);
        codec = codec_;
    }
    auto validation = codec->ValidateNotification(notif.method, SerializeJsonRpcNotification(notif));
    if (validation != WireValidation::Ok) {
        LogContext ctx;
        ctx.method = notif.method;
        MCP_LOG_CTX(Warning, ctx, "dropping notification: not valid for the current protocol era");
        return;
    }

    if (on_notification_cb_) {
        InvokeSafely([&] { on_notification_cb_(notif); }, "on-notification");
    }

    // Handle protocol-level notifications first
    if (notif.method == notifications::kCancelled) {
        HandleCancelled(notif);
        return;
    }

    NotificationHandler handler;
    {
        std::shared_lock<std::shared_mutex> lock(handler_mutex_);
        auto it = notif_handlers_.find(notif.method);
        if (it != notif_handlers_.end()) {
            handler = it->second;
        }
    }
    if (handler) {
        handler(notif);
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

    // Ensure params is an object (matches the pre-meta-stamping wire format)
    if (req.params->IsNull()) *req.params = JsonValue(JsonValue::object_tag);

    // Stamp _meta at the top level for 2026 era (serialized from req.meta)
    if (IsModernProtocolVersion(meta.protocol_version)) {
        req.meta = SerializeRequestMeta(meta);
    }

    // Register pending request
    auto pending = std::make_shared<PendingRequest>();
    pending->callback = [promise](JsonValue result) {
        promise->set_value(std::move(result));
    };
    pending->deadline = std::chrono::steady_clock::now() + timeout;
    pending->progress_token = meta.progress_token;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_[std::to_string(id)] = pending;
        if (meta.progress_token) {
            auto pt_key = std::visit([](const auto& v) -> std::string {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>) return v;
                else return std::to_string(v);
            }, *meta.progress_token);
            progress_token_map_[pt_key] = std::to_string(id);
        }
    }

    SendMessage(JsonRpcMessage{std::move(req)});
    return future;
}

// ═══════════════════════════════════════════════════════════════════════
// Send notification
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::SendNotification(std::string_view method, JsonValue params) {
    JsonRpcNotification notif;
    notif.method = std::string(method);
    if (IsModernProtocolVersion(negotiated_version_)) {
        RequestMeta meta;
        meta.protocol_version = negotiated_version_;
        notif.meta = SerializeRequestMeta(meta);
    }
    if (!params.IsNull() && !params.Empty()) {
        notif.params = std::move(params);
    }

    SendMessage(JsonRpcMessage{std::move(notif)});
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
IncomingRequestMeta McpSessionHandler::ExtractIncomingMeta(const JsonRpcRequest& req) {
    IncomingRequestMeta meta;
    if (!req.meta) return meta;

    try {
        auto core_meta = DeserializeRequestMeta(*req.meta);
        meta.protocol_version = core_meta.protocol_version;
        meta.client_info = std::move(core_meta.client_info);
        meta.client_capabilities = std::move(core_meta.client_capabilities);
        meta.log_level = core_meta.log_level;
        meta.progress_token = core_meta.progress_token;
        meta.traceparent = std::move(core_meta.traceparent);
        meta.tracestate = std::move(core_meta.tracestate);
        meta.baggage = std::move(core_meta.baggage);
    } catch (const std::exception& e) {
        LogContext ctx;
        ctx.method = req.method;
        MCP_LOG_CTX(Warning, ctx, "failed to parse incoming _meta: " + std::string(e.what()));
        return IncomingRequestMeta{};
    }

    if (auto* sid = req.meta->Find(detail::kMetaSubscriptionIdKey)) {
        if (sid->IsString()) {
            meta.subscription_id = sid->GetString();
        } else {
            LogContext ctx;
            ctx.method = req.method;
            MCP_LOG_CTX(Warning, ctx, "subscriptionId is not a string; ignored");
        }
    }
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
    AddSubscriptionEntry(std::move(entry));
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
    std::vector<JsonRpcMessage> outgoing;
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
            meta[detail::kMetaSubscriptionIdKey] = JsonValue(id);
            notif.meta = std::move(meta);

            outgoing.emplace_back(std::move(notif));
        }
    }

    for (auto& msg : outgoing) {
        transport_->SendMessageAsync(std::move(msg));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Error response helper
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::SendErrorResponse(const RequestId& id, McpErrorCode code, std::string_view message, std::optional<JsonValue> data) {
    std::shared_ptr<WireCodec> codec;
    {
        std::lock_guard<std::mutex> lock(codec_mutex_);
        codec = codec_;
    }
    JsonRpcErrorResponse err_resp;
    err_resp.id = id;
    err_resp.error.code = static_cast<McpErrorCode>(
        codec->EncodeErrorCode(static_cast<int32_t>(code)));
    err_resp.error.message = std::string(message);
    if (data) err_resp.error.data = std::move(*data);
    SendMessage(JsonRpcMessage{std::move(err_resp)});
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

    std::shared_ptr<PendingRequest> pending;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_.find(target_id_key);
        if (it != pending_.end()) {
            pending = std::move(it->second);
            EraseProgressTokens(target_id_key);
            pending_.erase(it);
        }
    }
    if (pending) {
        std::optional<JsonValue> data;
        if (!reason.empty()) {
            JsonValue d(JsonValue::object_tag);
            d["reason"] = JsonValue(reason);
            data = std::move(d);
        }
        auto err = SerializeErrorData(
            ErrorData{McpErrorCode::RequestCancelled, "request cancelled", std::move(data)});
        InvokeSafely([&] { pending->callback(std::move(err)); }, "cancel-callback");
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Internal helpers
// ═══════════════════════════════════════════════════════════════════════
void McpSessionHandler::EraseProgressTokens(const std::string& request_id) {
    for (auto it = progress_token_map_.begin(); it != progress_token_map_.end(); ) {
        if (it->second == request_id) it = progress_token_map_.erase(it);
        else ++it;
    }
}

void McpSessionHandler::ReapCompletedResponses() {
    std::lock_guard<std::mutex> lock(response_mutex_);
    for (auto it = pending_responses_.begin(); it != pending_responses_.end(); ) {
        if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            it = pending_responses_.erase(it);
        else
            ++it;
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
    auto new_codec = MakeWireCodec(version);
    std::lock_guard<std::mutex> lock(codec_mutex_);
    negotiated_version_ = std::string(version);
    codec_ = std::move(new_codec);
}

} // namespace mcp
