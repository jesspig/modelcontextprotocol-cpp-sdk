// 全链路 Client ↔ Server 集成测试
// 使用 InMemoryTransport，覆盖所有主要 API

#include <mcp/server/McpServer.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <gtest/gtest.h>

#include <thread>

using namespace mcp;
using Ctx = RequestContext<CallToolRequestParams>;

struct ClientServerFixture : ::testing::Test {
    std::unique_ptr<McpServer> server;
    std::unique_ptr<McpClient> client;
    std::thread server_thread;

    void SetUp() override {
        auto pair = InMemoryTransport::CreatePair();

        // Use the transport's shared io_context so server->Run() processes
        // async work for both client and server transports.
        auto& io_ctx = pair.server->GetMessageChannel().IoContext();

        ServerOptions sopts;
        sopts.server_info = Implementation{"TestServer", "1.0.0"};
        server = McpServer::Create(pair.server, sopts, &io_ctx);

        // Register echo tool
        server->RegisterTool("echo",
            ToolOptions{}.Description("Echo input"),
            std::function<CallToolResult(const Ctx&)>(
                [](const Ctx& ctx) -> CallToolResult {
                    auto text = ctx.Params().arguments
                        ? ctx.Params().arguments->value("text", "")
                        : "";
                    CallToolResult r;
                    r.content.push_back(TextContent{"text", text});
                    return r;
                }));

        // Register add tool (numeric)
        server->RegisterTool("add",
            ToolOptions{}.Description("Add two numbers"),
            std::function<CallToolResult(const Ctx&)>(
                [](const Ctx& ctx) -> CallToolResult {
                    auto& args = ctx.Params().arguments;
                    int a = args ? args->value("a", 0) : 0;
                    int b = args ? args->value("b", 0) : 0;
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

        // Start server (runs the shared io_context, processing both sides)
        server_thread = std::thread([this]() { server->Run(); });

        // Create client with auto mode — discovers server info/capabilities
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

// ── 列出工具 ──
TEST_F(ClientServerFixture, ListTools) {
    auto result = client->ListTools();
    ASSERT_GE(result.tools.size(), 2);

    // Find echo tool
    bool found_echo = false, found_add = false;
    for (const auto& t : result.tools) {
        if (t.name == "echo") found_echo = true;
        if (t.name == "add") found_add = true;
    }
    EXPECT_TRUE(found_echo);
    EXPECT_TRUE(found_add);
}

// ── 调用 echo ──
TEST_F(ClientServerFixture, CallToolEcho) {
    auto result = client->CallTool("echo",
        nlohmann::json{{"text", "Hello MCP"}});

    ASSERT_GE(result.content.size(), 1);
    auto* text = std::get_if<TextContent>(&result.content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "Hello MCP");
    EXPECT_FALSE(result.is_error);
}

// ── 调用 add ──
TEST_F(ClientServerFixture, CallToolAdd) {
    auto result = client->CallTool("add",
        nlohmann::json{{"a", 40}, {"b", 2}});

    ASSERT_GE(result.content.size(), 1);
    auto* text = std::get_if<TextContent>(&result.content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "42");
}

// ── 调用不存在的工具 ──
TEST_F(ClientServerFixture, CallToolNotFound) {
    EXPECT_THROW(
        client->CallTool("nonexistent"),
        McpError);
}

// ── 读取资源 ──
TEST_F(ClientServerFixture, ReadResource) {
    ReadResourceResult result;
    ASSERT_NO_THROW(result = client->ReadResource("hello://world"));
    ASSERT_GE(result.contents.size(), 1);

    auto* text = std::get_if<TextResourceContents>(&result.contents[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "Hello, World!");
}

// ── Server 信息 ──
TEST_F(ClientServerFixture, ServerInfo) {
    EXPECT_EQ(client->GetServerInfo().name, "TestServer");
    EXPECT_EQ(client->GetServerInfo().version, "1.0.0");
}

// ── Server 能力 (有 tools + resources) ──
TEST_F(ClientServerFixture, ServerCapabilities) {
    auto& caps = client->GetServerCapabilities();
    EXPECT_TRUE(caps.tools.has_value());
    EXPECT_TRUE(caps.resources.has_value());
    EXPECT_FALSE(caps.prompts.has_value());
}

// ── Ping ──
TEST_F(ClientServerFixture, Ping) {
    EXPECT_NO_THROW(client->Ping());
}
