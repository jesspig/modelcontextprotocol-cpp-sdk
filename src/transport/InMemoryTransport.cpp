#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/protocol/MessageChannel.hpp>

#include <queue>

namespace mcp {

namespace {

class InMemoryTransportImpl : public TransportBase {
public:
    InMemoryTransportImpl(
        std::shared_ptr<MessageChannel> recv_channel,
        std::shared_ptr<MessageChannel> send_channel)
        : TransportBase()
        , recv_channel_(std::move(recv_channel))
        , send_channel_(std::move(send_channel))
    {}

    void Close() override {
        if (GetState() == TransportState::Disconnected) return;
        recv_channel_->Close();
        SetDisconnected();
    }

    void SendMessageAsync(JsonRpcMessage message) override {
        send_channel_->Send(std::move(message));
    }

    MessageChannel& GetMessageChannel() override { return *recv_channel_; }

private:
    std::shared_ptr<MessageChannel> recv_channel_;
    std::shared_ptr<MessageChannel> send_channel_;
};

} // namespace

InMemoryTransport::Pair InMemoryTransport::CreatePair() {
    // Client receives from s2c, writes to c2s; server receives from c2s, writes to s2c
    auto c2s = std::make_shared<MessageChannel>(64);
    auto s2c = std::make_shared<MessageChannel>(64);

    auto client = std::make_shared<InMemoryTransportImpl>(s2c, c2s);
    auto server = std::make_shared<InMemoryTransportImpl>(c2s, s2c);

    Pair pair;
    pair.client = std::move(client);
    pair.server = std::move(server);
    return pair;
}

} // namespace mcp
