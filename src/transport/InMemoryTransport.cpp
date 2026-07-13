#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/JsonRpc.hpp>

#include <asio/experimental/channel.hpp>
#include <asio/post.hpp>

#include <queue>

namespace mcp {

namespace {

// ── InMemory transport implementation: bidirectional pair ──
class InMemoryTransportImpl : public Transport {
public:
    InMemoryTransportImpl(
        asio::io_context& io_ctx,
        std::shared_ptr<asio::experimental::channel<void(asio::error_code, JsonRpcMessage)>> rx_channel,
        std::shared_ptr<std::queue<JsonRpcMessage>> tx_queue,
        std::shared_ptr<std::mutex> tx_mutex,
        std::shared_ptr<bool> closed)
        : io_ctx_(io_ctx)
        , rx_channel_(std::move(rx_channel))
        , tx_queue_(std::move(tx_queue))
        , tx_mutex_(std::move(tx_mutex))
        , closed_(std::move(closed))
    {}

    void Start() override {
        // No-op for in-memory
    }

    void Close() override {
        *closed_ = true;
        rx_channel_->close();
        NotifyClose();
    }

    void SendMessageAsync(JsonRpcMessage message) override {
        if (*closed_) return;
        std::lock_guard<std::mutex> lock(*tx_mutex_);
        tx_queue_->push(std::move(message));
    }

    MessageChannel& GetMessageChannel() override { return *rx_channel_; }

    asio::io_context& IoContext() override { return io_ctx_; }

    std::string_view SessionId() const override { return {}; }

private:
    asio::io_context& io_ctx_;
    std::shared_ptr<asio::experimental::channel<void(asio::error_code, JsonRpcMessage)>> rx_channel_;
    std::shared_ptr<std::queue<JsonRpcMessage>> tx_queue_;
    std::shared_ptr<std::mutex> tx_mutex_;
    std::shared_ptr<bool> closed_;
};

} // namespace

// ── CreatePair builds two linked transports ──
InMemoryTransport::Pair InMemoryTransport::CreatePair() {
    auto closed = std::make_shared<bool>(false);

    // Client → Server channel
    auto c2s = std::make_shared<
        asio::experimental::channel<void(asio::error_code, JsonRpcMessage)>>(
            *new asio::io_context, 64);

    // Server → Client channel
    auto s2c = std::make_shared<
        asio::experimental::channel<void(asio::error_code, JsonRpcMessage)>>(
            *new asio::io_context, 64);

    auto q_c2s = std::make_shared<std::queue<JsonRpcMessage>>();
    auto q_s2c = std::make_shared<std::queue<JsonRpcMessage>>();
    auto mtx = std::make_shared<std::mutex>();

    // We need shared io_contexts - use static ones for test simplicity
    static asio::io_context ctx;
    auto client = std::make_unique<InMemoryTransportImpl>(
        ctx, s2c, q_c2s, mtx, closed);
    auto server = std::make_unique<InMemoryTransportImpl>(
        ctx, c2s, q_s2c, mtx, closed);

    Pair pair;
    pair.client = std::move(client);
    pair.server = std::move(server);
    return pair;
}

} // namespace mcp
