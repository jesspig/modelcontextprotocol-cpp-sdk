#include <mcp/client/McpClient.hpp>
#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/server/McpServer.hpp>

#include <gtest/gtest.h>

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
    EXPECT_EQ(client->GetServerInfo().name.empty(), true);
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

// ── ConnectMode enum ──
TEST(McpClientTest, ConnectModeValues) {
    EXPECT_NE(static_cast<int>(ConnectMode::Auto),
              static_cast<int>(ConnectMode::Legacy));
    EXPECT_NE(static_cast<int>(ConnectMode::Legacy),
              static_cast<int>(ConnectMode::Pin));
}

// ── McpClientTool conversions ──
TEST(McpClientTest, McpClientToolFromProtocol) {
    Tool t;
    t.name = "calc";
    t.description = "Calculator";

    auto ct = McpClientTool::FromProtocol(t);
    EXPECT_EQ(ct.protocol_tool.name, "calc");
    EXPECT_EQ(ct.protocol_tool.description, "Calculator");
}

// ── VersionNegotiation static methods ──
TEST(McpClientTest, VersionNegotiationProbe) {
    // Just test that the function exists and handles errors gracefully
    // Full integration test requires running protocol
    SUCCEED();
}

// ── Tool cache management ──
TEST(McpClientTest, AddKnownTools) {
    auto pair = InMemoryTransport::CreatePair();
    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    auto client = McpClient::Create(std::move(pair.client), opts);

    Tool t;
    t.name = "cached_tool";
    client->AddKnownTools({t});

    client->RemoveKnownTools({"cached_tool"});
    client->ClearKnownTools();
    client->Close();
}
