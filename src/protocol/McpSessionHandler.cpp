#include <detail/JsonFields.hpp>
#include <mcp/detail/ThreadUtils.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/protocol/MessageChannel.hpp>
#include <mcp/Transport.hpp>
#include <mcp/McpError.hpp>
#include <mcp/Methods.hpp>
#include <mcp/Log.hpp>
#include <mcp/Content.hpp>
#include <mcp/ProtocolVersion.hpp>

#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

namespace mcp {

JsonValue SerializeErrorData(const ErrorData& v);

namespace {

constexpr std::chrono::milliseconds kResponsePollInterval{10};

bool HasCapability(const ClientCapabilities& caps, const std::string& required) {
    if (required == "sampling") return caps.sampling.has_value();
    if (required == "roots") return caps.roots.has_value();
    return false;
}

constexpr char kErrorDataSupported[] = "supported";
constexpr char kErrorDataRequested[] = "requested";

bool IsSupportedProtocolVersion(std::string_view version) {
    for (auto known : kProtocolVersions) {
        if (known == version) return true;
    }
    return false;
}

JsonValue MakeUnsupportedProtocolVersionData(const std::string& requested) {
    JsonValue supported(JsonValue::array_tag);
    for (auto known : kProtocolVersions) {
        supported.PushBack(JsonValue(std::string(known)));
    }
    JsonValue data(JsonValue::object_tag);
    data[kErrorDataSupported] = std::move(supported);
    data[kErrorDataRequested] = JsonValue(requested);
    return data;
}

const char* InvalidMetaField(const JsonValue& meta) {
    const JsonValue* value = nullptr;
    if ((value = meta.Find(detail::kMetaProtocolVersionKey)) && !value->IsString())
        return detail::kMetaProtocolVersionKey;
    if ((value = meta.Find(detail::kProgressToken)) && !value->IsString() && !value->IsInt())
        return detail::kProgressToken;
    if ((value = meta.Find(detail::kMetaClientInfoKey)) && !value->IsObject())
        return detail::kMetaClientInfoKey;
    if ((value = meta.Find(detail::kMetaClientCapabilitiesKey)) && !value->IsObject())
        return detail::kMetaClientCapabilitiesKey;
    if ((value = meta.Find(detail::kMetaLogLevelKey)) && !value->IsString())
        return detail::kMetaLogLevelKey;
    if ((value = meta.Find(detail::kTraceparent)) && !value->IsString())
        return detail::kTraceparent;
    if ((value = meta.Find(detail::kTracestate)) && !value->IsString())
        return detail::kTracestate;
    if ((value = meta.Find(detail::kBaggage)) && !value->IsString())
        return detail::kBaggage;
    return nullptr;
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

std::string RequestIdToText(const RequestId& rid) {
    if (const auto* text = std::get_if<std::string>(&rid)) return *text;
    if (const auto* number = std::get_if<int64_t>(&rid)) return std::to_string(*number);
    return {};
}

void AttachTraceContext(SpanEvent& event, const IncomingRequestMeta& meta) {
    if (meta.traceparent) event.traceparent = *meta.traceparent;
    if (meta.tracestate) event.tracestate = *meta.tracestate;
    if (meta.baggage) event.baggage = *meta.baggage;
}

SpanEvent MakeServerRequestEvent(const JsonRpcRequest& req, SpanPhase phase, bool failed) {
    SpanEvent event;
    event.phase = phase;
    event.kind = SpanKind::ServerRequest;
    event.name = req.method;
    event.method = req.method;
    event.request_id = RequestIdToText(req.id);
    event.failed = failed;
    return event;
}

SpanEvent MakeTransportEvent(SpanKind kind, const JsonRpcMessage& msg) {
    SpanEvent event;
    event.kind = kind;
    if (const auto* request = AsRequest(msg)) {
        event.name = request->method;
        event.method = request->method;
        event.request_id = RequestIdToText(request->id);
    } else if (const auto* notification = AsNotification(msg)) {
        event.name = notification->method;
        event.method = notification->method;
    } else if (const auto* response = AsResponse(msg)) {
        event.request_id = RequestIdToText(response->id);
    } else if (const auto* error = AsError(msg)) {
        event.request_id = error->id ? RequestIdToText(*error->id) : std::string{};
        event.failed = true;
    }
    return event;
}

void InvokeSpan(const SpanHandler& handler, SpanEvent& event, SpanPhase phase) {
    event.phase = phase;
    InvokeSafely([&] { handler(event); }, "span-hook");
}

class SpanScope {
public:
    SpanScope() noexcept = default;
    SpanScope(const SpanHandler& handler, SpanEvent event)
        : handler_(&handler), event_(std::move(event)) {
        InvokeSpan(*handler_, *event_, SpanPhase::Begin);
    }
    SpanScope(SpanScope&& other) noexcept
        : handler_(other.handler_), event_(std::move(other.event_)) {
        other.handler_ = nullptr;
    }
    SpanScope& operator=(SpanScope&& other) noexcept {
        if (this != &other) {
            Close();
            handler_ = other.handler_;
            event_ = std::move(other.event_);
            other.handler_ = nullptr;
        }
        return *this;
    }
    ~SpanScope() { Close(); }
    SpanScope(const SpanScope&) = delete;
    SpanScope& operator=(const SpanScope&) = delete;

private:
    void Close() {
        if (handler_ && event_) InvokeSpan(*handler_, *event_, SpanPhase::End);
        handler_ = nullptr;
    }

    const SpanHandler* handler_ = nullptr;
    std::optional<SpanEvent> event_;
};

} // anonymous namespace

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

void McpSessionHandler::Start() {
    if (closed_.load()) {
        throw std::logic_error("McpSessionHandler::Start() called after Close()");
    }
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
    response_worker_ = std::thread([this]() { ResponseWorkerLoop(); });
}

void McpSessionHandler::Close() {
    if (closed_.load()) return;
    closed_.store(true);
    running_.store(false);

    transport_->GetMessageChannel().Close();

    detail::JoinThreadSafely(message_loop_thread_);
    detail::JoinThreadSafely(timeout_thread_);

    {
        std::lock_guard<std::mutex> lock(response_queue_mutex_);
    }
    response_cv_.notify_all();
    detail::JoinThreadSafely(response_worker_);

    std::vector<std::shared_ptr<PendingRequest>> to_fire;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        to_fire.reserve(pending_.size());
        for (auto& [id, pending] : pending_) to_fire.push_back(std::move(pending));
        pending_.clear();
        progress_token_map_.clear();
        absolute_deadlines_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(incoming_cancel_mutex_);
        incoming_cancel_flags_.clear();
    }
    for (auto& pending : to_fire) {
        if (pending) {
            InvokeSafely([&pending] {
                pending->callback(SerializeErrorData(
                    ErrorData{McpErrorCode::ConnectionClosed, "connection closed"}));
            }, "pending-callback");
        }
    }

