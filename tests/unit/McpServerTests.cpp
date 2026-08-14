// McpServerTests — unit tests for McpServer creation, tool/resource/prompt registration

#include <mcp/server/McpServer.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <mcp/test/McpTest.hpp>

#include <chrono>
#include <functional>
#include <future>

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
           const std::optional<JsonValue>& args) -> GetPromptResult {
            (void)name; (void)args;
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

// ── SendToolListChanged & friends deliver notifications to the peer ──
namespace {

void ExpectNotificationArrives(
    const std::shared_ptr<McpSessionHandler>& client,
    std::string_view method,
    const std::function<void()>& send)
{
    std::promise<void> received;
    auto received_future = received.get_future();
    client->SetNotificationHandler(method,
        [&received](const JsonRpcNotification&) { received.set_value(); });
    send();
    EXPECT_EQ(received_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
}

} // namespace

TEST(McpServerTest, SendToolListChanged) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    ExpectNotificationArrives(client, notifications::kToolListChanged,
        [&] { server->SendToolListChanged(); });
    ExpectNotificationArrives(client, notifications::kResourceListChanged,
        [&] { server->SendResourceListChanged(); });
    ExpectNotificationArrives(client, notifications::kPromptListChanged,
        [&] { server->SendPromptListChanged(); });
    ExpectNotificationArrives(client, notifications::kMessage,
        [&] { server->SendLoggingMessage(LoggingLevel::Info, "test"); });

    server->Close();
    client->Close();
}

// ── Null client capabilities initially ──
TEST(McpServerTest, ClientCapabilitiesInitialState) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    EXPECT_EQ(server->GetClientCapabilities(), nullptr);
    EXPECT_EQ(server->GetClientInfo(), nullptr);
    server->Close();
}

// ── RequireInitialized guard: method handlers reject requests before
// notifications/initialized arrives ──
TEST(McpServerTest, RejectsRequestsBeforeInitialized) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLegacyProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), sopts);

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    auto future = client->SendRequest(methods::kPing,
        JsonValue(JsonValue::object_tag), {},
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.Contains("code"));
    EXPECT_EQ(result["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::InvalidRequest));
    EXPECT_EQ(result["message"].GetString(), std::string("Server not initialized"));

    server->Close();
    client->Close();
}

// ── HandleInitialize echoes the client's legacy protocol version back ──
TEST(McpServerTest, InitializeEchoesClientVersion) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), sopts);

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    InitializeRequestParams params;
    params.protocol_version = std::string(kLegacyProtocolVersion);
    params.client_info = Implementation{"test-client", "1.0"};
    auto future = client->SendRequest(methods::kInitialize,
        SerializeInitializeRequestParams(params), {},
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    ASSERT_FALSE(result.Contains("code"));
    auto init = DeserializeInitializeResult(result);
    EXPECT_EQ(init.protocol_version, std::string(kLegacyProtocolVersion));
    EXPECT_EQ(init.server_info.name, "test-server");

    server->Close();
    client->Close();
}
