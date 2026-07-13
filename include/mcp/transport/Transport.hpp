#pragma once

#include <mcp/JsonRpc.hpp>
#include <memory>
#include <functional>
#include <system_error>
#include <asio.hpp>

namespace mcp {

/// Transport interface for MCP communication.
/// Matches C# ITransport + IClientTransport combined model.
class Transport {
public:
    virtual ~Transport() = default;

    /// Session ID assigned by the transport.
    virtual std::string_view SessionId() const = 0;

    /// Channel that receives incoming messages.
    virtual asio::channel<void(asio::error_code, SessionMessage)>&
        MessageChannel() = 0;

    /// Start the transport (open connection, begin reading).
    virtual asio::awaitable<void> Start() = 0;

    /// Send a JSON-RPC message through the transport.
    virtual asio::awaitable<void> SendMessageAsync(
        const JsonRpcMessage& message,
        const std::optional<MessageMetadata>& metadata = std::nullopt) = 0;

    /// Close the transport gracefully.
    virtual asio::awaitable<void> Close() = 0;

    /// Returns true if this transport supports per-request streams (Streamable HTTP).
    virtual bool HasPerRequestStream() const { return false; }

    /// Returns true if this transport supports server-to-client requests.
    virtual bool SupportsBackChannel() const { return true; }

    // ── Callbacks ──
    void SetOnClose(std::function<void()> cb) { on_close_ = std::move(cb); }
    void SetOnError(std::function<void(std::error_code)> cb) { on_error_ = std::move(cb); }

protected:
    std::function<void()> on_close_;
    std::function<void(std::error_code)> on_error_;
};

/// Client-side transport factory.
/// Matches C# IClientTransport.
class ClientTransport {
public:
    virtual ~ClientTransport() = default;

    /// Transport name (for debugging/logging).
    virtual std::string_view Name() const = 0;

    /// Connect to the server and return a Transport instance.
    virtual asio::awaitable<std::unique_ptr<Transport>> ConnectAsync() = 0;
};

} // namespace mcp
