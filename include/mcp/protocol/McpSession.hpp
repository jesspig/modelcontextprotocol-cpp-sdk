#pragma once
#include <mcp/JsonRpc.hpp>
#include <mcp/Transport.hpp>
#include <mcp/ProtocolVersion.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/Capabilities.hpp>
#include <mcp/Meta.hpp>
#include <memory>
#include <string_view>
#include <functional>
#include <future>

namespace mcp {

// Forward declarations
class ITransport;
class MessageChannel;
class McpSessionHandler;

// ═══════════════════════════════════════════════════════════════════════
// McpSession — abstract base for both client and server sessions
// ═══════════════════════════════════════════════════════════════════════
// Mirrors the C# McpSession abstract class pattern.
// Provides shared session lifecycle, request/response, and notification dispatching.
class McpSession : public std::enable_shared_from_this<McpSession> {
public:
    virtual ~McpSession() = default;

    // ── Lifecycle ──
    virtual void Run() = 0;
    virtual void Close() = 0;
    virtual bool IsRunning() const = 0;

    // ── Session identity ──
    virtual std::string_view SessionId() const = 0;
    virtual std::string_view NegotiatedProtocolVersion() const = 0;

    // ── Send ──
    // Send request and wait for response (future-based)
    virtual std::future<nlohmann::json> SendRequestAsync(
        std::string_view method,
        nlohmann::json params,
        const RequestMeta& meta = {},
        std::chrono::milliseconds timeout = std::chrono::seconds(30)) = 0;

    // Send notification (fire-and-forget)
    virtual void SendNotificationAsync(
        std::string_view method,
        nlohmann::json params = {}) = 0;

    // Send any JSON-RPC message
    virtual void SendMessageAsync(JsonRpcMessage message) = 0;

    // ── Notification handler registration ──
    virtual void RegisterNotificationHandler(
        std::string_view method,
        std::function<void(const JsonRpcNotification&)> handler) = 0;

    // ── Accessors ──
    virtual ITransport& GetTransport() = 0;
    virtual asio::io_context& IoContext() = 0;

    // ── Version helpers ──
    bool IsJuly2026OrLater() const {
        auto v = NegotiatedProtocolVersion();
        return !v.empty() && v >= "2026-07-28";
    }

protected:
    McpSession() = default;
};

} // namespace mcp
