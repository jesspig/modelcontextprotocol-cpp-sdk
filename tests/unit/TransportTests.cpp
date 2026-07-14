#include <mcp/transport/InMemoryTransport.hpp>
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

    // 发送消息不应崩溃
    JsonRpcRequest req;
    req.id = int64_t(1);
    req.method = "ping";
    pair.client->SendMessageAsync(JsonRpcMessage{req});
    pair.server->SendMessageAsync(JsonRpcMessage{req});

    pair.client->Close();
    pair.server->Close();

    // 关闭后发送应被忽略
    pair.client->SendMessageAsync(JsonRpcMessage{req});
}

// 验证 InMemoryTransport 的 Pair 可正常 move
TEST(TransportTest, InMemoryPairMove) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = std::move(pair.server);
    auto client = std::move(pair.client);
    EXPECT_NE(server, nullptr);
    EXPECT_NE(client, nullptr);
    server->Close();
    client->Close();
}
