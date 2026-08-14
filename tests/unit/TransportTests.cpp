// TransportTests — unit tests for InMemoryTransport creation, move, and state machine

#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/Transport.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/test/McpTest.hpp>

using namespace mcp;

// InMemoryTransport creation and basic functionality
TEST(TransportTest, InMemoryCreate) {
    auto pair = InMemoryTransport::CreatePair();

    EXPECT_TRUE(pair.client->SessionId().empty());
    EXPECT_TRUE(pair.server->SessionId().empty());
    EXPECT_FALSE(pair.client->IsStateless());
    EXPECT_FALSE(pair.server->IsStateless());

    JsonRpcRequest req;
    req.id = int64_t(1);
    req.method = "ping";
    pair.client->SendMessageAsync(JsonRpcMessage{req});
    pair.server->SendMessageAsync(JsonRpcMessage{req});

    pair.client->Close();
    pair.server->Close();
}

TEST(TransportTest, InMemoryPairMove) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = std::move(pair.server);
    auto client = std::move(pair.client);
    EXPECT_NE(server, nullptr);
    EXPECT_NE(client, nullptr);
    server->Close();
    client->Close();
}

TEST(TransportTest, TransportBaseStateMachine) {
    auto pair = InMemoryTransport::CreatePair();
    auto* tb = dynamic_cast<TransportBase*>(pair.client.get());
    ASSERT_NE(tb, nullptr);
    EXPECT_EQ(tb->GetState(), TransportState::Initial);

    tb->SetConnected();
    EXPECT_EQ(tb->GetState(), TransportState::Connected);

    tb->SetDisconnected();
    EXPECT_EQ(tb->GetState(), TransportState::Disconnected);
}

TEST(TransportTest, TransportBaseErrorPropagation) {
    auto pair = InMemoryTransport::CreatePair();
    auto* tb = dynamic_cast<TransportBase*>(pair.client.get());
    ASSERT_NE(tb, nullptr);

    bool close_called = false;
    tb->SetOnClose([&close_called]() { close_called = true; });

    tb->SetConnected();
    tb->SetDisconnected();

    EXPECT_TRUE(close_called);
    EXPECT_EQ(tb->GetState(), TransportState::Disconnected);
}

// InMemoryTransport delivers synchronously on SendMessageAsync: the peer
// receives the message from its MessageChannel without an external event loop.
TEST(TransportTest, InMemoryMessageSendNoCrash) {
    auto pair = InMemoryTransport::CreatePair();

    JsonRpcRequest req;
    req.id = int64_t(42);
    req.method = "tools/list";

    pair.client->SendMessageAsync(JsonRpcMessage{req});

    std::error_code ec;
    JsonRpcMessage received;
    pair.server->GetMessageChannel().AsyncReceive(
        [&](std::error_code recv_ec, JsonRpcMessage msg) {
            ec = recv_ec;
            received = std::move(msg);
        });

    ASSERT_FALSE(ec);
    const auto& req2 = std::get<JsonRpcRequest>(received);
    EXPECT_EQ(req2.method, "tools/list");
    EXPECT_EQ(req2.id, RequestId{int64_t(42)});

    pair.client->Close();
    pair.server->Close();
}

// After Close the channels are shut down: sending must not throw or crash,
// and the message is dropped instead of delivered.
TEST(TransportTest, CloseThenSendDoesNotThrow) {
    auto pair = InMemoryTransport::CreatePair();
    pair.client->Close();
    pair.server->Close();

    JsonRpcRequest req;
    req.id = int64_t(1);
    req.method = "ping";
    EXPECT_NO_THROW(pair.client->SendMessageAsync(JsonRpcMessage{req}));
    EXPECT_NO_THROW(pair.server->SendMessageAsync(JsonRpcMessage{req}));
}