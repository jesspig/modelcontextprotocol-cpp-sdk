// McpServerTests — unit tests for McpServer creation, tool/resource/prompt registration

#include <mcp/server/McpServer.hpp>
#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/storage/FileTaskStore.hpp>

#include <mcp/test/McpTest.hpp>

#include <chrono>
#include <filesystem>
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

// ── Transport Start is idempotent: pre-Starting the transport before
// McpServer::Create must not break the session ──
TEST(McpServerTest, PreStartedTransportRemainsFunctional) {
    auto pair = InMemoryTransport::CreatePair();
    pair.server->Start();

    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(pair.server, sopts);

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

// ── HandleInitialize falls back to the legacy protocol version when the
// client declares a version outside the supported table ──
TEST(McpServerTest, InitializeFallsBackToDefaultWhenClientVersionUnsupported) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), sopts);

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    InitializeRequestParams params;
    params.protocol_version = "2024-10-07";
    params.client_info = Implementation{"test-client", "1.0"};
    auto future = client->SendRequest(methods::kInitialize,
        SerializeInitializeRequestParams(params), {},
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    ASSERT_FALSE(result.Contains("code"));
    auto init = DeserializeInitializeResult(result);
    EXPECT_EQ(init.protocol_version,
              std::string(kLegacyProtocolVersion));

    server->Close();
    client->Close();
}

// ── HandleInitialize falls back to the default negotiated version when the
// client declares no protocol version at all ──
TEST(McpServerTest, InitializeFallsBackToDefaultWhenClientVersionMissing) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), sopts);

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    InitializeRequestParams params;
    params.protocol_version = "";
    params.client_info = Implementation{"test-client", "1.0"};
    auto future = client->SendRequest(methods::kInitialize,
        SerializeInitializeRequestParams(params), {},
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    ASSERT_FALSE(result.Contains("code"));
    auto init = DeserializeInitializeResult(result);
    EXPECT_EQ(init.protocol_version,
              std::string(kDefaultNegotiatedProtocolVersion));

    server->Close();
    client->Close();
}

// ── Tool name validation ──
namespace {

std::shared_ptr<McpServerTool> MakeTool(std::string_view name) {
    return McpServerTool::Create(name,
        std::function<CallToolResult(const Ctx&)>(
            [](const Ctx&) { return CallToolResult{}; }),
        ToolOptions{});
}

} // namespace

TEST(McpServerTest, RegisterToolRejectsInvalidName) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    EXPECT_THROW(server->RegisterTool(MakeTool("")), mcp::McpError);
    EXPECT_THROW(server->RegisterTool(MakeTool("bad name")), mcp::McpError);
    EXPECT_THROW(server->RegisterTool(MakeTool("bad@name")), mcp::McpError);
    EXPECT_THROW(server->RegisterTool(MakeTool("中文名")), mcp::McpError);
    EXPECT_THROW(server->RegisterTool(MakeTool(std::string(129, 'a'))), mcp::McpError);

    server->Close();
}

TEST(McpServerTest, RegisterToolRejectsInvalidNameWithInvalidParamsCode) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    try {
        server->RegisterTool(MakeTool("bad name"));
        EXPECT_TRUE(false);
    } catch (const mcp::McpError& e) {
        EXPECT_EQ(e.Code(), mcp::McpErrorCode::InvalidParams);
    }

    server->Close();
}

TEST(McpServerTest, RegisterToolAcceptsValidName) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    EXPECT_NO_THROW(server->RegisterTool(MakeTool("tool_1.a-b")));
    EXPECT_NO_THROW(server->RegisterTool(MakeTool(std::string(128, 'a'))));
    EXPECT_TRUE(server->GetCapabilities().tools.has_value());

    server->Close();
}

// ── Discover declares the full supported version list ──
TEST(McpServerTest, DiscoverDeclaresAllSupportedVersions) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    DiscoverRequestParams params;
    auto future = client->SendRequest(methods::kDiscover,
        SerializeDiscoverRequestParams(params), {},
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));
    auto result = DeserializeDiscoverResult(resp);
    std::vector<std::string> expected;
    for (auto v : kProtocolVersions) expected.emplace_back(v);
    EXPECT_EQ(result.supported_versions, expected);

    server->Close();
    client->Close();
}

