#pragma once

#include <mcp/JsonRpc.hpp>

#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <asio/experimental/channel.hpp>

#include <functional>
#include <memory>
#include <string_view>

namespace mcp {

// ── Transport — abstract base for all transports ──
// Provides send/receive over any underlying channel (stdio, HTTP, SSE, in-memory).
class Transport : public std::enable_shared_from_this<Transport> {
public:
    virtual ~Transport() = default;

    // ── Lifecycle ──
    virtual void Start() = 0;
    virtual void Close() = 0;

    // ── Send ──
    // Async send of a JSON-RPC message. Thread-safe.
    virtual void SendMessageAsync(JsonRpcMessage message) = 0;

    // ── Receive ──
    // asio channel: async_receive() returns JsonRpcMessage.
    // The channel is backed by the transport's receive loop.
    using MessageChannel = asio::experimental::channel<void(asio::error_code, JsonRpcMessage)>;
    virtual MessageChannel& GetMessageChannel() = 0;
    virtual asio::io_context& IoContext() = 0;

    // ── Properties ──
    virtual std::string_view SessionId() const { return {}; }
    virtual bool IsStateless() const { return false; }

    // ── Callbacks ──
    void SetOnClose(std::function<void()> cb) { on_close_ = std::move(cb); }
    void SetOnError(std::function<void(std::string_view)> cb) { on_error_ = std::move(cb); }

protected:
    void NotifyClose() { if (on_close_) on_close_(); }
    void NotifyError(std::string_view msg) { if (on_error_) on_error_(msg); }

    std::function<void()> on_close_;
    std::function<void(std::string_view)> on_error_;
};

// ── ClientTransport — factory for client-side Transport ──
class ClientTransport {
public:
    virtual ~ClientTransport() = default;
    virtual std::string_view Name() const = 0;
    virtual std::unique_ptr<Transport> Connect() = 0;
};

} // namespace mcp