    transport_->Close();
}

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
        SpanScope span;
        if (span_handler_) span = SpanScope(span_handler_, MakeTransportEvent(SpanKind::TransportReceive, msg));
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
            auto abs_it = absolute_deadlines_.find(it->first);
            const bool total_expired =
                abs_it != absolute_deadlines_.end() && now >= abs_it->second;
            if (total_expired || now >= it->second->deadline) {
                to_fire.push_back(std::move(it->second));
                EraseProgressTokens(it->first);
                absolute_deadlines_.erase(it->first);
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

void McpSessionHandler::SetMaxTotalTimeout(std::chrono::seconds total) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    max_total_timeout_ = total;
}

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

void McpSessionHandler::OnRequest(const JsonRpcRequest& req) {
    bool span_open = false;
    if (span_handler_) {
        auto event = MakeServerRequestEvent(req, SpanPhase::Begin, false);
        AttachTraceContext(event, ExtractIncomingMeta(req));
        InvokeSpan(span_handler_, event, SpanPhase::Begin);
        span_open = true;
    }
    const struct SpanGuard {
        const SpanHandler& handler;
        const JsonRpcRequest& req;
        bool& open;
        ~SpanGuard() {
            if (!open) return;
            auto event = MakeServerRequestEvent(req, SpanPhase::End, true);
            InvokeSpan(handler, event, SpanPhase::End);
        }
    } span_guard{span_handler_, req, span_open};

    std::shared_ptr<WireCodec> codec;
    {
        std::shared_lock<std::shared_mutex> lock(codec_mutex_);
        codec = codec_;
    }
    JsonValue validation_view(JsonValue::object_tag);
    validation_view[detail::kMethod] = JsonValue(req.method);
    if (req.method == methods::kInitialize && req.params)
        validation_view[detail::kParams] = *req.params;
    if (req.meta) {
        JsonValue& view_params = validation_view[detail::kParams];
        if (!view_params.IsObject()) view_params = JsonValue(JsonValue::object_tag);
        view_params[detail::kMeta] = *req.meta;
    }
    auto validation = codec->ValidateRequest(req.method, validation_view);
    if (validation == WireValidation::NotInEra && req.method != methods::kInitialize) {
        SendErrorResponse(req.id, McpErrorCode::MethodNotFound, "method not found: " + req.method);
        return;
    }
    if (validation == WireValidation::Invalid) {
        SendErrorResponse(req.id, McpErrorCode::InvalidRequest, "invalid request: " + req.method);
        return;
    }

    if (req.meta && req.method != methods::kInitialize) {
        if (const char* field = InvalidMetaField(*req.meta)) {
            SendErrorResponse(req.id, McpErrorCode::InvalidParams,
                std::string("invalid _meta field: ") + field);
            return;
        }
        if (auto* declared = req.meta->Find(detail::kMetaProtocolVersionKey)) {
            const std::string requested = declared->GetString();
            if (!IsSupportedProtocolVersion(requested)) {
                SendErrorResponse(req.id, McpErrorCode::UnsupportedProtocolVersion,
                    "unsupported protocol version: " + requested,
                    MakeUnsupportedProtocolVersionData(requested));
                return;
            }
        }
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

    auto required_cap = RequiredClientCapability(req.method);
    if (required_cap && !VerifyCapability(req, *required_cap)) return;

    if (request_state_verifier_ && req.params) {
        auto* rs = req.params->Find(detail::kRequestState);
        if (rs && rs->IsString()) {
            if (!request_state_verifier_(rs->GetString())) {
                JsonValue data(JsonValue::object_tag);
                data[detail::kReason] = JsonValue("invalid_request_state");
                SendErrorResponse(req.id, McpErrorCode::InvalidParams,
                    "invalid requestState", std::move(data));
                return;
            }
        }
    }

    const std::string cancel_key = GetRequestIdKey(req.id);
    {
        std::lock_guard<std::mutex> lock(incoming_cancel_mutex_);
        incoming_cancel_flags_[cancel_key] = std::make_shared<std::atomic<bool>>(false);
    }

    auto promise = std::make_shared<std::promise<JsonValue>>();
    auto future = promise->get_future();

    try {
        handler(req, std::move(*promise));
    } catch (const McpError& e) {
        EraseIncomingCancellationFlag(cancel_key);
        SendErrorResponse(req.id, e.Code(), std::string(e.what()));
        return;
    } catch (const std::exception& e) {
        EraseIncomingCancellationFlag(cancel_key);
        SendErrorResponse(req.id, McpErrorCode::InternalError, std::string("handler error: ") + e.what());
        return;
    }

    span_open = false;
    EnqueueResponse(req, std::move(future));
}

bool McpSessionHandler::VerifyCapability(const JsonRpcRequest& req, const std::string& required) {
    auto meta = ExtractIncomingMeta(req);
    if (meta.client_capabilities && HasCapability(*meta.client_capabilities, required)) return true;
    if (client_capabilities_ && HasCapability(*client_capabilities_, required)) return true;

    JsonValue data(JsonValue::object_tag);
    JsonValue arr(JsonValue::array_tag);
    arr.PushBack(JsonValue(required));
    data[detail::kRequiredCapabilities] = std::move(arr);
    SendErrorResponse(req.id, McpErrorCode::MissingRequiredClientCapability,
        "missing required client capability: " + required, std::move(data));
    return false;
}

void McpSessionHandler::EnqueueResponse(const JsonRpcRequest& req, std::future<JsonValue> future) {
    PendingResponse item;
    item.req = req;
    item.cancel_key = GetRequestIdKey(req.id);
    item.future = std::move(future);
    {
        std::lock_guard<std::mutex> lock(response_queue_mutex_);
        if (closed_.load()) return;
        response_queue_.push_back(std::move(item));
    }
    response_cv_.notify_one();
}

void McpSessionHandler::DeliverResponse(PendingResponse item) {
    if (closed_.load()) return;
    EraseIncomingCancellationFlag(item.cancel_key);
    std::shared_ptr<WireCodec> codec;
    {
        std::shared_lock<std::shared_mutex> lock(codec_mutex_);
        codec = codec_;
    }
    const auto finish_span = [this, &item](bool failed) {
        if (!span_handler_) return;
        auto event = MakeServerRequestEvent(item.req, SpanPhase::End, failed);
        InvokeSpan(span_handler_, event, SpanPhase::End);
    };
    try {
        auto result = item.future.get();
        if (closed_.load()) return;

        JsonRpcResponse resp;
        resp.id = item.req.id;
        resp.result = codec->EncodeResult(item.req.method, result);
        finish_span(false);
        SendMessage(JsonRpcMessage{std::move(resp)});
    } catch (const McpError& e) {
        if (closed_.load()) return;
        JsonRpcErrorResponse err_resp;
        err_resp.id = item.req.id;
        err_resp.error.code = static_cast<McpErrorCode>(
            codec->EncodeErrorCode(static_cast<int32_t>(e.Code())));
        err_resp.error.message = e.what();
        finish_span(true);
        SendMessage(JsonRpcMessage{std::move(err_resp)});
    } catch (const std::exception& e) {
        if (closed_.load()) return;
        JsonRpcErrorResponse err_resp;
        err_resp.id = item.req.id;
        err_resp.error.code = static_cast<McpErrorCode>(
            codec->EncodeErrorCode(static_cast<int32_t>(McpErrorCode::InternalError)));
        err_resp.error.message = std::string("internal error: ") + e.what();
        finish_span(true);
        SendMessage(JsonRpcMessage{std::move(err_resp)});
    }
}

void McpSessionHandler::ResponseWorkerLoop() {
    std::deque<PendingResponse> waiting;
    for (;;) {
        std::deque<PendingResponse> ready;
        {
            std::unique_lock<std::mutex> lock(response_queue_mutex_);
            while (!response_queue_.empty()) {
                waiting.push_back(std::move(response_queue_.front()));
                response_queue_.pop_front();
            }
            if (closed_.load()) {
                waiting.clear();
                return;
            }
            if (waiting.empty()) {
                response_cv_.wait(lock, [this] {
                    return !response_queue_.empty() || closed_.load();
                });
                continue;
            }
        }
        for (auto it = waiting.begin(); it != waiting.end();) {
            if (it->future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                ready.push_back(std::move(*it));
                it = waiting.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& item : ready) DeliverResponse(std::move(item));
        if (!ready.empty()) continue;
        std::unique_lock<std::mutex> lock(response_queue_mutex_);
        if (closed_.load()) {
            waiting.clear();
            return;
        }
        response_cv_.wait_for(lock, kResponsePollInterval, [this] {
            return !response_queue_.empty() || closed_.load();
        });
    }
}

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
            absolute_deadlines_.erase(id);
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
            absolute_deadlines_.erase(id);
            pending_.erase(it);
        }
    }
    if (pending) {
        auto error_json = SerializeErrorData(err.error);
        InvokeSafely([&] { pending->callback(std::move(error_json)); }, "error-callback");
    }
}

