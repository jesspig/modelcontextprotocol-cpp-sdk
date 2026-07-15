#pragma once
#include <mcp/Export.hpp>
#include <mcp/JsonRpc.hpp>
#include <asio/io_context.hpp>
#include <asio/experimental/channel.hpp>
#include <system_error>
#include <memory>
#include <functional>
#include <future>

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
            [](asio::error_code) {});
    }

    // Try send without allocation
    bool TrySend(JsonRpcMessage message) {
        return channel_.try_send(asio::error_code{}, std::move(message));
    }

    // Close the channel
    void Close() { channel_.close(); }

    // Check if open
    bool IsOpen() const { return channel_.is_open(); }

    AsioChannel& GetAsioChannel() { return channel_; }

private:
    asio::io_context& io_ctx_;
    AsioChannel channel_;
};

// ═══════════════════════════════════════════════════════════════════════
// ChannelReader — async iterator helper (similar to C# ChannelReader)
// ═══════════════════════════════════════════════════════════════════════
class MCP_API ChannelReader {
public:
    explicit ChannelReader(MessageChannel& channel) : channel_(channel) {}

    // Read next message asynchronously (returns future)
    std::future<JsonRpcMessage> ReadAsync() {
        auto promise = std::make_shared<std::promise<JsonRpcMessage>>();
        auto future = promise->get_future();

        channel_.AsyncReceive(
            [promise](asio::error_code ec, JsonRpcMessage msg) {
                if (ec) {
                    promise->set_exception(
                        std::make_exception_ptr(std::system_error(ec)));
                    return;
                }
                promise->set_value(std::move(msg));
            });

        return future;
    }

private:
    MessageChannel& channel_;
};

} // namespace mcp
