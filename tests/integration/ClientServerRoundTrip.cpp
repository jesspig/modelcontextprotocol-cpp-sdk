// ClientServerRoundTrip — full-stack Client ↔ Server integration test
// Uses InMemoryTransport, covering all major APIs

#include <mcp/server/McpServer.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>

using namespace mcp;
using Ctx = RequestContext<CallToolRequestParams>;

namespace {

// Run the test body with a hard timeout guard: a hung call fails the test
// instead of blocking forever. Assertions inside the body keep their semantics.
template <typename F>
void RunWithTimeout(F&& body) {
    auto future = std::async(std::launch::async, std::forward<F>(body));
    if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        std::fprintf(stderr,
            "[  FAILED  ] test body hung: call did not complete within 10s\n");
        std::_Exit(1);
    }
    future.get();
}

} // namespace

struct ClientServerFixture : ::testing::Test {
    std::unique_ptr<McpServer> server;
    std::unique_ptr<McpClient> client;
    std::thread server_thread;

    void SetUp() override {
        auto pair = InMemoryTransport::CreatePair();

        ServerOptions sopts;
        sopts.server_info = Implementation{"TestServer", "1.0.0"};
        server = McpServer::Create(pair.server, sopts);

        // Register echo tool (text echo)
        server->RegisterTool("echo",
            ToolOptions{}.Description("Echo input"),
            std::function<CallToolResult(const Ctx&)>(
                [](const Ctx& ctx) -> CallToolResult {
                    auto text = ctx.Params().arguments
                        ? ((*ctx.Params().arguments)["text"].IsString()
                           ? (*ctx.Params().arguments)["text"].GetString()
                           : "")
                        : "";
                    CallToolResult r;
                    r.content.push_back(TextContent{"text", text});
                    return r;
                }));

        // Register add tool (numeric addition)
        server->RegisterTool("add",
            ToolOptions{}.Description("Add two numbers"),
            std::function<CallToolResult(const Ctx&)>(
                [](const Ctx& ctx) -> CallToolResult {
                    auto& args = ctx.Params().arguments;
                    int a = args && (*args)["a"].IsInt() ? static_cast<int>((*args)["a"].GetInt()) : 0;
                    int b = args && (*args)["b"].IsInt() ? static_cast<int>((*args)["b"].GetInt()) : 0;
                    CallToolResult r;
                    r.content.push_back(
                        TextContent{"text", std::to_string(a + b)});
                    return r;
                }));

        // Register static resource
        server->RegisterResource("hello", "hello://world",
            ResourceOptions{}.Description("Hello resource"),
            [](const std::string& uri) -> ReadResourceResult {
                TextResourceContents tc;
                tc.uri = uri;
                tc.text = "Hello, World!";
                ReadResourceResult rr;
                rr.contents = {ResourceContents{tc}};
                return rr;
            });

        // Start server in background thread
        server_thread = std::thread([this]() { server->Run(); });

        // Create client with auto mode to discover server info and capabilities
        ClientOptions cops;
        cops.client_info = Implementation{"TestClient", "1.0.0"};
        cops.connect_mode = ConnectMode::Auto;
        client = McpClient::Create(pair.client, cops);
    }

    void TearDown() override {
        if (client) client->Close();
        if (server) server->Close();
        if (server_thread.joinable()) server_thread.join();
    }
};

// ── List tools ──
TEST_F(ClientServerFixture, ListTools) {
    RunWithTimeout([this]() {
        auto result = client->ListTools();
        ASSERT_GE(result.tools.size(), 2);

        // Find echo and add tools in results
        bool found_echo = false, found_add = false;
        for (const auto& t : result.tools) {
            if (t.name == "echo") found_echo = true;
            if (t.name == "add") found_add = true;
        }
        EXPECT_TRUE(found_echo);
        EXPECT_TRUE(found_add);
    });
}

// ── Call echo tool ──
TEST_F(ClientServerFixture, CallToolEcho) {
    RunWithTimeout([this]() {
        auto result = client->CallTool("echo",
            JsonValue::Parse(R"({"text":"Hello MCP"})"));

        ASSERT_GE(result.content.size(), 1);
        auto* text = std::get_if<TextContent>(&result.content[0]);
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(text->text, "Hello MCP");
        EXPECT_FALSE(result.is_error);
    });
}

// ── Call add tool ──
TEST_F(ClientServerFixture, CallToolAdd) {
    RunWithTimeout([this]() {
        auto result = client->CallTool("add",
            JsonValue::Parse(R"({"a":40,"b":2})"));

        ASSERT_GE(result.content.size(), 1);
        auto* text = std::get_if<TextContent>(&result.content[0]);
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(text->text, "42");
    });
}

// ── Call nonexistent tool ──
TEST_F(ClientServerFixture, CallToolNotFound) {
    RunWithTimeout([this]() {
        EXPECT_THROW(
            client->CallTool("nonexistent"),
            McpError);
    });
}

// ── Read resource ──
TEST_F(ClientServerFixture, ReadResource) {
    RunWithTimeout([this]() {
        ReadResourceResult result;
        ASSERT_NO_THROW(result = client->ReadResource("hello://world"));
        ASSERT_GE(result.contents.size(), 1);

        auto* text = std::get_if<TextResourceContents>(&result.contents[0]);
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(text->text, "Hello, World!");
    });
}

// ── Server info ──
TEST_F(ClientServerFixture, ServerInfo) {
    RunWithTimeout([this]() {
        EXPECT_EQ(client->GetServerInfo().name, "TestServer");
        EXPECT_EQ(client->GetServerInfo().version, "1.0.0");
    });
}

// ── Server capabilities (tools + resources) ──
TEST_F(ClientServerFixture, ServerCapabilities) {
    RunWithTimeout([this]() {
        auto& caps = client->GetServerCapabilities();
        EXPECT_TRUE(caps.tools.has_value());
        EXPECT_TRUE(caps.resources.has_value());
        EXPECT_FALSE(caps.prompts.has_value());
    });
}

// ── Ping server ──
TEST_F(ClientServerFixture, Ping) {
    RunWithTimeout([this]() {
        // Ping is a 2025-only wire method; the fixture's Auto client
        // negotiates 2026, so build a dedicated legacy connection.
        auto pair = InMemoryTransport::CreatePair();

        ServerOptions sopts;
        sopts.server_info = Implementation{"TestServer", "1.0.0"};
        auto legacy_server = McpServer::Create(pair.server, sopts);
        std::thread server_thread([&legacy_server]() { legacy_server->Run(); });

        ClientOptions cops;
        cops.client_info = Implementation{"TestClient", "1.0.0"};
        cops.connect_mode = ConnectMode::Legacy;
        auto legacy_client = McpClient::Create(pair.client, cops);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        EXPECT_NO_THROW(legacy_client->Ping());
#pragma clang diagnostic pop

        legacy_client->Close();
        legacy_server->Close();
        server_thread.join();
    });
}
