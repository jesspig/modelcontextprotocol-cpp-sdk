#pragma once

#include <mcp/Transport.hpp>
#include <mcp/protocol/WireCodec.hpp>
#include <mcp/protocol/IncomingRequestMeta.hpp>
#include <mcp/Methods.hpp>

#include <asio/steady_timer.hpp>
#include <asio/io_context.hpp>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mcp {

// ── Handler types ──
using RequestHandler = std::function<
    void(const JsonRpcRequest&, std::promise<nlohmann::json>)>;

using NotificationHandler = std::function<
    void(const JsonRpcNotification&)>;

// ── Callback for pending requests ──
using ResponseCallback = std::function<void(nlohmann::json)>;

// ── Protocol — base protocol engine ──
// Handles: JSON-RPC framing, request/response correlation,
//          timeout, handler dispatch, per-request _meta envelopes,
//          subscription/listen management, cancelled notification handling.
class Protocol : public std::enable_shared_from_this<Protocol> {
public:
    Protocol(
        asio::io_context& io_ctx,
        std::shared_ptr<ITransport> transport);

    ~Protocol();

    Protocol(const Protocol&) = delete;
    Protocol& operator=(const Protocol&) = delete;

    // ── Lifecycle ──
    void Start();
    void Close();

    // ── Send request (returns future) ──
    std::future<nlohmann::json> SendRequest(
        std::string_view method,
        nlohmann::json params,
        const RequestMeta& meta = {},
        std::chrono::milliseconds timeout = std::chrono::seconds(30));

    // ── Send notification (fire-and-forget) ──
    void SendNotification(
        std::string_view method,
        nlohmann::json params = {});

    // ── Per-request _meta extraction ──
    IncomingRequestMeta ExtractIncomingMeta(const JsonRpcRequest& req);

    // ── Subscription/listen management ──
    void AddSubscription(Subscription sub);
    void RemoveSubscription(std::string_view id);
    void NotifySubscribers(std::string_view notification, nlohmann::json params);

    // ── MRTR result type discrimination ──
    bool IsInputRequired(const nlohmann::json& result);

    // ── Cancelled notification handling ──
    void HandleCancelled(const JsonRpcNotification& notif);

    // ── Handler registration ──
    void SetRequestHandler(
        std::string_view method,
        RequestHandler handler);

    void SetNotificationHandler(
        std::string_view method,
        NotificationHandler handler);

    void RemoveRequestHandler(std::string_view method);
    void RemoveNotificationHandler(std::string_view method);

    // ── Accessors ──
    std::string_view NegotiatedProtocolVersion() const;
    void SetNegotiatedProtocolVersion(std::string_view version);

    std::unique_ptr<WireCodec>& Codec();
    const std::unique_ptr<WireCodec>& Codec() const;

    ITransport& GetTransport();
    asio::io_context& IoContext();

private:
    void MessageLoop();
    void DispatchMessage(const JsonRpcMessage& msg);
    void OnRequest(const JsonRpcRequest& req);
    void OnResponse(const JsonRpcResponse& resp);
    void OnError(const JsonRpcErrorResponse& err);
    void OnNotification(const JsonRpcNotification& notif);

    asio::io_context& io_ctx_;
    std::shared_ptr<ITransport> transport_;
    std::unique_ptr<WireCodec> codec_;
    std::string negotiated_version_;

    struct PendingRequest {
        ResponseCallback callback;
        std::shared_ptr<asio::steady_timer> timer;
    };
    std::unordered_map<int64_t, std::shared_ptr<PendingRequest>> pending_;
    std::unordered_map<std::string, RequestHandler> request_handlers_;
    std::unordered_map<std::string, NotificationHandler> notif_handlers_;

    int64_t next_request_id_ = 1;
    bool started_ = false;
    bool closed_ = false;

    std::unordered_map<std::string, Subscription> subscriptions_;
};

} // namespace mcp
