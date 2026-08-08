#pragma once
// McpSessionHandler.hpp
// Internal JSON-RPC engine for message dispatch, request/response correlation, and filter pipelines
#include <mcp/Export.hpp>
#include <mcp/protocol/McpSession.hpp>
#include <mcp/protocol/IncomingRequestMeta.hpp>
#include <mcp/protocol/MessageFilter.hpp>
#include <mcp/protocol/WireCodec.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/McpError.hpp>
#include <mcp/Methods.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/ProtocolVersion.hpp>

#include <chrono>
#include <future>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>

namespace mcp {

// Forward declarations
class ITransport;
class MessageChannel;

// Handler type aliases
using RequestHandler = std::function<void(const JsonRpcRequest&, std::promise<JsonValue>)>;
using NotificationHandler = std::function<void(const JsonRpcNotification&)>;
using ResponseCallback = std::function<void(JsonValue)>;

// Enhanced subscription entry with 2026-era filter support
struct SubscriptionEntry {
    std::string id;
    std::string session_id;
    SubscriptionFilter filter;
    std::chrono::steady_clock::time_point created_at;
};

// ═══════════════════════════════════════════════════════════════════════
// McpSessionHandler — internal JSON-RPC engine
// ═══════════════════════════════════════════════════════════════════════
// This is the C++ equivalent of the C# McpSessionHandler class.
// It handles all message dispatching, request/response correlation,
// cancellation, and filter pipelines.
class MCP_API McpSessionHandler : public std::enable_shared_from_this<McpSessionHandler> {
public:
    inline static constexpr std::chrono::seconds kProgressTimeoutExtension{30};
    inline static constexpr std::chrono::milliseconds kTimeoutPollInterval{100};

    // Construct with transport, wire codec, and optional filter pipeline
    McpSessionHandler(
        std::shared_ptr<ITransport> transport,
        std::unique_ptr<WireCodec> codec,
        std::shared_ptr<FilterPipeline> incoming_filters = nullptr,
        std::shared_ptr<FilterPipeline> outgoing_filters = nullptr);

    ~McpSessionHandler();

    // ── Lifecycle ──
    void Start();
    void Close();
    bool IsRunning() const { return running_; }

    // ── Send ──
    std::future<JsonValue> SendRequest(
        std::string_view method,
        JsonValue params,
        const RequestMeta& meta = {},
        std::chrono::milliseconds timeout = kDefaultRequestTimeout);

    void SendNotification(std::string_view method, JsonValue params = {});
    void SendMessage(JsonRpcMessage message);

    // ── Handler registration ──
    void SetRequestHandler(std::string_view method, RequestHandler handler);
    void SetNotificationHandler(std::string_view method, NotificationHandler handler);
    void RemoveRequestHandler(std::string_view method);
    void RemoveNotificationHandler(std::string_view method);

    // ── Capability validation ──
    // SetClientCapabilities must be called before Start(); the message loop
    // reads client_capabilities_ without synchronization.
    static std::optional<std::string> RequiredClientCapability(std::string_view method);
    void SetClientCapabilities(ClientCapabilities caps);

    // ── Meta helpers (2026-era) ──
    IncomingRequestMeta ExtractIncomingMeta(const JsonRpcRequest& req);

    // ── Subscription management ──
    // AddSubscription converts a Subscription and delegates to AddSubscriptionEntry.
    void AddSubscription(Subscription sub);
    void AddSubscriptionEntry(SubscriptionEntry entry);
    void RemoveSubscription(std::string_view id);
    void NotifySubscribers(
        std::string_view notification_type,
        JsonValue params,
        std::optional<std::string> resource_uri = std::nullopt);

    // ── Error helper ──
    void SendErrorResponse(const RequestId& id, McpErrorCode code, std::string_view message, std::optional<JsonValue> data = std::nullopt);

    // ── Cancel ──
    void HandleCancelled(const JsonRpcNotification& notif);

    // ── Progress tracking ──
    void ResetTimeoutByProgressToken(const std::string& pt_key);

