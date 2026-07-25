#include <mcp/transport/WebSocketClientTransport.hpp>
#include <mcp/Transport.hpp>
#include <mcp/JsonRpc.hpp>
#include <gtest/gtest.h>

using namespace mcp;

TEST(WebSocketTransportTest, ConstructionWithUrl) {
    WebSocketClientTransport transport("ws://localhost:8080/mcp", "test-ws");
    EXPECT_EQ(transport.Name(), "test-ws");
}

TEST(WebSocketTransportTest, ConstructionDefaultName) {
    WebSocketClientTransport transport("wss://example.com/mcp");
    EXPECT_EQ(transport.Name(), "websocket");
}

TEST(WebSocketTransportTest, ConstructionWithHttpUrl) {
    WebSocketClientTransport transport("ws://127.0.0.1:8080");
    EXPECT_EQ(transport.Name(), "websocket");
}

TEST(WebSocketTransportTest, ConstructionDoesNotThrow) {
    EXPECT_NO_THROW({
        WebSocketClientTransport transport("ws://localhost:8080");
    });
}

TEST(WebSocketTransportTest, MultipleTransports) {
    WebSocketClientTransport t1("ws://host1:8080", "ws1");
    WebSocketClientTransport t2("ws://host2:8080", "ws2");
    EXPECT_EQ(t1.Name(), "ws1");
    EXPECT_EQ(t2.Name(), "ws2");
}
