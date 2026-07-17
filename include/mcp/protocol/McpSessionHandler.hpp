#pragma once
#include <mcp/Export.hpp>
#include <mcp/protocol/McpSession.hpp>
#include <mcp/protocol/IncomingRequestMeta.hpp>
#include <mcp/protocol/MessageFilter.hpp>
#include <mcp/protocol/WireCodec.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/McpError.hpp>
#include <mcp/Methods.hpp>
#include <asio/steady_timer.hpp>
#include <asio/post.hpp>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>

namespace mcp {

// Forward declarations
class ITransport;
class MessageChannel;

// Handler type aliases
using RequestHandler = std::function<void(const JsonRpcRequest&, std::promise<nlohmann::json>)>;
using NotificationHandler = std::function<void(const JsonRpcNotification&)>;
using ResponseCallback = std::function<void(nlohmann::json)>;

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
    // Construct with transport, wire codec, and optional filter pipeline
    McpSessionHandler(
        std::shared_ptr<ITransport> transport,
        std::unique_ptr<WireCodec> codec,
        bool is_server,
        std::shared_ptr<FilterPipeline> incoming_filters = nullptr,
        std::shared_ptr<FilterPipeline> outgoing_filters = nullptr);

    ~McpSessionHandler();

    // ── Lifecycle ──
    void Start();
    void Close();
    bool IsRunning() const { return running_; }

    // ── Send ──
    std::future<nlohmann::json> SendRequest(
        std::string_view method,
        nlohmann::json params,
        const RequestMeta& meta = {},
        std::chrono::milliseconds timeout = std::chrono::seconds(30));

    void SendNotification(std::string_view method, nlohmann::json params = {});
    void SendMessage(JsonRpcMessage message);

    // ── Handler registration ──
    void SetRequestHandler(std::string_view method, RequestHandler handler);
    void SetNotificationHandler(std::string_view method, NotificationHandler handler);
    void RemoveRequestHandler(std::string_view method);
    void RemoveNotificationHandler(std::string_view method);

    // ── Capability validation ──
    static std::optional<std::string> RequiredClientCapability(std::string_view method);
    void SetClientCapabilities(ClientCapabilities caps);

    // ── Meta helpers (2026-era) ──
    void StampOutgoingMeta(nlohmann::json& body, const RequestMeta& meta);
    IncomingRequestMeta ExtractIncomingMeta(const JsonRpcRequest& req);

    // ── Subscription management ──
    void AddSubscription(Subscription sub);
    void AddSubscriptionEntry(SubscriptionEntry entry);
    void RemoveSubscription(std::string_view id);
    void NotifySubscribers(
        std::string_view notification_type,
        nlohmann::json params,
        std::optional<std::string> resource_uri = std::nullopt);

    // ── Error helper ──
    void SendErrorResponse(const RequestId& id, McpErrorCode code, std::string_view message, std::optional<nlohmann::json> data = std::nullopt);

    // ── Cancel ──
    void HandleCancelled(const JsonRpcNotification& notif);

    // ── Request state verification (HMAC/AEAD) ──
    void SetRequestStateVerifier(std::function<bool(std::string_view)> verifier);

    // ── Version negotiation ──
    void SetNegotiatedProtocolVersion(std::string_view version);

    // ── Protocol-era gates (semantic helpers, matching C# McpProtocolVersions) ──
    std::string_view NegotiatedProtocolVersion() const { return negotiated_version_; }
    bool IsJuly2026OrLater() const { return !negotiated_version_.empty() && negotiated_version_ >= "2026-07-28"; }
    WireCodec& GetCodec() { return *codec_; }
    ITransport& GetTransport() { return *transport_; }
    asio::io_context& IoContext() { return io_ctx_; }

private:
    // ── Message loop ──
    void MessageLoop();
    void DispatchMessage(const JsonRpcMessage& msg);

    // ── Message handlers ──
    void OnRequest(const JsonRpcRequest& req);
    void OnResponse(const JsonRpcResponse& resp);
    void OnError(const JsonRpcErrorResponse& err);
    void OnNotification(const JsonRpcNotification& notif);

    // ── Request/response correlation ──
    static std::string GetRequestIdKey(const RequestId& rid);
    std::atomic<int64_t> next_request_id_{1};

    // ── Members ──
    std::shared_ptr<ITransport> transport_;
    std::unique_ptr<WireCodec> codec_;
    bool is_server_;
    std::atomic<bool> running_{false};
    std::atomic<bool> closed_{false};
    std::string negotiated_version_;

    // Handler maps
    std::unordered_map<std::string, RequestHandler> request_handlers_;
    std::unordered_map<std::string, NotificationHandler> notif_handlers_;
    mutable std::shared_mutex handler_mutex_;

    // Pending request tracking (for response/error correlation)
    std::unordered_map<std::string, std::shared_ptr<PendingRequest>> pending_;
    std::mutex pending_mutex_;

    // Subscriptions
    std::unordered_map<std::string, SubscriptionEntry> subscriptions_;
    std::mutex subscriptions_mutex_;

    // Filter pipelines
    std::shared_ptr<FilterPipeline> incoming_filters_;
    std::shared_ptr<FilterPipeline> outgoing_filters_;

    // Request state verification callback
    std::function<bool(std::string_view)> request_state_verifier_;

    // Client capabilities (2025-era, set from initialize)
    std::optional<ClientCapabilities> client_capabilities_;

    asio::io_context& io_ctx_;
};

} // namespace mcp
