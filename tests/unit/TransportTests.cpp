#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/Transport.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/test/McpTest.hpp>

#include "TestFakes.hpp"

using namespace mcp;

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

TEST(TransportTest, FakeTransportRecordsSends) {
    FakeTransport fake;
    fake.SetConnectResult(true);
    EXPECT_TRUE(fake.ConnectResult());
    EXPECT_FALSE(fake.Closed());
    EXPECT_TRUE(fake.Sent().empty());

    JsonRpcRequest initialize;
    initialize.id = int64_t(1);
    initialize.method = "initialize";
    fake.SendMessageAsync(JsonRpcMessage{initialize});

    JsonRpcRequest list_tools;
    list_tools.id = int64_t(2);
    list_tools.method = "tools/list";
    JsonRpcMessage last_sent{list_tools};
    fake.SendMessageAsync(last_sent);

    EXPECT_CALL_COUNT(fake, 2);
    EXPECT_EQ(fake.LastSent(), SerializeMessage(last_sent));

    auto first = DeserializeMessage(fake.Sent()[0].payload);
    ASSERT_TRUE(IsRequest(first));
    EXPECT_EQ(AsRequest(first)->id, RequestId{int64_t(1)});
    EXPECT_EQ(AsRequest(first)->method, "initialize");

    JsonRpcNotification initialized;
    initialized.method = "notifications/initialized";
    ASSERT_TRUE(fake.PushIncoming(JsonRpcMessage{initialized}));

    JsonRpcMessage received;
    fake.GetMessageChannel().AsyncReceive(
        [&received](std::error_code, JsonRpcMessage message) {
            received = std::move(message);
        });
    ASSERT_TRUE(IsNotification(received));
    EXPECT_EQ(AsNotification(received)->method, "notifications/initialized");

    fake.Close();
    EXPECT_TRUE(fake.Closed());
    EXPECT_FALSE(fake.GetMessageChannel().IsOpen());
}