#pragma once
#include <mcp/Export.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/Log.hpp>
#include <asio/io_context.hpp>
#include <asio/experimental/channel.hpp>
#include <system_error>
#include <memory>
#include <functional>

namespace mcp {

// ═══════════════════════════════════════════════════════════════════════
// MessageChannel — wraps asio::experimental::channel for JsonRpcMessage
// ═══════════════════════════════════════════════════════════════════════
class MCP_API MessageChannel {
public:
    using AsioChannel = asio::experimental::channel<void(asio::error_code, JsonRpcMessage)>;

    explicit MessageChannel(asio::io_context& io_ctx, size_t max_buffer = 64)
        : io_ctx_(io_ctx)
        , channel_(io_ctx, max_buffer) {}

    // Get the associated io_context
    asio::io_context& IoContext() const { return io_ctx_; }

    // Async receive — calls callback when a message is available
    template <typename Callback>
    void AsyncReceive(Callback&& cb) {
        channel_.async_receive(std::forward<Callback>(cb));
    }

    // Send a message into the channel
    void Send(JsonRpcMessage message) {
        channel_.async_send(asio::error_code{}, std::move(message),
            [](asio::error_code ec) {
                if (ec && ec != asio::error::operation_aborted) {
                    MCP_BUG("MessageChannel::Send failed");
                }
            });
    }

    // Close the channel
    void Close() { channel_.close(); }

    // Check if open
    bool IsOpen() const { return channel_.is_open(); }

private:
    asio::io_context& io_ctx_;
    AsioChannel channel_;
};

} // namespace mcp