void McpSessionHandler::OnNotification(const JsonRpcNotification& notif) {
    std::shared_ptr<WireCodec> codec;
    {
        std::shared_lock<std::shared_mutex> lock(codec_mutex_);
        codec = codec_;
    }
    JsonValue validation_view(JsonValue::object_tag);
    validation_view[detail::kMethod] = JsonValue(notif.method);
    if (notif.params) validation_view[detail::kParams] = *notif.params;
    auto validation = codec->ValidateNotification(notif.method, validation_view);
    if (validation != WireValidation::Ok) {
        LogContext ctx;
        ctx.method = notif.method;
        MCP_LOG_CTX(Warning, ctx, "dropping notification: not valid for the current protocol era");
        return;
    }

    if (on_notification_cb_) {
        InvokeSafely([&] { on_notification_cb_(notif); }, "on-notification");
    }

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

    if (req.params->IsNull()) *req.params = JsonValue(JsonValue::object_tag);

    if (IsModernProtocolVersion(meta.protocol_version)) {
        req.meta = SerializeRequestMeta(meta);
    } else if (meta.progress_token) {
        JsonValue* legacy_meta = req.params->Find(detail::kMeta);
        if (!legacy_meta) {
            (*req.params)[detail::kMeta] = JsonValue(JsonValue::object_tag);
            legacy_meta = req.params->Find(detail::kMeta);
        }
        (*legacy_meta)[detail::kProgressToken] = SerializeProgressToken(*meta.progress_token);
    }

    auto pending = std::make_shared<PendingRequest>();
    pending->callback = [promise](JsonValue result) {
        promise->set_value(std::move(result));
    };
    pending->deadline = std::chrono::steady_clock::now() + timeout;
    pending->progress_token = meta.progress_token;

    const std::string id_key = std::to_string(id);
    std::optional<std::string> pt_key;
    if (meta.progress_token) {
        pt_key = std::holds_alternative<std::string>(*meta.progress_token)
            ? std::get<std::string>(*meta.progress_token)
            : std::to_string(std::get<int64_t>(*meta.progress_token));
    }

    bool closed_after_register = false;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (closed_.load()) {
            closed_after_register = true;
        } else {
            pending_[id_key] = pending;
            if (pt_key) progress_token_map_[*pt_key] = id_key;
            if (max_total_timeout_ > std::chrono::seconds{0}) {
                absolute_deadlines_[id_key] =
                    std::chrono::steady_clock::now() + max_total_timeout_;
            }
        }
    }

    if (closed_after_register) {
        pending->callback(SerializeErrorData(
            ErrorData{McpErrorCode::ConnectionClosed, "connection closed"}));
        return future;
    }

    SendMessage(JsonRpcMessage{std::move(req)});
    return future;
}

