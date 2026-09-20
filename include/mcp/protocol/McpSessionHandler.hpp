#pragma once
#include <mcp/Export.hpp>
#include <mcp/protocol/IncomingRequestMeta.hpp>
#include <mcp/protocol/MessageFilter.hpp>
#include <mcp/protocol/WireCodec.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/McpError.hpp>
#include <mcp/Methods.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/ProtocolVersion.hpp>
#include <mcp/SpanHooks.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mcp {

class ITransport;
class MessageChannel;

inline constexpr std::chrono::milliseconds kDefaultRequestTimeout{60000};

using RequestHandler = std::function<void(const JsonRpcRequest&, std::promise<JsonValue>)>;
using NotificationHandler = std::function<void(const JsonRpcNotification&)>;
using ResponseCallback = std::function<void(JsonValue)>;

struct SubscriptionEntry {
    std::string id;
    std::string session_id;
    SubscriptionFilter filter;
    std::chrono::steady_clock::time_point created_at;
};

class MCP_API McpSessionHandler : public std::enable_shared_from_this<McpSessionHandler> {
public:
    inline static constexpr std::chrono::seconds kProgressTimeoutExtension{30};
    inline static constexpr std::chrono::milliseconds kTimeoutPollInterval{100};

    McpSessionHandler(
        std::shared_ptr<ITransport> transport,
        std::unique_ptr<WireCodec> codec,
        std::shared_ptr<FilterPipeline> incoming_filters = nullptr,
        std::shared_ptr<FilterPipeline> outgoing_filters = nullptr);

    ~McpSessionHandler();

    void Start();
    void Close();
    bool IsRunning() const { return running_; }

    std::future<JsonValue> SendRequest(
        std::string_view method,
        JsonValue params,
        const RequestMeta& meta = {},
        std::chrono::milliseconds timeout = kDefaultRequestTimeout);

    void SendNotification(std::string_view method, JsonValue params = {});
    void SendMessage(JsonRpcMessage message);

    void SetRequestHandler(std::string_view method, RequestHandler handler);
    void SetNotificationHandler(std::string_view method, NotificationHandler handler);

    static std::optional<std::string> RequiredClientCapability(std::string_view method);
    void SetClientCapabilities(ClientCapabilities caps);

    IncomingRequestMeta ExtractIncomingMeta(const JsonRpcRequest& req);

    void AddSubscription(Subscription sub);
    void AddSubscriptionEntry(SubscriptionEntry entry);
    void RemoveSubscription(std::string_view id);
    void NotifySubscribers(
        std::string_view notification_type,
        JsonValue params,
        std::optional<std::string> resource_uri = std::nullopt);

    void SendErrorResponse(const RequestId& id, McpErrorCode code, std::string_view message, std::optional<JsonValue> data = std::nullopt);

    void HandleCancelled(const JsonRpcNotification& notif);

    std::shared_ptr<std::atomic<bool>> GetIncomingCancellationFlag(const RequestId& id) const;

    void ResetTimeoutByProgressToken(const std::string& pt_key);

    void SetMaxTotalTimeout(std::chrono::seconds total);

    void SetSpanHandler(SpanHandler handler);

    void SetOnRequestCallback(std::function<void(std::string_view method, const JsonRpcRequest&)> cb);
    void SetOnResponseCallback(std::function<void(const JsonRpcResponse&)> cb);
    void SetOnErrorCallback(std::function<void(const JsonRpcErrorResponse&)> cb);
    void SetOnNotificationCallback(std::function<void(const JsonRpcNotification&)> cb);

    void SetRequestStateVerifier(std::function<bool(std::string_view)> verifier);

    void SetNegotiatedProtocolVersion(std::string_view version);

    std::string NegotiatedProtocolVersion() const;
    bool IsJuly2026OrLater() const {
        return mcp::IsModernProtocolVersion(NegotiatedProtocolVersion());
    }
    ITransport& GetTransport() { return *transport_; }

private:
    void MessageLoop();
    void DispatchMessage(const JsonRpcMessage& msg);

    void OnRequest(const JsonRpcRequest& req);
    void OnResponse(const JsonRpcResponse& resp);
    void OnError(const JsonRpcErrorResponse& err);
    void OnNotification(const JsonRpcNotification& notif);

    bool VerifyCapability(const JsonRpcRequest& req, const std::string& required);
    void EnqueueResponse(const JsonRpcRequest& req, std::future<JsonValue> future);

    struct PendingResponse {
        JsonRpcRequest req;
        std::future<JsonValue> future;
        std::string cancel_key;
    };

    void DeliverResponse(PendingResponse item);
    void ResponseWorkerLoop();

    static std::string GetRequestIdKey(const RequestId& rid);
    std::atomic<int64_t> next_request_id_{1};

    void CheckTimeouts();
    void EraseProgressTokens(const std::string& request_id);
    void EraseIncomingCancellationFlag(const std::string& request_id_key);

    std::shared_ptr<ITransport> transport_;
    std::shared_ptr<WireCodec> codec_;
    mutable std::shared_mutex codec_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> closed_{false};
    std::shared_ptr<const std::string> negotiated_version_;

    std::thread message_loop_thread_;
    std::thread timeout_thread_;
    std::thread response_worker_;

    std::unordered_map<std::string, RequestHandler> request_handlers_;
    std::unordered_map<std::string, NotificationHandler> notif_handlers_;
    mutable std::shared_mutex handler_mutex_;

    std::unordered_map<std::string, std::shared_ptr<PendingRequest>> pending_;
    std::mutex pending_mutex_;

    std::unordered_map<std::string, std::string> progress_token_map_;

    std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> incoming_cancel_flags_;
    mutable std::mutex incoming_cancel_mutex_;

    std::unordered_map<std::string, std::chrono::steady_clock::time_point> absolute_deadlines_;
    std::chrono::seconds max_total_timeout_{0};

    std::deque<PendingResponse> response_queue_;
    std::mutex response_queue_mutex_;
    std::condition_variable response_cv_;

    std::unordered_map<std::string, SubscriptionEntry> subscriptions_;
    std::mutex subscriptions_mutex_;

    std::shared_ptr<FilterPipeline> incoming_filters_;
    std::shared_ptr<FilterPipeline> outgoing_filters_;

    SpanHandler span_handler_;

    std::function<void(std::string_view, const JsonRpcRequest&)> on_request_cb_;
    std::function<void(const JsonRpcResponse&)> on_response_cb_;
    std::function<void(const JsonRpcErrorResponse&)> on_error_cb_;
    std::function<void(const JsonRpcNotification&)> on_notification_cb_;

    std::function<bool(std::string_view)> request_state_verifier_;

    std::optional<ClientCapabilities> client_capabilities_;
};

} // namespace mcp
