#include <mcp/transport/WebSocketClientTransport.hpp>
#include <mcp/Transport.hpp>
#include <gtest/gtest.h>

using namespace mcp;

TEST(WebSocketTransportTest, ConstructionWithExplicitName) {
    WebSocketClientTransport transport("ws://localhost:8080/mcp", "test-ws");
    EXPECT_EQ(transport.Name(), "test-ws");
}

TEST(WebSocketTransportTest, ConstructionDefaultNameAndIndependence) {
    WebSocketClientTransport t1("wss://example.com/mcp");
    WebSocketClientTransport t2("ws://127.0.0.1:8080");
    EXPECT_EQ(t1.Name(), "websocket");
    EXPECT_EQ(t2.Name(), "websocket");

    // Construction must not throw or open a connection
    EXPECT_NO_THROW({
        WebSocketClientTransport t3("ws://localhost:8080");
        EXPECT_EQ(t3.Name(), "websocket");
    });
}
