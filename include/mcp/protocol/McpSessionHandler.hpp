#pragma once
#include <mcp/protocol/McpSession.hpp>
#include <mcp/protocol/IncomingRequestMeta.hpp>
#include <mcp/protocol/MessageFilter.hpp>
#include <mcp/protocol/WireCodec.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/McpError.hpp>
#include <mcp/Methods.hpp>
#include <asio/steady_timer.hpp>
#include <asio/post.hpp>
#include <mutex>
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

// ═══════════════════════════════════════════════════════════════════════
// McpSessionHandler — internal JSON-RPC engine
// ═══════════════════════════════════════════════════════════════════════
// This is the C++ equivalent of the C# McpSessionHandler class.
// It handles all message dispatching, request/response correlation,
// cancellation, and filter pipelines.
class McpSessionHandler : public std::enable_shared_from_this<McpSessionHandler> {
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
    static void PopulateContextFromMeta(JsonRpcRequest& req);

    // ── Subscription management ──
    void AddSubscription(Subscription sub);
    void RemoveSubscription(std::string_view id);
    void NotifySubscribers(std::string_view notification, nlohmann::json params);

    // ── Cancel ──
    void HandleCancelled(const JsonRpcNotification& notif);

    // ── Version negotiation ──
    void SetNegotiatedProtocolVersion(std::string_view version);

    // ── Accessors ──
    std::string_view NegotiatedProtocolVersion() const { return negotiated_version_; }
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
    int64_t GetNumericId(const RequestId& rid);
    int64_t next_request_id_ = 1;

    // ── Members ──
    std::shared_ptr<ITransport> transport_;
    std::unique_ptr<WireCodec> codec_;
    bool is_server_;
    bool running_ = false;
    bool closed_ = false;
    std::string negotiated_version_;

    // Handler maps
    std::unordered_map<std::string, RequestHandler> request_handlers_;
    std::unordered_map<std::string, NotificationHandler> notif_handlers_;

    // Pending request tracking (for response/error correlation)
    std::unordered_map<int64_t, std::shared_ptr<PendingRequest>> pending_;
    std::mutex pending_mutex_;

    // Subscriptions
    std::unordered_map<std::string, Subscription> subscriptions_;

    // Filter pipelines
    std::shared_ptr<FilterPipeline> incoming_filters_;
    std::shared_ptr<FilterPipeline> outgoing_filters_;

    // Client capabilities (2025-era, set from initialize)
    std::optional<ClientCapabilities> client_capabilities_;

    asio::io_context& io_ctx_;
};

} // namespace mcp
