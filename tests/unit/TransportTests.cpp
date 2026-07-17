#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/Transport.hpp>
#include <mcp/JsonRpc.hpp>
#include <gtest/gtest.h>

using namespace mcp;

// InMemoryTransport 创建和基本功能验证
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

    pair.client->SendMessageAsync(JsonRpcMessage{req});
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

// 验证消息收发不掉崩溃（异步交付由 io_context 驱动，此处仅测无崩溃）
TEST(TransportTest, InMemoryMessageSendNoCrash) {
    auto pair = InMemoryTransport::CreatePair();

    JsonRpcRequest req;
    req.id = int64_t(42);
    req.method = "tools/list";

    pair.client->SendMessageAsync(JsonRpcMessage{req});
    pair.client->Close();
    pair.server->Close();
}