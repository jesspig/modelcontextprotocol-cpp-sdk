#include <mcp/server/McpServer.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <gtest/gtest.h>

using namespace mcp;
using Ctx = RequestContext<CallToolRequestParams>;

// ── Server creation ──
TEST(McpServerTest, CreateAndDestroy) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));
    ASSERT_NE(server, nullptr);
    server->Close();
}

// ── Tool registration via shared_ptr ──
TEST(McpServerTest, RegisterTool) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    auto tool = McpServerTool::Create("echo",
        std::function<CallToolResult(const Ctx&)>(
            [](const Ctx&) { return CallToolResult{}; }),
        ToolOptions{}.Description("Echo input"));
    server->RegisterTool(tool);

    EXPECT_TRUE(server->GetCapabilities().tools.has_value());
    server->Close();
}

// ── Capability derivation ──
TEST(McpServerTest, CapabilitiesDeriveFromTools) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions opts;
    opts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), opts);

    auto tool = McpServerTool::Create("tool1",
        std::function<CallToolResult(const Ctx&)>(
            [](const Ctx&) { return CallToolResult{}; }),
        ToolOptions{});
    server->RegisterTool(tool);

    EXPECT_TRUE(server->GetCapabilities().tools.has_value());
    EXPECT_FALSE(server->GetCapabilities().resources.has_value());
    EXPECT_FALSE(server->GetCapabilities().prompts.has_value());
    server->Close();
}

// ── Register resource ──
TEST(McpServerTest, RegisterResource) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions opts;
    opts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), opts);

    server->RegisterResource("static-resource", "resource://static",
        ResourceOptions{},
        [](const std::string& uri) -> ReadResourceResult {
            TextResourceContents trc;
            trc.uri = uri;
            trc.text = "static content";
            ReadResourceResult rr;
            rr.contents = {mcp::ResourceContents{trc}};
            return rr;
        });

    EXPECT_TRUE(server->GetCapabilities().resources.has_value());
    server->Close();
}

// ── Register resource template ──
TEST(McpServerTest, RegisterResourceTemplate) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions opts;
    opts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), opts);

    server->RegisterResourceTemplate("template-resource",
        "resource://{param}",
        ResourceOptions{},
        [](const std::string& uri,
           const std::map<std::string, std::string>& vars) -> ReadResourceResult {
            TextResourceContents trc;
            trc.uri = uri;
            trc.text = "param=" + vars.at("param");
            ReadResourceResult rr;
            rr.contents = {mcp::ResourceContents{trc}};
            return rr;
        });

    EXPECT_TRUE(server->GetCapabilities().resources.has_value());
    server->Close();
}

// ── Register prompt ──
TEST(McpServerTest, RegisterPrompt) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions opts;
    opts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), opts);

    server->RegisterPrompt("greet",
        PromptOptions{}.Description("Greet someone"),
        [](const std::string& name,
           const std::optional<nlohmann::json>& args) -> GetPromptResult {
            GetPromptResult r;
            PromptMessage pm;
            pm.role = "user";
            pm.content = TextContent{"text", "Hello"};
            r.messages = {std::move(pm)};
            return r;
        });

    EXPECT_TRUE(server->GetCapabilities().prompts.has_value());
    server->Close();
}

// ── McpServerTool::Create ──
TEST(McpServerTest, McpServerToolFactory) {
    auto tool = McpServerTool::Create("calc",
        std::function<CallToolResult(const Ctx&)>(
            [](const Ctx&) { return CallToolResult{}; }),
        ToolOptions{}.Description("Calculator"));

    ASSERT_NE(tool, nullptr);
    EXPECT_EQ(tool->ProtocolTool().name, "calc");
    EXPECT_EQ(tool->ProtocolTool().description, "Calculator");
}

// ── SendToolListChanged ──
TEST(McpServerTest, SendToolListChanged) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    server->SendToolListChanged();
    server->SendResourceListChanged();
    server->SendPromptListChanged();
    server->SendLoggingMessage(LoggingLevel::Info, "test");
    server->Close();
}

// ── Null client capabilities initially ──
TEST(McpServerTest, ClientCapabilitiesInitialState) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    EXPECT_EQ(server->GetClientCapabilities(), nullptr);
    EXPECT_EQ(server->GetClientInfo(), nullptr);
    server->Close();
}
