// McpClientTests — unit tests for McpClient creation, options, tool conversion,
// and real protocol negotiation against an in-memory server

#include <mcp/client/McpClient.hpp>
#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/McpTypes.hpp>

#include <gtest/gtest.h>

#include <future>
#include <thread>

using namespace mcp;

// ── Create client directly (no server, for unit testing) ──
TEST(McpClientTest, CreateAndDestroy) {
    auto pair = InMemoryTransport::CreatePair();
    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    auto client = McpClient::Create(std::move(pair.client), opts);
    ASSERT_NE(client, nullptr);
    EXPECT_TRUE(client->IsModernProtocol());
    client->Close();
}

// ── Initial state ──
TEST(McpClientTest, InitialState) {
    auto pair = InMemoryTransport::CreatePair();
    ClientOptions opts;
    opts.client_info = Implementation{"my-client", "2.0"};
    opts.connect_mode = ConnectMode::Pin;

    auto client = McpClient::Create(std::move(pair.client), opts);
    EXPECT_TRUE(client->GetServerInfo().name.empty());
    client->Close();
}

// ── ClientOptions ──
TEST(McpClientTest, ClientOptionsDefaults) {
    ClientOptions opts;
    EXPECT_EQ(opts.connect_mode, ConnectMode::Auto);
    EXPECT_EQ(opts.client_info.name, "mcp-cpp-client");
    EXPECT_EQ(opts.initialization_timeout.count(), 60);
    EXPECT_EQ(opts.discover_probe_timeout.count(), 5);
}

// ── Version negotiation against a real in-memory server ──

// Legacy mode: must go through the initialize handshake and stay legacy.
TEST(McpClientTest, LegacyNegotiationUsesInitialize) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), sopts);

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Legacy;
    opts.initialization_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), opts);

    EXPECT_FALSE(client->IsModernProtocol());
    EXPECT_EQ(client->GetNegotiatedProtocolVersion(), std::string(kLegacyProtocolVersion));
    EXPECT_EQ(client->GetServerInfo().name, "test-server");
    client->Close();
    server->Close();
}

// Auto mode with a modern server: server/discover probe succeeds.
TEST(McpClientTest, AutoNegotiationDiscoversModern) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), sopts);

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Auto;
    opts.discover_probe_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), opts);

    EXPECT_TRUE(client->IsModernProtocol());
    EXPECT_EQ(client->GetNegotiatedProtocolVersion(), std::string(kLatestProtocolVersion));
    EXPECT_EQ(client->GetServerInfo().name, "test-server");
    client->Close();
    server->Close();
}

// Auto mode against a peer that answers MethodNotFound for server/discover:
// must fall back to the initialize handshake.
TEST(McpClientTest, AutoNegotiationFallsBackToInitialize) {
    auto pair = InMemoryTransport::CreatePair();

    // Bare peer handler with no server/discover handler registered: the probe
    // fails with MethodNotFound, so negotiation must fall back to initialize.
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    server_handler->SetRequestHandler(methods::kInitialize,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue result(JsonValue::object_tag);
            result["protocolVersion"] = JsonValue(std::string(kLegacyProtocolVersion));
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            result["serverInfo"] = SerializeImplementation(Implementation{"raw-server", "1.0"});
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Auto;
    opts.discover_probe_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), opts);

    EXPECT_FALSE(client->IsModernProtocol());
    EXPECT_EQ(client->GetNegotiatedProtocolVersion(), std::string(kLegacyProtocolVersion));
    EXPECT_EQ(client->GetServerInfo().name, "raw-server");
    client->Close();
    server_handler->Close();
}

// Pin mode: no handshake is sent; the pinned version is negotiated directly.
// Note: Pin mode does not populate server_info (known limitation).
TEST(McpClientTest, PinNegotiationUsesPinnedVersion) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    EXPECT_TRUE(client->IsModernProtocol());
    EXPECT_EQ(client->GetNegotiatedProtocolVersion(), std::string(kLatestProtocolVersion));
    client->Close();
    server_handler->Close();
}

// ── SetNotificationHandler must take effect immediately after Create ──
TEST(McpClientTest, SetNotificationHandlerFiresAfterCreate) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    auto client = McpClient::Create(std::move(pair.client), opts);

    std::promise<void> received;
    auto received_future = received.get_future();
    bool called = false;
    client->SetNotificationHandler(notifications::kToolListChanged,
        [&called, &received](const JsonRpcNotification&) {
            called = true;
            received.set_value();
        });

    server->SendToolListChanged();

    EXPECT_EQ(received_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    EXPECT_TRUE(called);
    client->Close();
    server->Close();
}

// ── Client lifecycle ──
TEST(McpClientTest, CreateAndClose) {
    auto pair = InMemoryTransport::CreatePair();
    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    auto client = McpClient::Create(std::move(pair.client), opts);
    ASSERT_NE(client, nullptr);
    client->Close();
}