void McpSessionHandler::SendNotification(std::string_view method, JsonValue params) {
    JsonRpcNotification notif;
    notif.method = std::string(method);
    std::shared_ptr<const std::string> version;
    {
        std::shared_lock<std::shared_mutex> lock(codec_mutex_);
        version = negotiated_version_;
    }
    if (version && IsModernProtocolVersion(*version)) {
        RequestMeta meta;
        meta.protocol_version = *version;
        notif.meta = SerializeRequestMeta(meta);
    }
    if (!params.IsNull() && !params.Empty()) {
        notif.params = std::move(params);
    }

    SendMessage(JsonRpcMessage{std::move(notif)});
}

void McpSessionHandler::SendMessage(JsonRpcMessage message) {
    SpanScope span;
    if (span_handler_) span = SpanScope(span_handler_, MakeTransportEvent(SpanKind::TransportSend, message));
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

        JsonRpcNotification skeleton;
        skeleton.method = std::string(notification_type);
        skeleton.params = std::move(params);
        skeleton.meta = JsonValue(JsonValue::object_tag);

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

            JsonRpcNotification notif = skeleton;
            std::string_view sub_id =
                entry.session_id.empty() ? id : entry.session_id;
            (*notif.meta)[detail::kMetaSubscriptionIdKey] =
                JsonValue(std::string(sub_id));
            outgoing.emplace_back(std::move(notif));
        }
    }

    for (auto& msg : outgoing) {
        transport_->SendMessageAsync(std::move(msg));
    }
}