    // ── Event callbacks ──
    void SetOnRequestCallback(std::function<void(std::string_view method, const JsonRpcRequest&)> cb);
    void SetOnResponseCallback(std::function<void(const JsonRpcResponse&)> cb);
    void SetOnErrorCallback(std::function<void(const JsonRpcErrorResponse&)> cb);
    void SetOnNotificationCallback(std::function<void(const JsonRpcNotification&)> cb);

    // ── Request state verification (HMAC/AEAD) ──
    // Must be called before Start(); the message loop reads the verifier
    // without synchronization.
    void SetRequestStateVerifier(std::function<bool(std::string_view)> verifier);

    // ── Version negotiation ──
    // Must be called before Start() (or before Close() re-Starts); the message
    // loop reads negotiated_version_ without synchronization.
    void SetNegotiatedProtocolVersion(std::string_view version);

    // ── Protocol-era gates (semantic helpers, matching C# McpProtocolVersions) ──
    std::string_view NegotiatedProtocolVersion() const { return negotiated_version_; }
    bool IsJuly2026OrLater() const { return mcp::IsModernProtocolVersion(negotiated_version_); }
    WireCodec& GetCodec() { return *codec_; }
    ITransport& GetTransport() { return *transport_; }

private:
    // ── Message loop ──
    void MessageLoop();
    void DispatchMessage(const JsonRpcMessage& msg);

    // ── Message handlers ──
    void OnRequest(const JsonRpcRequest& req);
    void OnResponse(const JsonRpcResponse& resp);
    void OnError(const JsonRpcErrorResponse& err);
    void OnNotification(const JsonRpcNotification& notif);

    // ── Request handling helpers ──
    bool VerifyCapability(const JsonRpcRequest& req, const std::string& required);
    void SendResponseAsync(const JsonRpcRequest& req, std::future<JsonValue> future);

    // ── Request/response correlation ──
    static std::string GetRequestIdKey(const RequestId& rid);
    std::atomic<int64_t> next_request_id_{1};

    // ── Internal ──
    void CheckTimeouts();
    void EraseProgressTokens(const std::string& request_id);
    void ReapCompletedResponses();

    // ── Members ──
    std::shared_ptr<ITransport> transport_;
    std::unique_ptr<WireCodec> codec_;
    std::atomic<bool> running_{false};
    std::atomic<bool> closed_{false};
    std::string negotiated_version_;

    // Threads
    std::thread message_loop_thread_;
    std::thread timeout_thread_;

    // Handler maps
    std::unordered_map<std::string, RequestHandler> request_handlers_;
    std::unordered_map<std::string, NotificationHandler> notif_handlers_;
    mutable std::shared_mutex handler_mutex_;

    // Pending request tracking (for response/error correlation)
    std::unordered_map<std::string, std::shared_ptr<PendingRequest>> pending_;
    std::mutex pending_mutex_;

    // Progress token → request_id mapping (for timeout reset)
    std::unordered_map<std::string, std::string> progress_token_map_;

    // Async response tasks; reaped once their future is ready so the
    // destructor never blocks on an in-flight task.
    std::vector<std::future<void>> pending_responses_;
    std::mutex response_mutex_;

    // Subscriptions
    std::unordered_map<std::string, SubscriptionEntry> subscriptions_;
    std::mutex subscriptions_mutex_;

    // Filter pipelines
    std::shared_ptr<FilterPipeline> incoming_filters_;
    std::shared_ptr<FilterPipeline> outgoing_filters_;

    // Event callbacks
    std::function<void(std::string_view, const JsonRpcRequest&)> on_request_cb_;
    std::function<void(const JsonRpcResponse&)> on_response_cb_;
    std::function<void(const JsonRpcErrorResponse&)> on_error_cb_;
    std::function<void(const JsonRpcNotification&)> on_notification_cb_;

    // Request state verification callback
    std::function<bool(std::string_view)> request_state_verifier_;

    // Client capabilities (2025-era, set from initialize)
    std::optional<ClientCapabilities> client_capabilities_;
};

} // namespace mcp
