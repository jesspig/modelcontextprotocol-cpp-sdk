#include <mcp/server/McpServer.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <mcp/test/McpTest.hpp>
#include <mcp/test/McpTimeout.hpp>

#include <thread>

using namespace mcp;
using Ctx = RequestContext<CallToolRequestParams>;

struct ClientServerFixture : mcp::test::TestCase {
    std::unique_ptr<McpServer> server;
    std::unique_ptr<McpClient> client;
    std::thread server_thread;

    void SetUp() override {
        auto pair = InMemoryTransport::CreatePair();

        ServerOptions sopts;
        sopts.server_info = Implementation{"TestServer", "1.0.0"};
        server = McpServer::Create(pair.server, sopts);

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

        server_thread = std::thread([this]() { server->Run(); });

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

TEST_F(ClientServerFixture, ListTools) {
    MCP_RUN_WITH_TIMEOUT([this]() {
        auto result = client->ListTools();
        ASSERT_GE(result.tools.size(), 2);

        bool found_echo = false, found_add = false;
        for (const auto& t : result.tools) {
            if (t.name == "echo") found_echo = true;
            if (t.name == "add") found_add = true;
        }
        EXPECT_TRUE(found_echo);
        EXPECT_TRUE(found_add);
    });
}

TEST_F(ClientServerFixture, CallToolEcho) {
    MCP_RUN_WITH_TIMEOUT([this]() {
        auto result = client->CallTool("echo",
            JsonValue::Parse(R"({"text":"Hello MCP"})"));

        ASSERT_GE(result.content.size(), 1);
        auto* text = std::get_if<TextContent>(&result.content[0]);
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(text->text, "Hello MCP");
        EXPECT_FALSE(result.is_error);
    });
}

TEST_F(ClientServerFixture, CallToolAdd) {
    MCP_RUN_WITH_TIMEOUT([this]() {
        auto result = client->CallTool("add",
            JsonValue::Parse(R"({"a":40,"b":2})"));

        ASSERT_GE(result.content.size(), 1);
        auto* text = std::get_if<TextContent>(&result.content[0]);
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(text->text, "42");
    });
}

TEST_F(ClientServerFixture, CallToolNotFound) {
    MCP_RUN_WITH_TIMEOUT([this]() {
        EXPECT_THROW(
            client->CallTool("nonexistent"),
            McpError);
    });
}

TEST_F(ClientServerFixture, ReadResource) {
    MCP_RUN_WITH_TIMEOUT([this]() {
        ReadResourceResult result;
        ASSERT_NO_THROW(result = client->ReadResource("hello://world"));
        ASSERT_GE(result.contents.size(), 1);

        auto* text = std::get_if<TextResourceContents>(&result.contents[0]);
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(text->text, "Hello, World!");
    });
}

TEST_F(ClientServerFixture, ServerInfo) {
    MCP_RUN_WITH_TIMEOUT([this]() {
        EXPECT_EQ(client->GetServerInfo().name, "TestServer");
        EXPECT_EQ(client->GetServerInfo().version, "1.0.0");
    });
}

TEST_F(ClientServerFixture, ServerCapabilities) {
    MCP_RUN_WITH_TIMEOUT([this]() {
        auto& caps = client->GetServerCapabilities();
        EXPECT_TRUE(caps.tools.has_value());
        EXPECT_TRUE(caps.resources.has_value());
        EXPECT_FALSE(caps.prompts.has_value());
    });
}

TEST_F(ClientServerFixture, Ping) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
__pragma(warning(push))
__pragma(warning(disable : 4996))
#endif
    MCP_RUN_WITH_TIMEOUT([]() {
        auto pair = InMemoryTransport::CreatePair();

        ServerOptions sopts;
        sopts.server_info = Implementation{"TestServer", "1.0.0"};
        auto legacy_server = McpServer::Create(pair.server, sopts);
        std::thread server_thread([&legacy_server]() { legacy_server->Run(); });

        ClientOptions cops;
        cops.client_info = Implementation{"TestClient", "1.0.0"};
        cops.connect_mode = ConnectMode::Legacy;
        auto legacy_client = McpClient::Create(pair.client, cops);

        EXPECT_NO_THROW(legacy_client->Ping());

        legacy_client->Close();
        legacy_server->Close();
        server_thread.join();
    });
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
__pragma(warning(pop))
#endif
}
