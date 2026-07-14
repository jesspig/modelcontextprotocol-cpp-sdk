#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/JsonRpc.hpp>

#include <asio/experimental/channel.hpp>
#include <asio/post.hpp>

#include <queue>

namespace mcp {

namespace {

class InMemoryTransportImpl : public Transport {
public:
    InMemoryTransportImpl(
        std::shared_ptr<asio::io_context> io_ctx,
        std::shared_ptr<asio::experimental::channel<void(asio::error_code, JsonRpcMessage)>> tx_channel)
        : io_ctx_(std::move(io_ctx))
        , tx_channel_(std::move(tx_channel))
    {}

    void Start() override {}

    void Close() override {
        closed_ = true;
        tx_channel_->close();
        NotifyClose();
    }

    void SendMessageAsync(JsonRpcMessage message) override {
        if (closed_) return;
        tx_channel_->async_send(asio::error_code{}, std::move(message),
            [](asio::error_code) {});
    }

    MessageChannel& GetMessageChannel() override { return *tx_channel_; }
    asio::io_context& IoContext() override { return *io_ctx_; }
    std::string_view SessionId() const override { return {}; }

private:
    std::shared_ptr<asio::io_context> io_ctx_;
    std::shared_ptr<asio::experimental::channel<void(asio::error_code, JsonRpcMessage)>> tx_channel_;
    bool closed_ = false;
};

} // namespace

InMemoryTransport::Pair InMemoryTransport::CreatePair() {
    auto io_ctx = std::make_shared<asio::io_context>();

    auto c2s = std::make_shared<
        asio::experimental::channel<void(asio::error_code, JsonRpcMessage)>>(
            *io_ctx, 64);

    auto s2c = std::make_shared<
        asio::experimental::channel<void(asio::error_code, JsonRpcMessage)>>(
            *io_ctx, 64);

    auto client = std::make_unique<InMemoryTransportImpl>(io_ctx, c2s);
    auto server = std::make_unique<InMemoryTransportImpl>(io_ctx, s2c);

    Pair pair;
    pair.client = std::move(client);
    pair.server = std::move(server);
    return pair;
}

} // namespace mcp