// ── Task status notifications (2025 era) ──
namespace {

std::filesystem::path MakeTaskStorePath() {
    auto temp_dir = std::filesystem::temp_directory_path();
    return temp_dir / ("mcp_server_tasks_" +
        std::string(mcp::test::CurrentTestName()) + ".json");
}

std::string NotificationField(const JsonRpcNotification& n, const char* key) {
    if (!n.params || !n.params->IsObject()) return "";
    auto* v = n.params->Find(key);
    return (v && v->IsString()) ? v->GetString() : "";
}

} // namespace

TEST(McpServerTest, UpdateTaskSendsCompletedNotification) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLegacyProtocolVersion);
    auto store_path = MakeTaskStorePath();
    std::error_code ec;
    std::filesystem::remove(store_path, ec);
    auto store = std::make_shared<FileTaskStore>(store_path);
    sopts.task_store = store;
    auto server = McpServer::Create(std::move(pair.server), sopts);

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    store->CreateTask("task-completed");

    std::promise<void> received;
    auto received_future = received.get_future();
    std::string received_status;
    client->SetNotificationHandler(notifications::kTaskCompleted,
        [&received, &received_status](const JsonRpcNotification& n) {
            received_status = NotificationField(n, "status");
            received.set_value();
        });

    UpdateTaskRequestParams params;
    params.task_id = "task-completed";
    JsonValue result(JsonValue::object_tag);
    result["answer"] = JsonValue(42);
    params.result = result;
    auto future = client->SendRequest(methods::kUpdateTask,
        SerializeUpdateTaskRequestParams(params), {},
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));
    ASSERT_EQ(received_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    EXPECT_EQ(received_status, "completed");

    server->Close();
    client->Close();
    std::filesystem::remove(store_path, ec);
}

TEST(McpServerTest, UpdateTaskSendsWorkingNotification) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLegacyProtocolVersion);
    auto store_path = MakeTaskStorePath();
    std::error_code ec;
    std::filesystem::remove(store_path, ec);
    auto store = std::make_shared<FileTaskStore>(store_path);
    sopts.task_store = store;
    auto server = McpServer::Create(std::move(pair.server), sopts);

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    store->CreateTask("task-working");

    std::promise<void> received;
    auto received_future = received.get_future();
    std::string received_task_id;
    std::string received_status;
    client->SetNotificationHandler(notifications::kTaskWorking,
        [&received, &received_task_id, &received_status](const JsonRpcNotification& n) {
            received_task_id = NotificationField(n, "taskId");
            received_status = NotificationField(n, "status");
            received.set_value();
        });

    UpdateTaskRequestParams params;
    params.task_id = "task-working";
    auto future = client->SendRequest(methods::kUpdateTask,
        SerializeUpdateTaskRequestParams(params), {},
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));
    ASSERT_EQ(received_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    EXPECT_EQ(received_task_id, "task-working");
    EXPECT_EQ(received_status, "working");

    server->Close();
    client->Close();
    std::filesystem::remove(store_path, ec);
}

TEST(McpServerTest, CancelTaskSendsCancelledNotification) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLegacyProtocolVersion);
    auto store_path = MakeTaskStorePath();
    std::error_code ec;
    std::filesystem::remove(store_path, ec);
    auto store = std::make_shared<FileTaskStore>(store_path);
    sopts.task_store = store;
    auto server = McpServer::Create(std::move(pair.server), sopts);

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    store->CreateTask("task-cancelled");

    std::promise<void> received;
    auto received_future = received.get_future();
    std::string received_status;
    client->SetNotificationHandler(notifications::kTaskCancelled,
        [&received, &received_status](const JsonRpcNotification& n) {
            received_status = NotificationField(n, "status");
            received.set_value();
        });

    CancelTaskRequestParams params;
    params.task_id = "task-cancelled";
    params.reason = "user aborted";
    auto future = client->SendRequest(methods::kCancelTask,
        SerializeCancelTaskRequestParams(params), {},
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));
    ASSERT_EQ(received_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    EXPECT_EQ(received_status, "cancelled");

    server->Close();
    client->Close();
    std::filesystem::remove(store_path, ec);
}

TEST(McpServerTest, SendTaskStatusDeliversStatusNotification) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    std::promise<void> received;
    auto received_future = received.get_future();
    std::string received_task_id;
    std::string received_status;
    client->SetNotificationHandler(notifications::kTaskStatus,
        [&received, &received_task_id, &received_status](const JsonRpcNotification& n) {
            received_task_id = NotificationField(n, "taskId");
            received_status = NotificationField(n, "status");
            received.set_value();
        });

    server->SendTaskStatus("task-1", TaskStatus::Failed);

    ASSERT_EQ(received_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    EXPECT_EQ(received_task_id, "task-1");
    EXPECT_EQ(received_status, "failed");

    server->Close();
    client->Close();
}