void McpSessionHandler::SendErrorResponse(const RequestId& id, McpErrorCode code, std::string_view message, std::optional<JsonValue> data) {
    std::shared_ptr<WireCodec> codec;
    {
        std::shared_lock<std::shared_mutex> lock(codec_mutex_);
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

void McpSessionHandler::HandleCancelled(const JsonRpcNotification& notif) {
    if (!notif.params) return;

    auto* req_id_val = notif.params->Find(detail::kRequestId);
    if (!req_id_val) return;

    std::string reason;
    auto* reason_val = notif.params->Find(detail::kReason);
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

    std::shared_ptr<std::atomic<bool>> incoming_flag;
    {
        std::lock_guard<std::mutex> lock(incoming_cancel_mutex_);
        auto it = incoming_cancel_flags_.find(target_id_key);
        if (it != incoming_cancel_flags_.end()) incoming_flag = it->second;
    }
    if (incoming_flag) {
        incoming_flag->store(true);
    }

    std::shared_ptr<PendingRequest> pending;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_.find(target_id_key);
        if (it != pending_.end()) {
            pending = std::move(it->second);
            EraseProgressTokens(target_id_key);
            absolute_deadlines_.erase(target_id_key);
            pending_.erase(it);
        }
    }
    if (pending) {
        std::optional<JsonValue> data;
        if (!reason.empty()) {
            JsonValue d(JsonValue::object_tag);
            d[detail::kReason] = JsonValue(reason);
            data = std::move(d);
        }
        auto err = SerializeErrorData(
            ErrorData{McpErrorCode::RequestCancelled, "request cancelled", std::move(data)});
        InvokeSafely([&] { pending->callback(std::move(err)); }, "cancel-callback");
    }
}

void McpSessionHandler::EraseProgressTokens(const std::string& request_id) {
    for (auto it = progress_token_map_.begin(); it != progress_token_map_.end(); ) {
        if (it->second == request_id) it = progress_token_map_.erase(it);
        else ++it;
    }
}

void McpSessionHandler::EraseIncomingCancellationFlag(const std::string& request_id_key) {
    std::lock_guard<std::mutex> lock(incoming_cancel_mutex_);
    incoming_cancel_flags_.erase(request_id_key);
}

std::shared_ptr<std::atomic<bool>> McpSessionHandler::GetIncomingCancellationFlag(
    const RequestId& id) const
{
    const std::string key = GetRequestIdKey(id);
    std::lock_guard<std::mutex> lock(incoming_cancel_mutex_);
    auto it = incoming_cancel_flags_.find(key);
    if (it == incoming_cancel_flags_.end()) return nullptr;
    return it->second;
}

void McpSessionHandler::SetRequestHandler(std::string_view method, RequestHandler handler) {
    std::unique_lock<std::shared_mutex> lock(handler_mutex_);
    request_handlers_[std::string(method)] = std::move(handler);
}

void McpSessionHandler::SetNotificationHandler(std::string_view method, NotificationHandler handler) {
    std::unique_lock<std::shared_mutex> lock(handler_mutex_);
    notif_handlers_[std::string(method)] = std::move(handler);
}

void McpSessionHandler::SetRequestStateVerifier(std::function<bool(std::string_view)> verifier) {
    request_state_verifier_ = std::move(verifier);
}

void McpSessionHandler::SetSpanHandler(SpanHandler handler) {
    span_handler_ = std::move(handler);
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

std::optional<std::string> McpSessionHandler::RequiredClientCapability(std::string_view method) {
    if (method == methods::kCreateMessage) return std::string("sampling");
    if (method == methods::kListRoots) return std::string("roots");
    return std::nullopt;
}

void McpSessionHandler::SetClientCapabilities(ClientCapabilities caps) {
    client_capabilities_ = std::move(caps);
}

void McpSessionHandler::SetNegotiatedProtocolVersion(std::string_view version) {
    auto new_codec = MakeWireCodec(version);
    std::unique_lock<std::shared_mutex> lock(codec_mutex_);
    negotiated_version_ = std::make_shared<const std::string>(version);
    codec_ = std::move(new_codec);
}

std::string McpSessionHandler::NegotiatedProtocolVersion() const {
    std::shared_ptr<const std::string> version;
    {
        std::shared_lock<std::shared_mutex> lock(codec_mutex_);
        version = negotiated_version_;
    }
    return version ? *version : std::string();
}

} // namespace mcp