// ── subscriptions/listen acknowledges the honored filter (2026 era) ──
namespace {

struct ModernServerWithClient {
    std::unique_ptr<McpServer> server;
    std::shared_ptr<McpSessionHandler> client;
};

ModernServerWithClient MakeModernServerWithClient() {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));
    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();
    return ModernServerWithClient{std::move(server), std::move(client)};
}

void NegotiateModernVersion(const std::unique_ptr<McpServer>& server) {
    server->GetSessionHandler().SetNegotiatedProtocolVersion(kLatestProtocolVersion);
}

RequestMeta MetaWithSubscriptionId(std::string_view subscription_id) {
    RequestMeta meta;
    JsonValue ext(JsonValue::object_tag);
    ext["io.modelcontextprotocol/subscriptionId"] =
        JsonValue(std::string(subscription_id));
    meta.extensions = std::move(ext);
    return meta;
}

} // namespace

TEST(McpServerTest, SubscriptionsListenSendsAcknowledgedFirstFrame) {
    auto ctx = MakeModernServerWithClient();
    NegotiateModernVersion(ctx.server);

    std::promise<JsonRpcNotification> ack_received;
    auto ack_future = ack_received.get_future();
    ctx.client->SetNotificationHandler(notifications::kSubscriptionsAcknowledged,
        [&ack_received](const JsonRpcNotification& n) {
            ack_received.set_value(n);
        });

    SubscriptionsListenRequestParams params;
    params.notifications.tools_list_changed = true;
    auto future = ctx.client->SendRequest(methods::kSubscribe,
        SerializeSubscriptionsListenRequestParams(params),
        MetaWithSubscriptionId("client-sub-1"),
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));
    EXPECT_TRUE(resp.IsObject());
    ASSERT_TRUE(resp.Contains("resultType"));
    EXPECT_EQ(resp["resultType"], JsonValue("complete"));

    ASSERT_EQ(ack_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto ack = ack_future.get();
    EXPECT_EQ(ack.method, std::string(notifications::kSubscriptionsAcknowledged));
    ASSERT_TRUE(ack.meta.has_value());
    auto* sid = ack.meta->Find("io.modelcontextprotocol/subscriptionId");
    ASSERT_NE(sid, nullptr);
    EXPECT_EQ(sid->GetString(), "client-sub-1");

    ctx.server->Close();
    ctx.client->Close();
}

TEST(McpServerTest, SubscriptionsListenAcknowledgesHonoredFilter) {
    auto ctx = MakeModernServerWithClient();
    NegotiateModernVersion(ctx.server);

    std::promise<JsonRpcNotification> ack_received;
    auto ack_future = ack_received.get_future();
    ctx.client->SetNotificationHandler(notifications::kSubscriptionsAcknowledged,
        [&ack_received](const JsonRpcNotification& n) {
            ack_received.set_value(n);
        });

    SubscriptionsListenRequestParams params;
    params.notifications.tools_list_changed = true;
    params.notifications.prompts_list_changed = false;
    params.notifications.resources_list_changed = true;
    params.notifications.resource_subscriptions = {"resource://sub/1"};
    auto future = ctx.client->SendRequest(methods::kSubscribe,
        SerializeSubscriptionsListenRequestParams(params),
        MetaWithSubscriptionId("client-sub-2"),
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));

    ASSERT_EQ(ack_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto ack = ack_future.get();
    ASSERT_TRUE(ack.params.has_value());
    auto honored = DeserializeSubscriptionsAcknowledgedNotificationParams(*ack.params);
    EXPECT_EQ(honored.notifications.tools_list_changed.value_or(false), true);
    EXPECT_EQ(honored.notifications.prompts_list_changed.value_or(true), false);
    EXPECT_EQ(honored.notifications.resources_list_changed.value_or(false), true);
    ASSERT_EQ(honored.notifications.resource_subscriptions.size(), 1u);
    EXPECT_EQ(honored.notifications.resource_subscriptions[0], "resource://sub/1");

    ctx.server->Close();
    ctx.client->Close();
}
