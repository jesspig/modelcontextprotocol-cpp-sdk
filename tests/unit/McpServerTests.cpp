#include <mcp/server/McpServer.hpp>
#include <mcp/server/RequestState.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/ext/Apps.hpp>
#include <mcp/ext/ServerCard.hpp>
#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/storage/FileTaskStore.hpp>

#include <gtest/gtest.h>
#include <mcp/test/McpTestInfo.hpp>
#include <mcp/test/McpTimeout.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

using namespace mcp;
using Ctx = RequestContext<CallToolRequestParams>;

TEST(McpServerTest, CreateAndDestroy) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));
    ASSERT_NE(server, nullptr);
    server->Close();
}

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

TEST(McpServerTest, ReadResourceMatchesRegisteredTemplate) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), sopts);

    server->RegisterResourceTemplate("template-resource",
        "test://template/{id}/data",
        ResourceOptions{},
        [](const std::string& uri,
           const std::map<std::string, std::string>& vars) -> ReadResourceResult {
            TextResourceContents trc;
            trc.uri = uri;
            trc.text = "id=" + vars.at("id");
            ReadResourceResult rr;
            rr.contents = {mcp::ResourceContents{trc}};
            return rr;
        });

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();

    ResourceRequestParams params;
    params.uri = "test://template/123/data";
    auto future = client->SendRequest(methods::kReadResource,
        SerializeResourceRequestParams(params), {}, std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));
    auto* contents = resp.Find("contents");
    ASSERT_NE(contents, nullptr);
    ASSERT_TRUE(contents->IsArray());
    ASSERT_EQ(contents->Size(), 1u);
    auto* text = (*contents)[0].Find("text");
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->GetString(), "id=123");

    server->Close();
    client->Close();
}

TEST(McpServerTest, ReadResourceTemplateValueIsPctDecoded) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), sopts);

    server->RegisterResourceTemplate("template-resource",
        "test://template/{id}/data",
        ResourceOptions{},
        [](const std::string& uri,
           const std::map<std::string, std::string>& vars) -> ReadResourceResult {
            TextResourceContents trc;
            trc.uri = uri;
            trc.text = "id=" + vars.at("id");
            ReadResourceResult rr;
            rr.contents = {mcp::ResourceContents{trc}};
            return rr;
        });

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();

    ResourceRequestParams params;
    params.uri = "test://template/a%20b/data";
    auto future = client->SendRequest(methods::kReadResource,
        SerializeResourceRequestParams(params), {}, std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));
    auto* contents = resp.Find("contents");
    ASSERT_NE(contents, nullptr);
    ASSERT_TRUE(contents->IsArray());
    ASSERT_EQ(contents->Size(), 1u);
    auto* text = (*contents)[0].Find("text");
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->GetString(), "id=a b");

    server->Close();
    client->Close();
}

TEST(McpServerTest, ReadResourceUnknownUriStillNotFound) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), sopts);

    server->RegisterResourceTemplate("template-resource",
        "test://template/{id}/data",
        ResourceOptions{},
        [](const std::string& uri,
           const std::map<std::string, std::string>& vars) -> ReadResourceResult {
            (void)vars;
            TextResourceContents trc;
            trc.uri = uri;
            trc.text = "unused";
            ReadResourceResult rr;
            rr.contents = {mcp::ResourceContents{trc}};
            return rr;
        });

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();

    ResourceRequestParams params;
    params.uri = "test://unknown";
    auto future = client->SendRequest(methods::kReadResource,
        SerializeResourceRequestParams(params), {}, std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_TRUE(resp.Contains("code"));
    EXPECT_EQ(resp["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::InvalidParams));

    server->Close();
    client->Close();
}

TEST(McpServerTest, ReadResourceMissCarriesUri) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), sopts);

    server->RegisterResourceTemplate("template-resource",
        "test://template/{id}/data",
        ResourceOptions{},
        [](const std::string& uri,
           const std::map<std::string, std::string>& vars) -> ReadResourceResult {
            (void)vars;
            TextResourceContents trc;
            trc.uri = uri;
            trc.text = "unused";
            ReadResourceResult rr;
            rr.contents = {mcp::ResourceContents{trc}};
            return rr;
        });

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();

    ResourceRequestParams params;
    params.uri = "test://missing-resource";
    auto future = client->SendRequest(methods::kReadResource,
        SerializeResourceRequestParams(params), {}, std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_TRUE(resp.Contains("code"));
    EXPECT_EQ(resp["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::InvalidParams));
    ASSERT_TRUE(resp.Contains("data"));
    auto* uri = resp["data"].Find("uri");
    ASSERT_NE(uri, nullptr);
    EXPECT_EQ(uri->GetString(), "test://missing-resource");

    server->Close();
    client->Close();
}

TEST(McpServerTest, ReadResourcePrefersStaticOverTemplate) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), sopts);

    server->RegisterResourceTemplate("catch-all-template",
        "test://{id}",
        ResourceOptions{},
        [](const std::string& uri,
           const std::map<std::string, std::string>& vars) -> ReadResourceResult {
            TextResourceContents trc;
            trc.uri = uri;
            trc.text = "template:" + vars.at("id");
            ReadResourceResult rr;
            rr.contents = {mcp::ResourceContents{trc}};
            return rr;
        });

    server->RegisterResource("static-resource", "test://x",
        ResourceOptions{},
        [](const std::string& uri) -> ReadResourceResult {
            TextResourceContents trc;
            trc.uri = uri;
            trc.text = "static";
            ReadResourceResult rr;
            rr.contents = {mcp::ResourceContents{trc}};
            return rr;
        });

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();

    ResourceRequestParams params;
    params.uri = "test://x";
    auto future = client->SendRequest(methods::kReadResource,
        SerializeResourceRequestParams(params), {}, std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));
    auto* contents = resp.Find("contents");
    ASSERT_NE(contents, nullptr);
    ASSERT_TRUE(contents->IsArray());
    ASSERT_EQ(contents->Size(), 1u);
    auto* text = (*contents)[0].Find("text");
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->GetString(), "static");

    server->Close();
    client->Close();
}

TEST(McpServerTest, RegisterResourceTemplateRejectsInvalidTemplate) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    auto handler = [](const std::string&,
                      const std::map<std::string, std::string>&) -> ReadResourceResult {
        return ReadResourceResult{};
    };

    auto register_unbalanced = [&] {
        server->RegisterResourceTemplate("bad-unbalanced",
            "test://{unclosed", ResourceOptions{}, handler);
    };
    auto register_bad_var = [&] {
        server->RegisterResourceTemplate("bad-varname",
            "bad://{a..b}", ResourceOptions{}, handler);
    };

    EXPECT_THROW(register_unbalanced(), mcp::McpError);
    EXPECT_THROW(register_bad_var(), mcp::McpError);

    try {
        register_unbalanced();
        EXPECT_TRUE(false);
    } catch (const mcp::McpError& e) {
        EXPECT_EQ(e.Code(), mcp::McpErrorCode::InvalidParams);
    }

    EXPECT_FALSE(server->GetCapabilities().resources.has_value());
    server->Close();
}

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

TEST(McpServerTest, ExplicitCapabilityDeclarationDefaultsOff) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    EXPECT_FALSE(server->GetCapabilities().logging.has_value());
    EXPECT_FALSE(server->GetCapabilities().completions.has_value());
    server->Close();
}

TEST(McpServerTest, InitializeDeclaresExplicitLoggingAndCompletions) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.declare_logging = true;
    sopts.declare_completions = true;
    auto server = McpServer::Create(std::move(pair.server), sopts);

    EXPECT_TRUE(server->GetCapabilities().logging.has_value());
    EXPECT_TRUE(server->GetCapabilities().completions.has_value());

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
    auto* caps = result.Find("capabilities");
    ASSERT_NE(caps, nullptr);
    auto* logging = caps->Find("logging");
    ASSERT_NE(logging, nullptr);
    EXPECT_TRUE(logging->IsObject());
    auto* completions = caps->Find("completions");
    ASSERT_NE(completions, nullptr);
    EXPECT_TRUE(completions->IsObject());

    server->Close();
    client->Close();
}

TEST(McpServerTest, SendLoggingMessageEncodesLevelAsString) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    std::promise<JsonRpcNotification> received;
    auto received_future = received.get_future();
    client->SetNotificationHandler(notifications::kMessage,
        [&received](const JsonRpcNotification& n) { received.set_value(n); });

    server->SendLoggingMessage(LoggingLevel::Info, "hello");

    ASSERT_EQ(received_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    auto notif = received_future.get();
    ASSERT_TRUE(notif.params.has_value());
    auto* level = notif.params->Find("level");
    ASSERT_NE(level, nullptr);
    EXPECT_TRUE(level->IsString());
    EXPECT_EQ(level->GetString(), "info");
    auto* data = notif.params->Find("data");
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->GetString(), "hello");

    server->Close();
    client->Close();
}

namespace {

struct ModernLoggingEnv {
    std::unique_ptr<McpServer> server;
    std::shared_ptr<McpSessionHandler> client;
    std::shared_ptr<std::atomic<int>> message_count;
};

ModernLoggingEnv MakeModernLoggingEnv() {
    ModernLoggingEnv env;
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    env.server = McpServer::Create(std::move(pair.server), sopts);
    env.client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    env.client->Start();
    env.message_count = std::make_shared<std::atomic<int>>(0);
    auto count = env.message_count;
    env.client->SetNotificationHandler(notifications::kMessage,
        [count](const JsonRpcNotification&) { count->fetch_add(1); });
    return env;
}

JsonValue CallLogProbe(ModernLoggingEnv& env,
                       std::optional<LoggingLevel> log_level)
{
    RequestMeta meta;
    meta.log_level = log_level;
    CallToolRequestParams params;
    params.name = "log_probe";
    auto future = env.client->SendRequest(methods::kCallTool,
        SerializeCallToolRequestParams(params), meta,
        std::chrono::milliseconds(2000));
    if (future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        ADD_FAILURE();
        return JsonValue(JsonValue::object_tag);
    }
    return future.get();
}

bool WaitForMessageCount(const std::shared_ptr<std::atomic<int>>& count,
                         int expected)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (count->load() >= expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return count->load() >= expected;
}

} // namespace

TEST(McpServerTest, ModernLoggingRequiresRequestLogLevel) {
    std::atomic<bool> handler_ran{false};
    auto env = MakeModernLoggingEnv();
    env.server->RegisterTool("log_probe",
        ToolOptions{}.Description("logging probe"),
        std::function<CallToolResult(const Ctx&)>(
            [&handler_ran](const Ctx& ctx) {
                ctx.Log(LoggingLevel::Info, "hidden");
                handler_ran.store(true);
                return CallToolResult{};
            }));

    auto resp = CallLogProbe(env, std::nullopt);
    EXPECT_FALSE(resp.Contains("code"));
    EXPECT_TRUE(handler_ran.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(env.message_count->load(), 0);

    env.server->Close();
    env.client->Close();
}

TEST(McpServerTest, ModernLoggingHonorsRequestLogLevel) {
    auto env = MakeModernLoggingEnv();
    env.server->RegisterTool("log_probe",
        ToolOptions{}.Description("logging probe"),
        std::function<CallToolResult(const Ctx&)>(
            [](const Ctx& ctx) {
                ctx.Log(LoggingLevel::Info, "visible");
                return CallToolResult{};
            }));

    auto debug_resp = CallLogProbe(env, LoggingLevel::Debug);
    EXPECT_FALSE(debug_resp.Contains("code"));
    EXPECT_TRUE(WaitForMessageCount(env.message_count, 1));

    auto warning_resp = CallLogProbe(env, LoggingLevel::Warning);
    EXPECT_FALSE(warning_resp.Contains("code"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(env.message_count->load(), 1);

    env.server->Close();
    env.client->Close();
}

TEST(McpServerTest, ModernSendLoggingMessageWithoutRequestIsSilent) {
    auto env = MakeModernLoggingEnv();

    env.server->SendLoggingMessage(LoggingLevel::Info, "broadcast");

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(env.message_count->load(), 0);

    env.server->Close();
    env.client->Close();
}

TEST(McpServerTest, ListPromptsIncludesArguments) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), sopts);

    PromptArgument arg;
    arg.name = "city";
    arg.description = "City name";
    arg.required = true;
    server->RegisterPrompt("weather",
        PromptOptions{}.Description("Weather lookup").Arguments({arg}),
        [](const std::string& name,
           const std::optional<JsonValue>& args) -> GetPromptResult {
            (void)name; (void)args;
            return GetPromptResult{};
        });

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();

    auto future = client->SendRequest(methods::kListPrompts,
        JsonValue(JsonValue::object_tag), {}, std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));
    auto* prompts_arr = resp.Find("prompts");
    ASSERT_NE(prompts_arr, nullptr);
    ASSERT_TRUE(prompts_arr->IsArray());
    ASSERT_EQ(prompts_arr->Size(), 1u);
    const auto& entry = (*prompts_arr)[0];
    EXPECT_EQ(entry.Find("name")->GetString(), "weather");
    auto* args = entry.Find("arguments");
    ASSERT_NE(args, nullptr);
    ASSERT_TRUE(args->IsArray());
    ASSERT_EQ(args->Size(), 1u);
    const auto& wire_arg = (*args)[0];
    EXPECT_EQ(wire_arg.Find("name")->GetString(), "city");
    ASSERT_NE(wire_arg.Find("description"), nullptr);
    EXPECT_EQ(wire_arg.Find("description")->GetString(), "City name");
    ASSERT_NE(wire_arg.Find("required"), nullptr);
    EXPECT_EQ(wire_arg.Find("required")->GetBool(), true);

    server->Close();
    client->Close();
}

TEST(McpServerTest, McpServerToolFactory) {
    auto tool = McpServerTool::Create("calc",
        std::function<CallToolResult(const Ctx&)>(
            [](const Ctx&) { return CallToolResult{}; }),
        ToolOptions{}.Description("Calculator"));

    ASSERT_NE(tool, nullptr);
    EXPECT_EQ(tool->ProtocolTool().name, "calc");
    EXPECT_EQ(tool->ProtocolTool().description, "Calculator");
}

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

TEST(McpServerTest, ClientCapabilitiesInitialState) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    EXPECT_EQ(server->GetClientCapabilities(), nullptr);
    EXPECT_EQ(server->GetClientInfo(), nullptr);
    server->Close();
}

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

TEST(McpServerTest, ServesRequestsBeforeInitializedInModernEra) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), sopts);

    server->RegisterTool(McpServerTool::Create("echo",
        std::function<CallToolResult(const Ctx&)>(
            [](const Ctx&) { return CallToolResult{}; }),
        ToolOptions{}.Description("Echo input")));

    ClientOptions cops;
    cops.connect_mode = ConnectMode::Pin;
    cops.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), cops);

    auto result = client->CallTool("echo", JsonValue(JsonValue::object_tag));
    EXPECT_FALSE(result.is_error);

    client->Close();
    server->Close();
}

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

TEST(McpServerTest, DiscoverMirrorsServerInfoInResultMeta) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions opts;
    opts.server_info = Implementation{"meta-server", "2.0.0"};
    auto server = McpServer::Create(std::move(pair.server), opts);

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
    auto* meta = resp.Find("_meta");
    ASSERT_NE(meta, nullptr);
    ASSERT_TRUE(meta->IsObject());
    auto* mirrored = meta->Find("io.modelcontextprotocol/serverInfo");
    ASSERT_NE(mirrored, nullptr);
    EXPECT_EQ(mirrored->Find("name")->GetString(), "meta-server");
    EXPECT_EQ(mirrored->Find("version")->GetString(), "2.0.0");

    JsonValue body_only = resp;
    body_only.GetObject().erase(std::string("serverInfo"));
    auto recovered = DeserializeDiscoverResult(body_only);
    EXPECT_EQ(recovered.server_info.name, "meta-server");
    EXPECT_EQ(recovered.server_info.version, "2.0.0");

    server->Close();
    client->Close();
}

namespace {

void ExpectServerInfoMeta(const JsonValue& result,
                          std::string_view name,
                          std::string_view version) {
    ASSERT_TRUE(result.IsObject());
    auto* meta = result.Find("_meta");
    ASSERT_NE(meta, nullptr);
    ASSERT_TRUE(meta->IsObject());
    auto* info = meta->Find("io.modelcontextprotocol/serverInfo");
    ASSERT_NE(info, nullptr);
    ASSERT_TRUE(info->IsObject());
    auto* actual_name = info->Find("name");
    ASSERT_NE(actual_name, nullptr);
    EXPECT_EQ(actual_name->GetString(), std::string(name));
    auto* actual_version = info->Find("version");
    ASSERT_NE(actual_version, nullptr);
    EXPECT_EQ(actual_version->GetString(), std::string(version));
}

JsonValue SendServerInfoRequest(
    const std::shared_ptr<McpSessionHandler>& client,
    std::string_view method,
    JsonValue params) {
    auto future = client->SendRequest(method, std::move(params), RequestMeta{},
        std::chrono::milliseconds(2000));
    if (future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        ADD_FAILURE();
        return JsonValue(JsonValue::object_tag);
    }
    return future.get();
}

std::vector<std::string> ToolNames(const JsonValue& result) {
    std::vector<std::string> names;
    const JsonValue* tools = result.Find("tools");
    if (!tools || !tools->IsArray()) return names;
    for (const auto& tool : tools->GetArray()) {
        const JsonValue* name = tool.Find("name");
        if (name && name->IsString()) names.push_back(name->GetString());
    }
    return names;
}

struct ToolListEnv {
    std::unique_ptr<McpServer> server;
    std::shared_ptr<McpSessionHandler> client;
};

ToolListEnv MakeToolListEnv(std::initializer_list<std::string_view> names,
                            bool progressive_discovery = false) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions opts;
    opts.server_info = Implementation{"tool-order-server", "1.0.0"};
    opts.protocol_version = std::string(kLatestProtocolVersion);
    opts.enable_progressive_discovery = progressive_discovery;
    ToolListEnv env;
    env.server = McpServer::Create(std::move(pair.server), opts);
    for (auto name : names) {
        env.server->RegisterTool(name, ToolOptions{},
            [](const Ctx&) { return CallToolResult{}; });
    }
    env.client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    env.client->Start();
    return env;
}

void CloseToolListEnv(ToolListEnv& env) {
    env.client->Close();
    env.server->Close();
}

} // namespace

TEST(McpServerTest, ModernResultsCarryServerInfoMeta) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions opts;
    opts.server_info = Implementation{"meta-server", "2.0.0"};
    opts.protocol_version = std::string(kLatestProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), opts);
    server->RegisterTool("echo", ToolOptions{},
        [](const Ctx&) {
            CallToolResult result;
            result.content.push_back(TextContent{"text", "ok"});
            return result;
        });
    server->RegisterResource("info-resource", "test://server-info",
        ResourceOptions{},
        [](const std::string& uri) {
            TextResourceContents contents;
            contents.uri = uri;
            contents.text = "info";
            ReadResourceResult result;
            result.contents = {mcp::ResourceContents{contents}};
            return result;
        });
    server->RegisterPrompt("greet", PromptOptions{},
        [](const std::string&, const std::optional<JsonValue>&) {
            PromptMessage message;
            message.role = "user";
            message.content = TextContent{"text", "Hello"};
            GetPromptResult result;
            result.messages = {std::move(message)};
            return result;
        });

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();

    auto tools = SendServerInfoRequest(client, methods::kListTools,
        JsonValue(JsonValue::object_tag));
    ASSERT_FALSE(tools.Contains("code"));
    ExpectServerInfoMeta(tools, "meta-server", "2.0.0");

    CallToolRequestParams call_params;
    call_params.name = "echo";
    auto call = SendServerInfoRequest(client, methods::kCallTool,
        SerializeCallToolRequestParams(call_params));
    ASSERT_FALSE(call.Contains("code"));
    ExpectServerInfoMeta(call, "meta-server", "2.0.0");

    ResourceRequestParams read_params;
    read_params.uri = "test://server-info";
    auto read = SendServerInfoRequest(client, methods::kReadResource,
        SerializeResourceRequestParams(read_params));
    ASSERT_FALSE(read.Contains("code"));
    ExpectServerInfoMeta(read, "meta-server", "2.0.0");

    JsonValue prompt_params(JsonValue::object_tag);
    prompt_params["name"] = JsonValue("greet");
    auto prompt = SendServerInfoRequest(client, methods::kGetPrompt,
        std::move(prompt_params));
    ASSERT_FALSE(prompt.Contains("code"));
    ExpectServerInfoMeta(prompt, "meta-server", "2.0.0");

    auto discover = SendServerInfoRequest(client, methods::kDiscover,
        SerializeDiscoverRequestParams(DiscoverRequestParams{}));
    ASSERT_FALSE(discover.Contains("code"));
    ExpectServerInfoMeta(discover, "meta-server", "2.0.0");

    server->Close();
    client->Close();
}

TEST(McpServerTest, ToolsListReturnsToolsInNameOrder) {
    auto env = MakeToolListEnv({"z", "a", "m"});

    auto result = SendServerInfoRequest(env.client, methods::kListTools,
        JsonValue(JsonValue::object_tag));
    ASSERT_FALSE(result.Contains("code"));
    EXPECT_EQ(ToolNames(result), (std::vector<std::string>{"a", "m", "z"}));

    CloseToolListEnv(env);
}

TEST(McpServerTest, ToolsListOrderIgnoresRegistrationOrder) {
    auto first = MakeToolListEnv({"z", "a", "m"});
    auto second = MakeToolListEnv({"m", "z", "a"});

    auto first_result = SendServerInfoRequest(first.client, methods::kListTools,
        JsonValue(JsonValue::object_tag));
    auto second_result = SendServerInfoRequest(second.client, methods::kListTools,
        JsonValue(JsonValue::object_tag));
    ASSERT_FALSE(first_result.Contains("code"));
    ASSERT_FALSE(second_result.Contains("code"));
    auto first_names = ToolNames(first_result);
    EXPECT_EQ(first_names, ToolNames(second_result));
    EXPECT_EQ(first_names, (std::vector<std::string>{"a", "m", "z"}));

    CloseToolListEnv(first);
    CloseToolListEnv(second);
}

TEST(McpServerTest, ProgressiveDiscoveryPaginatesToolsInNameOrder) {
    auto env = MakeToolListEnv({"z", "a", "m"}, true);

    std::vector<std::string> names;
    std::optional<std::string> cursor;
    for (int page = 0; page < 4; ++page) {
        JsonValue params(JsonValue::object_tag);
        params["pageSize"] = JsonValue(int64_t{1});
        if (cursor) params["cursor"] = JsonValue(*cursor);
        auto result = SendServerInfoRequest(env.client, methods::kListTools,
            std::move(params));
        ASSERT_FALSE(result.Contains("code"));
        auto page_names = ToolNames(result);
        names.insert(names.end(), page_names.begin(), page_names.end());
        const JsonValue* next = result.Find("nextCursor");
        if (!next || !next->IsString()) break;
        cursor = next->GetString();
    }
    EXPECT_EQ(names, (std::vector<std::string>{"a", "m", "z"}));

    CloseToolListEnv(env);
}

TEST(McpServerTest, LegacyResultsDoNotCarryServerInfoMeta) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions opts;
    opts.server_info = Implementation{"meta-server", "2.0.0"};
    opts.protocol_version = std::string(kLegacyProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), opts);
    server->RegisterTool("echo", ToolOptions{},
        [](const Ctx&) {
            CallToolResult result;
            result.content.push_back(TextContent{"text", "ok"});
            return result;
        });

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    RequestMeta meta;
    meta.protocol_version = std::string(kLegacyProtocolVersion);
    InitializeRequestParams init_params;
    init_params.protocol_version = std::string(kLegacyProtocolVersion);
    init_params.client_info = Implementation{"test-client", "1.0"};
    auto init_future = client->SendRequest(methods::kInitialize,
        SerializeInitializeRequestParams(init_params), meta,
        std::chrono::milliseconds(2000));
    ASSERT_EQ(init_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    auto init_result = init_future.get();
    ASSERT_FALSE(init_result.Contains("code"));
    EXPECT_FALSE(init_result.Contains("_meta"));
    auto* legacy_info = init_result.Find("serverInfo");
    ASSERT_NE(legacy_info, nullptr);
    ASSERT_NE(legacy_info->Find("name"), nullptr);
    EXPECT_EQ(legacy_info->Find("name")->GetString(), "meta-server");
    client->SendNotification(notifications::kInitialized, JsonValue{});

    CallToolRequestParams call_params;
    call_params.name = "echo";
    auto future = client->SendRequest(methods::kCallTool,
        SerializeCallToolRequestParams(call_params), meta,
        std::chrono::milliseconds(2000));
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    ASSERT_FALSE(result.Contains("code"));
    EXPECT_FALSE(result.Contains("_meta"));
    EXPECT_FALSE(result.Contains("io.modelcontextprotocol/serverInfo"));

    server->Close();
    client->Close();
}

TEST(McpServerTest, ModernResultsPreserveCallerServerInfoMeta) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions opts;
    opts.server_info = Implementation{"meta-server", "2.0.0"};
    opts.protocol_version = std::string(kLatestProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), opts);
    server->RegisterTool("custom-meta", ToolOptions{},
        [](const Ctx&) {
            CallToolResult result;
            result.content.push_back(TextContent{"text", "ok"});
            JsonValue meta(JsonValue::object_tag);
            meta["custom"] = JsonValue("kept");
            meta["io.modelcontextprotocol/serverInfo"] =
                SerializeImplementation(Implementation{"caller-server", "9.9.9"});
            result.meta = std::move(meta);
            return result;
        });
    server->RegisterTool("partial-meta", ToolOptions{},
        [](const Ctx&) {
            CallToolResult result;
            result.content.push_back(TextContent{"text", "ok"});
            JsonValue meta(JsonValue::object_tag);
            meta["custom"] = JsonValue("kept");
            result.meta = std::move(meta);
            return result;
        });

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();

    CallToolRequestParams call_params;
    call_params.name = "custom-meta";
    auto custom = SendServerInfoRequest(client, methods::kCallTool,
        SerializeCallToolRequestParams(call_params));
    ASSERT_FALSE(custom.Contains("code"));
    auto* custom_meta = custom.Find("_meta");
    ASSERT_NE(custom_meta, nullptr);
    ASSERT_TRUE(custom_meta->IsObject());
    ASSERT_NE(custom_meta->Find("custom"), nullptr);
    EXPECT_EQ(custom_meta->Find("custom")->GetString(), "kept");
    auto* caller_info =
        custom_meta->Find("io.modelcontextprotocol/serverInfo");
    ASSERT_NE(caller_info, nullptr);
    ASSERT_NE(caller_info->Find("name"), nullptr);
    EXPECT_EQ(caller_info->Find("name")->GetString(), "caller-server");
    ASSERT_NE(caller_info->Find("version"), nullptr);
    EXPECT_EQ(caller_info->Find("version")->GetString(), "9.9.9");

    call_params.name = "partial-meta";
    auto partial = SendServerInfoRequest(client, methods::kCallTool,
        SerializeCallToolRequestParams(call_params));
    ASSERT_FALSE(partial.Contains("code"));
    ExpectServerInfoMeta(partial, "meta-server", "2.0.0");
    auto* partial_meta = partial.Find("_meta");
    ASSERT_NE(partial_meta, nullptr);
    ASSERT_NE(partial_meta->Find("custom"), nullptr);
    EXPECT_EQ(partial_meta->Find("custom")->GetString(), "kept");

    server->Close();
    client->Close();
}

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
    sopts.enable_tasks_extension = true;
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
    sopts.enable_tasks_extension = true;
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
    sopts.enable_tasks_extension = true;
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

TEST(McpServerTest, SendProgressDeliversNotificationWithInt64Token) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    std::promise<JsonRpcNotification> received;
    auto received_future = received.get_future();
    client->SetNotificationHandler(notifications::kProgress,
        [&received](const JsonRpcNotification& n) { received.set_value(n); });

    server->SendProgress(ProgressToken{int64_t{42}}, 0.75, 1.0, "three quarters");

    ASSERT_EQ(received_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    auto notif = received_future.get();
    EXPECT_EQ(notif.method, std::string(notifications::kProgress));
    ASSERT_TRUE(notif.params.has_value());
    auto* token = notif.params->Find("progressToken");
    ASSERT_NE(token, nullptr);
    EXPECT_TRUE(token->IsInt());
    EXPECT_EQ(token->GetInt(), 42);
    auto* progress = notif.params->Find("progress");
    ASSERT_NE(progress, nullptr);
    EXPECT_EQ(progress->GetDouble(), 0.75);
    auto* total = notif.params->Find("total");
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->GetDouble(), 1.0);
    auto* message = notif.params->Find("message");
    ASSERT_NE(message, nullptr);
    EXPECT_EQ(message->GetString(), "three quarters");

    server->Close();
    client->Close();
}

TEST(McpServerTest, SendProgressDeliversNotificationWithStringToken) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    std::promise<JsonRpcNotification> received;
    auto received_future = received.get_future();
    client->SetNotificationHandler(notifications::kProgress,
        [&received](const JsonRpcNotification& n) { received.set_value(n); });

    server->SendProgress(ProgressToken{std::string("tok-1")}, 0.5);

    ASSERT_EQ(received_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    auto notif = received_future.get();
    EXPECT_EQ(notif.method, std::string(notifications::kProgress));
    ASSERT_TRUE(notif.params.has_value());
    auto* token = notif.params->Find("progressToken");
    ASSERT_NE(token, nullptr);
    EXPECT_TRUE(token->IsString());
    EXPECT_EQ(token->GetString(), "tok-1");
    auto* progress = notif.params->Find("progress");
    ASSERT_NE(progress, nullptr);
    EXPECT_EQ(progress->GetDouble(), 0.5);
    EXPECT_FALSE(notif.params->Contains("total"));
    EXPECT_FALSE(notif.params->Contains("message"));

    server->Close();
    client->Close();
}

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

    ASSERT_EQ(ack_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));
    EXPECT_TRUE(resp.IsObject());
    ASSERT_TRUE(resp.Contains("resultType"));
    EXPECT_EQ(resp["resultType"], JsonValue("complete"));

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

    ASSERT_EQ(ack_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));

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

TEST(McpServerTest, ToolListChangedHonorsSubscriptionFilter) {
    auto ctx = MakeModernServerWithClient();
    NegotiateModernVersion(ctx.server);

    std::mutex observed_mutex;
    std::vector<std::string> observed_ids;
    ctx.client->SetNotificationHandler(notifications::kToolListChanged,
        [&observed_mutex, &observed_ids](const JsonRpcNotification& n) {
            std::string sid;
            if (n.meta && n.meta->IsObject()) {
                if (auto* v = n.meta->Find("io.modelcontextprotocol/subscriptionId");
                    v && v->IsString()) {
                    sid = v->GetString();
                }
            }
            std::lock_guard<std::mutex> lock(observed_mutex);
            observed_ids.push_back(sid);
        });

    SubscriptionsListenRequestParams tools_only;
    tools_only.notifications.tools_list_changed = true;
    auto tools_future = ctx.client->SendRequest(methods::kSubscribe,
        SerializeSubscriptionsListenRequestParams(tools_only),
        MetaWithSubscriptionId("sub-tools"),
        std::chrono::milliseconds(2000));

    SubscriptionsListenRequestParams prompts_only;
    prompts_only.notifications.prompts_list_changed = true;
    auto prompts_future = ctx.client->SendRequest(methods::kSubscribe,
        SerializeSubscriptionsListenRequestParams(prompts_only),
        MetaWithSubscriptionId("sub-prompts"),
        std::chrono::milliseconds(2000));

    ASSERT_EQ(tools_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    ASSERT_FALSE(tools_future.get().Contains("code"));
    ASSERT_EQ(prompts_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    ASSERT_FALSE(prompts_future.get().Contains("code"));

    ctx.server->SendToolListChanged();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::lock_guard<std::mutex> lock(observed_mutex);
    ASSERT_EQ(observed_ids.size(), 1u);
    EXPECT_EQ(observed_ids[0], "sub-tools");

    ctx.server->Close();
    ctx.client->Close();
}

TEST(McpServerTest, RegisterPushesListChangedToSubscribers) {
    auto ctx = MakeModernServerWithClient();

    DiscoverRequestParams discover_params;
    auto discover_future = ctx.client->SendRequest(methods::kDiscover,
        SerializeDiscoverRequestParams(discover_params), {},
        std::chrono::milliseconds(2000));
    ASSERT_EQ(discover_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    ASSERT_FALSE(discover_future.get().Contains("code"));
    ASSERT_TRUE(ctx.server->IsRunning());

    std::promise<JsonRpcNotification> ack_received;
    auto ack_future = ack_received.get_future();
    ctx.client->SetNotificationHandler(notifications::kSubscriptionsAcknowledged,
        [&ack_received](const JsonRpcNotification& n) {
            ack_received.set_value(n);
        });

    SubscriptionsListenRequestParams listener;
    listener.notifications.tools_list_changed = true;
    listener.notifications.resources_list_changed = true;
    listener.notifications.prompts_list_changed = true;
    auto listen_future = ctx.client->SendRequest(methods::kSubscribe,
        SerializeSubscriptionsListenRequestParams(listener),
        MetaWithSubscriptionId("sub-auto"),
        std::chrono::milliseconds(2000));
    ASSERT_EQ(ack_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    ASSERT_EQ(listen_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    ASSERT_FALSE(listen_future.get().Contains("code"));

    std::promise<JsonRpcNotification> tool_changed;
    auto tool_future = tool_changed.get_future();
    ctx.client->SetNotificationHandler(notifications::kToolListChanged,
        [&tool_changed](const JsonRpcNotification& n) {
            tool_changed.set_value(n);
        });
    ctx.server->RegisterTool("auto_tool", ToolOptions{},
        [](const Ctx&) { return CallToolResult{}; });
    ASSERT_EQ(tool_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto tool_notification = tool_future.get();
    EXPECT_EQ(tool_notification.method, std::string(notifications::kToolListChanged));
    ASSERT_TRUE(tool_notification.meta.has_value());
    auto* tool_sid = tool_notification.meta->Find(
        "io.modelcontextprotocol/subscriptionId");
    ASSERT_NE(tool_sid, nullptr);
    EXPECT_EQ(tool_sid->GetString(), "sub-auto");

    std::promise<JsonRpcNotification> resource_changed;
    auto resource_future = resource_changed.get_future();
    ctx.client->SetNotificationHandler(notifications::kResourceListChanged,
        [&resource_changed](const JsonRpcNotification& n) {
            resource_changed.set_value(n);
        });
    ctx.server->RegisterResource("auto_resource", "test://auto/resource",
        ResourceOptions{},
        [](const std::string&) { return ReadResourceResult{}; });
    ASSERT_EQ(resource_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    auto resource_notification = resource_future.get();
    EXPECT_EQ(resource_notification.method,
              std::string(notifications::kResourceListChanged));
    ASSERT_TRUE(resource_notification.meta.has_value());
    auto* resource_sid = resource_notification.meta->Find(
        "io.modelcontextprotocol/subscriptionId");
    ASSERT_NE(resource_sid, nullptr);
    EXPECT_EQ(resource_sid->GetString(), "sub-auto");

    std::promise<JsonRpcNotification> template_changed;
    auto template_future = template_changed.get_future();
    ctx.client->SetNotificationHandler(notifications::kResourceListChanged,
        [&template_changed](const JsonRpcNotification& n) {
            template_changed.set_value(n);
        });
    ctx.server->RegisterResourceTemplate("auto_template", "test://auto/{id}",
        ResourceOptions{},
        [](const std::string&,
           const std::map<std::string, std::string>&) {
            return ReadResourceResult{};
        });
    ASSERT_EQ(template_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    auto template_notification = template_future.get();
    EXPECT_EQ(template_notification.method,
              std::string(notifications::kResourceListChanged));

    std::promise<JsonRpcNotification> prompt_changed;
    auto prompt_future = prompt_changed.get_future();
    ctx.client->SetNotificationHandler(notifications::kPromptListChanged,
        [&prompt_changed](const JsonRpcNotification& n) {
            prompt_changed.set_value(n);
        });
    ctx.server->RegisterPrompt("auto_prompt", PromptOptions{},
        [](const std::string&,
           const std::optional<JsonValue>&) {
            return GetPromptResult{};
        });
    ASSERT_EQ(prompt_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    auto prompt_notification = prompt_future.get();
    EXPECT_EQ(prompt_notification.method,
              std::string(notifications::kPromptListChanged));
    ASSERT_TRUE(prompt_notification.meta.has_value());
    auto* prompt_sid = prompt_notification.meta->Find(
        "io.modelcontextprotocol/subscriptionId");
    ASSERT_NE(prompt_sid, nullptr);
    EXPECT_EQ(prompt_sid->GetString(), "sub-auto");

    ctx.server->Close();
    ctx.client->Close();
}

TEST(McpServerTest, RegisterBeforeDiscoverDoesNotPushListChanged) {
    auto ctx = MakeModernServerWithClient();
    EXPECT_FALSE(ctx.server->IsRunning());

    std::atomic<int> tool_notifications{0};
    std::atomic<int> resource_notifications{0};
    std::atomic<int> prompt_notifications{0};
    ctx.client->SetNotificationHandler(notifications::kToolListChanged,
        [&tool_notifications](const JsonRpcNotification&) {
            tool_notifications.fetch_add(1);
        });
    ctx.client->SetNotificationHandler(notifications::kResourceListChanged,
        [&resource_notifications](const JsonRpcNotification&) {
            resource_notifications.fetch_add(1);
        });
    ctx.client->SetNotificationHandler(notifications::kPromptListChanged,
        [&prompt_notifications](const JsonRpcNotification&) {
            prompt_notifications.fetch_add(1);
        });

    ctx.server->RegisterTool("early_tool", ToolOptions{},
        [](const Ctx&) { return CallToolResult{}; });
    ctx.server->RegisterResource("early_resource", "test://early/resource",
        ResourceOptions{},
        [](const std::string&) { return ReadResourceResult{}; });
    ctx.server->RegisterPrompt("early_prompt", PromptOptions{},
        [](const std::string&,
           const std::optional<JsonValue>&) {
            return GetPromptResult{};
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(tool_notifications.load(), 0);
    EXPECT_EQ(resource_notifications.load(), 0);
    EXPECT_EQ(prompt_notifications.load(), 0);

    auto capabilities = ctx.server->GetCapabilities();
    EXPECT_TRUE(capabilities.tools.has_value());
    EXPECT_TRUE(capabilities.resources.has_value());
    EXPECT_TRUE(capabilities.prompts.has_value());
    EXPECT_FALSE(ctx.server->IsRunning());

    ctx.server->Close();
    ctx.client->Close();
}

namespace {

struct TaskEraEnv {
    std::unique_ptr<McpServer> server;
    std::shared_ptr<McpSessionHandler> client;
    std::shared_ptr<FileTaskStore> store;
    std::filesystem::path store_path;
};

TaskEraEnv MakeTaskEraEnv() {
    TaskEraEnv env;
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLegacyProtocolVersion);
    env.store_path = MakeTaskStorePath();
    std::error_code ec;
    std::filesystem::remove(env.store_path, ec);
    env.store = std::make_shared<FileTaskStore>(env.store_path);
    sopts.task_store = env.store;
    sopts.enable_tasks_extension = true;
    env.server = McpServer::Create(std::move(pair.server), sopts);
    env.client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    env.client->Start();
    return env;
}

void PerformLegacyHandshake(const std::shared_ptr<McpSessionHandler>& client) {
    InitializeRequestParams params;
    params.protocol_version = std::string(kLegacyProtocolVersion);
    params.client_info = Implementation{"test-client", "1.0"};
    auto future = client->SendRequest(methods::kInitialize,
        SerializeInitializeRequestParams(params), {},
        std::chrono::milliseconds(2000));
    (void)future.wait_for(std::chrono::seconds(3));
    client->SendNotification(notifications::kInitialized);
}

ToolOptions MakeTaskToolOptions() {
    ToolExecution exec;
    exec.mode = ToolExecutionMode::Task;
    ToolOptions opts;
    opts.execution = exec;
    return opts;
}

std::optional<TaskState> WaitForTaskStatus(
    const std::shared_ptr<FileTaskStore>& store,
    const std::string& task_id,
    TaskStatus expected)
{
    for (int i = 0; i < 100; ++i) {
        auto task = store->GetTask(task_id);
        if (task && task->status == expected) return task;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return std::nullopt;
}

std::vector<std::string> CopyStatuses(
    std::mutex& statuses_mutex, const std::vector<std::string>& statuses) {
    std::lock_guard<std::mutex> lock(statuses_mutex);
    return statuses;
}

bool WaitForStatusNotification(
    std::mutex& statuses_mutex,
    const std::vector<std::string>& statuses,
    std::string_view expected)
{
    for (int i = 0; i < 100; ++i) {
        {
            std::lock_guard<std::mutex> lock(statuses_mutex);
            if (std::find(statuses.begin(), statuses.end(), expected)
                    != statuses.end()) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

void CloseTaskEraEnv(TaskEraEnv& env) {
    env.server->Close();
    env.client->Close();
    std::error_code ec;
    std::filesystem::remove(env.store_path, ec);
}

} // namespace

TEST(McpServerTest, CallToolTaskModeRunsInBackgroundToCompletion) {
    auto env = MakeTaskEraEnv();

    env.server->RegisterTool("slow_tool", MakeTaskToolOptions(),
        [](const Ctx&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            CallToolResult r;
            r.content.push_back(TextContent{"text", "done"});
            return r;
        });

    std::mutex notif_mutex;
    std::vector<std::string> seen_statuses;
    env.client->SetNotificationHandler(notifications::kTaskStatus,
        [&](const JsonRpcNotification& n) {
            std::lock_guard<std::mutex> lock(notif_mutex);
            seen_statuses.push_back(NotificationField(n, "status"));
        });

    PerformLegacyHandshake(env.client);

    CallToolRequestParams params;
    params.name = "slow_tool";
    auto future = env.client->SendRequest(methods::kCallTool,
        SerializeCallToolRequestParams(params), {},
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));
    auto created = DeserializeCreateTaskResult(resp);
    EXPECT_EQ(created.status, "working");
    EXPECT_FALSE(created.task_id.empty());
    EXPECT_EQ(created.task_id.compare(0, 5, "task-"), 0);
    EXPECT_FALSE(created.created_at.empty());

    auto done = WaitForTaskStatus(env.store, created.task_id, TaskStatus::Completed);
    ASSERT_TRUE(done.has_value());
    EXPECT_EQ(done->status, TaskStatus::Completed);
    ASSERT_TRUE(done->result.has_value());

    GetTaskRequestParams result_params;
    result_params.task_id = created.task_id;
    auto result_future = env.client->SendRequest(methods::kGetTaskPayload,
        SerializeGetTaskRequestParams(result_params), {},
        std::chrono::milliseconds(2000));
    ASSERT_EQ(result_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result_resp = result_future.get();
    ASSERT_FALSE(result_resp.Contains("code"));
    auto task_result = DeserializeGetTaskResult(result_resp);
    EXPECT_EQ(task_result.task_id, created.task_id);
    EXPECT_EQ(task_result.status, "completed");
    ASSERT_TRUE(task_result.result.has_value());
    EXPECT_TRUE(task_result.result->Contains("content"));

    EXPECT_TRUE(WaitForStatusNotification(notif_mutex, seen_statuses, "working"));
    EXPECT_TRUE(WaitForStatusNotification(notif_mutex, seen_statuses, "completed"));
    auto statuses = CopyStatuses(notif_mutex, seen_statuses);
    EXPECT_GE(statuses.size(), size_t{2});

    auto list_future = env.client->SendRequest(methods::kListTasks,
        JsonValue(JsonValue::object_tag), {}, std::chrono::milliseconds(2000));
    ASSERT_EQ(list_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto list_resp = list_future.get();
    ASSERT_FALSE(list_resp.Contains("code"));
    auto* tasks_arr = list_resp.Find("tasks");
    ASSERT_NE(tasks_arr, nullptr);
    bool found = false;
    for (const auto& t : tasks_arr->GetArray()) {
        auto* id = t.Find("taskId");
        if (id && id->IsString() && id->GetString() == created.task_id) found = true;
    }
    EXPECT_TRUE(found);

    CloseTaskEraEnv(env);
}

TEST(McpServerTest, CallToolTaskModeFailsWhenToolErrors) {
    auto env = MakeTaskEraEnv();

    env.server->RegisterTool("boom_tool", MakeTaskToolOptions(),
        [](const Ctx&) -> CallToolResult {
            throw std::runtime_error("boom");
        });

    std::mutex notif_mutex;
    std::vector<std::string> seen_statuses;
    env.client->SetNotificationHandler(notifications::kTaskStatus,
        [&](const JsonRpcNotification& n) {
            std::lock_guard<std::mutex> lock(notif_mutex);
            seen_statuses.push_back(NotificationField(n, "status"));
        });

    PerformLegacyHandshake(env.client);

    CallToolRequestParams params;
    params.name = "boom_tool";
    auto future = env.client->SendRequest(methods::kCallTool,
        SerializeCallToolRequestParams(params), {},
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));
    auto created = DeserializeCreateTaskResult(resp);
    EXPECT_EQ(created.status, "working");

    auto failed = WaitForTaskStatus(env.store, created.task_id, TaskStatus::Failed);
    ASSERT_TRUE(failed.has_value());
    EXPECT_EQ(failed->status, TaskStatus::Failed);

    EXPECT_TRUE(WaitForStatusNotification(notif_mutex, seen_statuses, "working"));
    EXPECT_TRUE(WaitForStatusNotification(notif_mutex, seen_statuses, "failed"));
    auto statuses = CopyStatuses(notif_mutex, seen_statuses);
    EXPECT_GE(statuses.size(), size_t{2});

    CloseTaskEraEnv(env);
}

TEST(McpServerTest, CallToolTaskModeCancelsRunningTaskCooperatively) {
    auto env = MakeTaskEraEnv();

    auto tool_started = std::make_shared<std::promise<void>>();
    auto started_future = tool_started->get_future();
    auto cancelled_early = std::make_shared<std::atomic<bool>>(false);

    env.server->RegisterTool("cancellable_tool", MakeTaskToolOptions(),
        [tool_started, cancelled_early](const Ctx& ctx) {
            tool_started->set_value();
            for (int i = 0; i < 100; ++i) {
                if (ctx.IsCancellationRequested()) {
                    cancelled_early->store(true);
                    CallToolResult r;
                    r.content.push_back(TextContent{"text", "cancelled-early"});
                    return r;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            CallToolResult r;
            r.content.push_back(TextContent{"text", "ran-to-completion"});
            return r;
        });

    std::mutex notif_mutex;
    int cancelled_notifications = 0;
    env.client->SetNotificationHandler(notifications::kTaskCancelled,
        [&](const JsonRpcNotification&) {
            std::lock_guard<std::mutex> lock(notif_mutex);
            ++cancelled_notifications;
        });

    PerformLegacyHandshake(env.client);

    CallToolRequestParams params;
    params.name = "cancellable_tool";
    auto future = env.client->SendRequest(methods::kCallTool,
        SerializeCallToolRequestParams(params), {},
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));
    auto created = DeserializeCreateTaskResult(resp);

    ASSERT_EQ(started_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);

    CancelTaskRequestParams cancel_params;
    cancel_params.task_id = created.task_id;
    cancel_params.reason = "cooperative cancel";
    auto cancel_future = env.client->SendRequest(methods::kCancelTask,
        SerializeCancelTaskRequestParams(cancel_params), {},
        std::chrono::milliseconds(2000));
    ASSERT_EQ(cancel_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    auto cancel_resp = cancel_future.get();
    ASSERT_FALSE(cancel_resp.Contains("code"));

    for (int i = 0; i < 100 && !cancelled_early->load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(cancelled_early->load());

    auto cancelled = WaitForTaskStatus(env.store, created.task_id,
        TaskStatus::Cancelled);
    ASSERT_TRUE(cancelled.has_value());
    EXPECT_EQ(cancelled->status, TaskStatus::Cancelled);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        std::lock_guard<std::mutex> lock(notif_mutex);
        EXPECT_EQ(cancelled_notifications, 1);
    }

    CloseTaskEraEnv(env);
}

TEST(McpServerTest, CallToolTaskModeKeepsCompletedWhenCancelArrivesLate) {
    auto env = MakeTaskEraEnv();

    env.server->RegisterTool("quick_tool", MakeTaskToolOptions(),
        [](const Ctx&) {
            CallToolResult r;
            r.content.push_back(TextContent{"text", "done"});
            return r;
        });

    std::mutex notif_mutex;
    int cancelled_notifications = 0;
    env.client->SetNotificationHandler(notifications::kTaskCancelled,
        [&](const JsonRpcNotification&) {
            std::lock_guard<std::mutex> lock(notif_mutex);
            ++cancelled_notifications;
        });

    PerformLegacyHandshake(env.client);

    CallToolRequestParams params;
    params.name = "quick_tool";
    auto future = env.client->SendRequest(methods::kCallTool,
        SerializeCallToolRequestParams(params), {},
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));
    auto created = DeserializeCreateTaskResult(resp);

    auto done = WaitForTaskStatus(env.store, created.task_id, TaskStatus::Completed);
    ASSERT_TRUE(done.has_value());
    ASSERT_TRUE(done->result.has_value());

    CancelTaskRequestParams cancel_params;
    cancel_params.task_id = created.task_id;
    cancel_params.reason = "late cancel";
    auto cancel_future = env.client->SendRequest(methods::kCancelTask,
        SerializeCancelTaskRequestParams(cancel_params), {},
        std::chrono::milliseconds(2000));
    ASSERT_EQ(cancel_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    auto cancel_resp = cancel_future.get();
    ASSERT_FALSE(cancel_resp.Contains("code"));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto final_task = env.store->GetTask(created.task_id);
    ASSERT_TRUE(final_task.has_value());
    EXPECT_EQ(final_task->status, TaskStatus::Completed);
    ASSERT_TRUE(final_task->result.has_value());
    EXPECT_TRUE(final_task->result->Contains("content"));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        std::lock_guard<std::mutex> lock(notif_mutex);
        EXPECT_EQ(cancelled_notifications, 0);
    }

    CloseTaskEraEnv(env);
}

TEST(McpServerTest, ElicitRequestParamsFormModeOmitsModeOnWire) {
    ElicitRequestParams params;
    params.message = "pick one";
    JsonValue form = SerializeElicitRequestParams(params);
    EXPECT_FALSE(form.Contains("mode"));
    EXPECT_FALSE(form.Contains("url"));
    EXPECT_FALSE(form.Contains("elicitationId"));
    auto round_trip = DeserializeElicitRequestParams(form);
    EXPECT_EQ(round_trip.mode, "form");
    EXPECT_FALSE(round_trip.url.has_value());
    EXPECT_FALSE(round_trip.elicitation_id.has_value());

    ElicitRequestParams url_params;
    url_params.message = "authorize";
    url_params.mode = "url";
    url_params.url = "https://example.com/consent";
    url_params.elicitation_id = "elicit-1";
    JsonValue wire = SerializeElicitRequestParams(url_params);
    ASSERT_TRUE(wire.Contains("mode"));
    EXPECT_EQ(wire["mode"].GetString(), "url");
    ASSERT_TRUE(wire.Contains("url"));
    ASSERT_TRUE(wire.Contains("elicitationId"));
    auto url_round_trip = DeserializeElicitRequestParams(wire);
    EXPECT_EQ(url_round_trip.mode, "url");
    ASSERT_TRUE(url_round_trip.url.has_value());
    EXPECT_EQ(*url_round_trip.url, "https://example.com/consent");
    ASSERT_TRUE(url_round_trip.elicitation_id.has_value());
    EXPECT_EQ(*url_round_trip.elicitation_id, "elicit-1");
}

TEST(McpServerTest, ElicitUrlResolvesOnElicitationComplete) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLegacyProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), sopts);

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    ElicitRequestParams captured;
    std::promise<void> request_seen;
    auto request_seen_future = request_seen.get_future();
    client->SetRequestHandler(methods::kElicit,
        [&](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (req.params) captured = DeserializeElicitRequestParams(*req.params);
            JsonValue result(JsonValue::object_tag);
            result["action"] = JsonValue("accept");
            p.set_value(std::move(result));
            if (captured.elicitation_id) {
                JsonValue complete_params(JsonValue::object_tag);
                complete_params["elicitationId"] = JsonValue(*captured.elicitation_id);
                client->SendNotification(notifications::kElicitationComplete,
                    std::move(complete_params));
            }
            request_seen.set_value();
        });

    auto future = server->ElicitUrl("https://example.com/consent",
        "please authorize", std::chrono::seconds(5));

    ASSERT_EQ(request_seen_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    EXPECT_EQ(captured.mode, "url");
    EXPECT_EQ(captured.url, "https://example.com/consent");
    ASSERT_TRUE(captured.elicitation_id.has_value());
    EXPECT_FALSE(captured.elicitation_id->empty());

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    EXPECT_EQ(result.action, "accept");
    EXPECT_FALSE(result.content.has_value());

    server->Close();
    client->Close();
}

TEST(McpServerTest, ElicitUrlTimesOutWhenNoCompleteArrives) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLegacyProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), sopts);

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    client->SetRequestHandler(methods::kElicit,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue result(JsonValue::object_tag);
            result["action"] = JsonValue("accept");
            p.set_value(std::move(result));
        });

    auto future = server->ElicitUrl("https://example.com/consent",
        "please authorize", std::chrono::seconds(1));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    bool threw_timeout = false;
    try {
        (void)future.get();
    } catch (const McpError& e) {
        threw_timeout = e.Code() == McpErrorCode::RequestTimeout;
    }
    EXPECT_TRUE(threw_timeout);

    server->Close();
    client->Close();
}

TEST(McpServerTest, ElicitationMissingCapability32021) {
    {
        auto pair = InMemoryTransport::CreatePair();
        ServerOptions sopts;
        sopts.server_info = Implementation{"test-server", "1.0.0"};
        sopts.protocol_version = std::string(kLegacyProtocolVersion);
        auto server = McpServer::Create(std::move(pair.server), sopts);

        ElicitRequestParams params;
        params.message = "form question";
        try {
            (void)server->Elicit(params);
            FAIL();
        } catch (const McpError& e) {
            EXPECT_EQ(static_cast<int32_t>(e.Code()), -32021);
            ASSERT_TRUE(e.Data().has_value());
            const JsonValue* required = e.Data()->Find("requiredCapabilities");
            ASSERT_NE(required, nullptr);
            const JsonValue* elicitation = required->Find("elicitation");
            ASSERT_NE(elicitation, nullptr);
            EXPECT_TRUE(elicitation->Contains("form"));
            EXPECT_FALSE(elicitation->Contains("url"));
        }
        server->Close();
    }
    {
        auto pair = InMemoryTransport::CreatePair();
        ServerOptions sopts;
        sopts.server_info = Implementation{"test-server", "1.0.0"};
        sopts.protocol_version = std::string(kLegacyProtocolVersion);
        auto server = McpServer::Create(std::move(pair.server), sopts);

        ElicitRequestParams params;
        params.message = "url question";
        params.mode = "url";
        params.url = "https://example.com/consent";
        try {
            (void)server->Elicit(params);
            FAIL();
        } catch (const McpError& e) {
            EXPECT_EQ(static_cast<int32_t>(e.Code()), -32021);
            ASSERT_TRUE(e.Data().has_value());
            const JsonValue* required = e.Data()->Find("requiredCapabilities");
            ASSERT_NE(required, nullptr);
            const JsonValue* elicitation = required->Find("elicitation");
            ASSERT_NE(elicitation, nullptr);
            EXPECT_TRUE(elicitation->Contains("url"));
            EXPECT_FALSE(elicitation->Contains("form"));
        }
        server->Close();
    }
    {
        auto pair = InMemoryTransport::CreatePair();
        ServerOptions sopts;
        sopts.server_info = Implementation{"test-server", "1.0.0"};
        sopts.protocol_version = std::string(kLegacyProtocolVersion);
        auto server = McpServer::Create(std::move(pair.server), sopts);

        auto client = std::make_shared<McpSessionHandler>(
            std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
        client->Start();

        InitializeRequestParams iparams;
        iparams.protocol_version = std::string(kLegacyProtocolVersion);
        iparams.client_info = Implementation{"test-client", "1.0"};
        iparams.capabilities.roots = RootsCapability{};
        auto init_future = client->SendRequest(methods::kInitialize,
            SerializeInitializeRequestParams(iparams), {},
            std::chrono::milliseconds(2000));
        ASSERT_EQ(init_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
        (void)init_future.get();
        client->SendNotification(notifications::kInitialized);

        ElicitRequestParams params;
        params.message = "form question";
        try {
            (void)server->Elicit(params);
            FAIL();
        } catch (const McpError& e) {
            EXPECT_EQ(static_cast<int32_t>(e.Code()), -32021);
            ASSERT_TRUE(e.Data().has_value());
            const JsonValue* elicitation =
                e.Data()->Find("requiredCapabilities")->Find("elicitation");
            ASSERT_NE(elicitation, nullptr);
            EXPECT_TRUE(elicitation->Contains("form"));
        }
        server->Close();
        client->Close();
    }
    {
        auto pair = InMemoryTransport::CreatePair();
        ServerOptions sopts;
        sopts.server_info = Implementation{"test-server", "1.0.0"};
        sopts.protocol_version = std::string(kLegacyProtocolVersion);
        auto server = McpServer::Create(std::move(pair.server), sopts);

        auto client = std::make_shared<McpSessionHandler>(
            std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
        client->Start();
        client->SetRequestHandler(methods::kElicit,
            [](const JsonRpcRequest&, std::promise<JsonValue> p) {
                JsonValue result(JsonValue::object_tag);
                result["action"] = JsonValue("accept");
                p.set_value(std::move(result));
            });

        InitializeRequestParams iparams;
        iparams.protocol_version = std::string(kLegacyProtocolVersion);
        iparams.client_info = Implementation{"test-client", "1.0"};
        ElicitationCapability declared;
        declared.form = JsonValue(JsonValue::object_tag);
        iparams.capabilities.elicitation = declared;
        auto init_future = client->SendRequest(methods::kInitialize,
            SerializeInitializeRequestParams(iparams), {},
            std::chrono::milliseconds(2000));
        ASSERT_EQ(init_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
        (void)init_future.get();
        client->SendNotification(notifications::kInitialized);

        ElicitRequestParams params;
        params.message = "form question";
        auto future = server->Elicit(params);
        ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
        EXPECT_EQ(future.get().action, "accept");

        server->Close();
        client->Close();
    }
    {
        auto pair = InMemoryTransport::CreatePair();
        ServerOptions sopts;
        sopts.server_info = Implementation{"test-server", "1.0.0"};
        sopts.protocol_version = std::string(kLegacyProtocolVersion);
        auto server = McpServer::Create(std::move(pair.server), sopts);

        auto client = std::make_shared<McpSessionHandler>(
            std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
        client->Start();
        client->SetRequestHandler(methods::kElicit,
            [](const JsonRpcRequest&, std::promise<JsonValue> p) {
                JsonValue result(JsonValue::object_tag);
                result["action"] = JsonValue("accept");
                p.set_value(std::move(result));
            });

        InitializeRequestParams iparams;
        iparams.protocol_version = std::string(kLegacyProtocolVersion);
        iparams.client_info = Implementation{"test-client", "1.0"};
        iparams.capabilities.elicitation = ElicitationCapability{};
        auto init_future = client->SendRequest(methods::kInitialize,
            SerializeInitializeRequestParams(iparams), {},
            std::chrono::milliseconds(2000));
        ASSERT_EQ(init_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
        (void)init_future.get();
        client->SendNotification(notifications::kInitialized);

        ElicitRequestParams params;
        params.message = "form question";
        auto future = server->Elicit(params);
        ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
        EXPECT_EQ(future.get().action, "accept");

        server->Close();
        client->Close();
    }
}

TEST(McpServerTest, MrtrToolInputRequiredElicitationRoundTrip) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), sopts);

    server->RegisterTool(McpServerTool::Create("greeter",
        std::function<CallToolResult(const Ctx&)>(
            [](const Ctx& ctx) -> CallToolResult {
                if (ctx.Params().input_responses) {
                    auto* elicit = ctx.Params().input_responses->Find("name");
                    auto* content = elicit ? elicit->Find("content") : nullptr;
                    auto* name = content ? content->Find("user_name") : nullptr;
                    if (name && name->IsString()) {
                        CallToolResult done;
                        done.content.push_back(
                            TextContent{"text", "Hello, " + name->GetString() + "!"});
                        return done;
                    }
                }
                CallToolResult result;
                InputRequiredResult ir;
                ElicitRequestParams elicit_params;
                elicit_params.message = "What is your name?";
                JsonValue schema(JsonValue::object_tag);
                schema["type"] = JsonValue("object");
                JsonValue properties(JsonValue::object_tag);
                JsonValue field(JsonValue::object_tag);
                field["type"] = JsonValue("string");
                properties["user_name"] = std::move(field);
                schema["properties"] = std::move(properties);
                elicit_params.requested_schema = std::move(schema);
                ir.input_requests["name"] = MakeInputRequestForElicitation(elicit_params);
                result.input_required = std::move(ir);
                return result;
            }),
        ToolOptions{}.Description("MRTR elicitation fixture")));

    ClientOptions cops;
    cops.connect_mode = ConnectMode::Pin;
    cops.pin_protocol_version = std::string(kLatestProtocolVersion);
    cops.input_required_config = ClientOptions::InputRequiredConfig{};
    auto client = McpClient::Create(std::move(pair.client), cops);
    client->SetElicitationHandler([](const ElicitRequestParams&) {
        ElicitResult result;
        result.action = "accept";
        JsonValue content(JsonValue::object_tag);
        content["user_name"] = JsonValue("Alice");
        result.content = std::move(content);
        return result;
    });

    auto done = client->CallTool("greeter", JsonValue(JsonValue::object_tag));
    EXPECT_FALSE(done.is_error);
    ASSERT_EQ(done.content.size(), 1u);
    auto* text = std::get_if<TextContent>(&done.content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "Hello, Alice!");

    client->Close();
    server->Close();
}

TEST(McpServerTest, MrtrMintedRequestStateRoundTrip) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    sopts.request_state_key = std::string("unit-test-key");
    auto server = McpServer::Create(std::move(pair.server), sopts);

    server->RegisterTool(McpServerTool::Create("counter",
        std::function<CallToolResult(const Ctx&)>(
            [](const Ctx& ctx) -> CallToolResult {
                int round = 0;
                if (ctx.Params().request_state) {
                    auto payload =
                        detail::DecodeRequestStatePayload(*ctx.Params().request_state);
                    if (payload) {
                        auto* stored = payload->Find("round");
                        if (stored && stored->IsInt())
                            round = static_cast<int>(stored->GetInt());
                    }
                }
                if (round >= 2) {
                    CallToolResult done;
                    done.content.push_back(TextContent{"text", "round-2-done"});
                    return done;
                }
                CallToolResult result;
                InputRequiredResult ir;
                ElicitRequestParams elicit_params;
                elicit_params.message = "ack round";
                ir.input_requests["ack"] = MakeInputRequestForElicitation(elicit_params);
                JsonValue payload(JsonValue::object_tag);
                payload["round"] = JsonValue(static_cast<int64_t>(round + 1));
                ir.request_state = payload.Dump();
                result.input_required = std::move(ir);
                return result;
            }),
        ToolOptions{}.Description("MRTR requestState fixture")));

    ClientOptions cops;
    cops.connect_mode = ConnectMode::Pin;
    cops.pin_protocol_version = std::string(kLatestProtocolVersion);
    cops.input_required_config = ClientOptions::InputRequiredConfig{};
    auto client = McpClient::Create(std::move(pair.client), cops);
    client->SetElicitationHandler([](const ElicitRequestParams&) {
        ElicitResult result;
        result.action = "accept";
        JsonValue content(JsonValue::object_tag);
        content["ack"] = JsonValue("ok");
        result.content = std::move(content);
        return result;
    });

    auto done = client->CallTool("counter", JsonValue(JsonValue::object_tag));
    EXPECT_FALSE(done.is_error);
    ASSERT_EQ(done.content.size(), 1u);
    auto* text = std::get_if<TextContent>(&done.content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "round-2-done");

    client->Close();
    server->Close();
}

TEST(McpServerTest, MrtrForgedRequestStateRejectedWithReason) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    sopts.request_state_key = std::string("unit-test-key");
    auto server = McpServer::Create(std::move(pair.server), sopts);

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();

    JsonValue params(JsonValue::object_tag);
    params["name"] = JsonValue("any-tool");
    params["requestState"] = JsonValue("forged-state");
    auto future = client->SendRequest(methods::kCallTool, std::move(params), {},
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.Contains("code"));
    EXPECT_EQ(result["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::InvalidParams));
    auto* data = result.Find("data");
    ASSERT_NE(data, nullptr);
    auto* reason = data->Find("reason");
    ASSERT_NE(reason, nullptr);
    EXPECT_EQ(reason->GetString(), std::string("invalid_request_state"));

    server->Close();
    client->Close();
}

TEST(McpServerTest, PromptsGetInputRequired) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), sopts);

    auto calls = std::make_shared<std::atomic<int>>(0);
    server->RegisterPrompt("greeter",
        PromptOptions{}.Title("MRTR prompt fixture").Description("MRTR elicitation fixture"),
        [calls](const std::string&, const std::optional<JsonValue>&) -> GetPromptResult {
            if (calls->fetch_add(1) > 0) {
                GetPromptResult done;
                PromptMessage message;
                message.role = "user";
                message.content = TextContent{"text", "Hello, Alice!"};
                done.messages.push_back(std::move(message));
                return done;
            }
            GetPromptResult result;
            InputRequiredResult ir;
            ElicitRequestParams elicit_params;
            elicit_params.message = "What is your name?";
            JsonValue schema(JsonValue::object_tag);
            schema["type"] = JsonValue("object");
            JsonValue properties(JsonValue::object_tag);
            JsonValue field(JsonValue::object_tag);
            field["type"] = JsonValue("string");
            properties["user_name"] = std::move(field);
            schema["properties"] = std::move(properties);
            elicit_params.requested_schema = std::move(schema);
            ir.input_requests["name"] = MakeInputRequestForElicitation(elicit_params);
            result.input_required = std::move(ir);
            return result;
        });

    ClientOptions cops;
    cops.connect_mode = ConnectMode::Pin;
    cops.pin_protocol_version = std::string(kLatestProtocolVersion);
    cops.input_required_config = ClientOptions::InputRequiredConfig{};
    auto client = McpClient::Create(std::move(pair.client), cops);
    client->SetElicitationHandler([](const ElicitRequestParams&) {
        ElicitResult result;
        result.action = "accept";
        JsonValue content(JsonValue::object_tag);
        content["user_name"] = JsonValue("Alice");
        result.content = std::move(content);
        return result;
    });

    auto done = client->GetPrompt("greeter", JsonValue(JsonValue::object_tag));
    ASSERT_EQ(done.messages.size(), 1u);
    auto* text = std::get_if<TextContent>(&done.messages[0].content);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "Hello, Alice!");
    EXPECT_EQ(calls->load(), 2);

    client->Close();
    server->Close();
}

TEST(McpServerTest, ResourceReadInputRequired) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    sopts.request_state_key = std::string("unit-test-key");
    auto server = McpServer::Create(std::move(pair.server), sopts);

    auto calls = std::make_shared<std::atomic<int>>(0);
    server->RegisterResource("greeting", "test://greeting",
        ResourceOptions{}.Title("MRTR resource fixture").Description("MRTR elicitation fixture").MimeType("text/plain"),
        [calls](const std::string& uri) -> ReadResourceResult {
            if (calls->fetch_add(1) > 0) {
                TextResourceContents contents;
                contents.uri = uri;
                contents.mime_type = "text/plain";
                contents.text = "Hello, Alice!";
                ReadResourceResult done;
                done.contents.push_back(ResourceContents{std::move(contents)});
                return done;
            }
            ReadResourceResult result;
            InputRequiredResult ir;
            ElicitRequestParams elicit_params;
            elicit_params.message = "What is your name?";
            JsonValue schema(JsonValue::object_tag);
            schema["type"] = JsonValue("object");
            JsonValue properties(JsonValue::object_tag);
            JsonValue field(JsonValue::object_tag);
            field["type"] = JsonValue("string");
            properties["user_name"] = std::move(field);
            schema["properties"] = std::move(properties);
            elicit_params.requested_schema = std::move(schema);
            ir.input_requests["name"] = MakeInputRequestForElicitation(elicit_params);
            result.input_required = std::move(ir);
            return result;
        });

    ClientOptions cops;
    cops.connect_mode = ConnectMode::Pin;
    cops.pin_protocol_version = std::string(kLatestProtocolVersion);
    cops.input_required_config = ClientOptions::InputRequiredConfig{};
    auto client = McpClient::Create(std::move(pair.client), cops);
    client->SetElicitationHandler([](const ElicitRequestParams&) {
        ElicitResult result;
        result.action = "accept";
        JsonValue content(JsonValue::object_tag);
        content["user_name"] = JsonValue("Alice");
        result.content = std::move(content);
        return result;
    });

    auto done = client->ReadResource("test://greeting");
    ASSERT_EQ(done.contents.size(), 1u);
    auto* text = std::get_if<TextResourceContents>(&done.contents[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "Hello, Alice!");
    EXPECT_EQ(calls->load(), 2);

    client->Close();
    server->Close();
}

TEST(McpServerTest, SendResourceUpdatedReachesSubscribedUri) {
    auto ctx = MakeModernServerWithClient();
    NegotiateModernVersion(ctx.server);

    std::promise<JsonRpcNotification> ack_received;
    auto ack_future = ack_received.get_future();
    ctx.client->SetNotificationHandler(notifications::kSubscriptionsAcknowledged,
        [&ack_received](const JsonRpcNotification& n) {
            ack_received.set_value(n);
        });

    std::promise<JsonRpcNotification> updated_received;
    auto updated_future = updated_received.get_future();
    ctx.client->SetNotificationHandler(notifications::kResourceUpdated,
        [&updated_received](const JsonRpcNotification& n) {
            updated_received.set_value(n);
        });

    SubscriptionsListenRequestParams params;
    params.notifications.resource_subscriptions = {"resource://send/1"};
    auto subscribe = ctx.client->SendRequest(methods::kSubscribe,
        SerializeSubscriptionsListenRequestParams(params),
        MetaWithSubscriptionId("client-sub-upd"),
        std::chrono::milliseconds(2000));

    ASSERT_EQ(ack_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    ASSERT_EQ(subscribe.wait_for(std::chrono::seconds(3)), std::future_status::ready);

    ctx.server->SendResourceUpdated("resource://send/1");

    ASSERT_EQ(updated_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto notif = updated_future.get();
    EXPECT_EQ(notif.method, std::string(notifications::kResourceUpdated));
    ASSERT_TRUE(notif.params.has_value());
    auto* uri = notif.params->Find("uri");
    ASSERT_NE(uri, nullptr);
    EXPECT_EQ(uri->GetString(), "resource://send/1");

    ctx.server->Close();
    ctx.client->Close();
}

TEST(McpServerTest, SendResourceUpdatedSkipsUnsubscribedUri) {
    auto ctx = MakeModernServerWithClient();
    NegotiateModernVersion(ctx.server);

    std::promise<JsonRpcNotification> ack_received;
    auto ack_future = ack_received.get_future();
    ctx.client->SetNotificationHandler(notifications::kSubscriptionsAcknowledged,
        [&ack_received](const JsonRpcNotification& n) {
            ack_received.set_value(n);
        });

    std::promise<JsonRpcNotification> updated_received;
    auto updated_future = updated_received.get_future();
    ctx.client->SetNotificationHandler(notifications::kResourceUpdated,
        [&updated_received](const JsonRpcNotification& n) {
            updated_received.set_value(n);
        });

    SubscriptionsListenRequestParams params;
    params.notifications.resource_subscriptions = {"resource://send/1"};
    auto subscribe = ctx.client->SendRequest(methods::kSubscribe,
        SerializeSubscriptionsListenRequestParams(params),
        MetaWithSubscriptionId("client-sub-other"),
        std::chrono::milliseconds(2000));

    ASSERT_EQ(ack_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    ASSERT_EQ(subscribe.wait_for(std::chrono::seconds(3)), std::future_status::ready);

    ctx.server->SendResourceUpdated("resource://other/9");

    EXPECT_EQ(updated_future.wait_for(std::chrono::milliseconds(300)),
              std::future_status::timeout);

    ctx.server->Close();
    ctx.client->Close();
}

namespace {

struct ModernTaskEnv {
    std::unique_ptr<McpServer> server;
    std::shared_ptr<McpSessionHandler> client;
    std::shared_ptr<FileTaskStore> store;
    std::filesystem::path store_path;
};

ModernTaskEnv MakeModernTaskEnv() {
    ModernTaskEnv env;
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    env.store_path = MakeTaskStorePath();
    std::error_code ec;
    std::filesystem::remove(env.store_path, ec);
    env.store = std::make_shared<FileTaskStore>(env.store_path);
    sopts.task_store = env.store;
    sopts.enable_tasks_extension = true;
    env.server = McpServer::Create(std::move(pair.server), sopts);
    env.client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    env.client->Start();
    InitializeRequestParams iparams;
    iparams.protocol_version = std::string(kLatestProtocolVersion);
    iparams.client_info = Implementation{"test-client", "1.0"};
    ClientCapabilities declared_caps;
    std::map<std::string, JsonValue> declared_ext;
    declared_ext[std::string(kTasksExtensionId)] = JsonValue(JsonValue::object_tag);
    declared_caps.extensions = std::move(declared_ext);
    iparams.capabilities = std::move(declared_caps);
    auto init_future = env.client->SendRequest(methods::kInitialize,
        SerializeInitializeRequestParams(iparams), {},
        std::chrono::milliseconds(2000));
    if (init_future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        ADD_FAILURE();
        return env;
    }
    auto init_resp = init_future.get();
    if (!init_resp.Contains("code")) {
        ADD_FAILURE();
        return env;
    }
    EXPECT_EQ(init_resp["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::MethodNotFound));
    return env;
}

ModernTaskEnv MakeBareModernTaskEnv() {
    ModernTaskEnv env;
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    env.store_path = MakeTaskStorePath();
    std::error_code ec;
    std::filesystem::remove(env.store_path, ec);
    env.store = std::make_shared<FileTaskStore>(env.store_path);
    sopts.task_store = env.store;
    sopts.enable_tasks_extension = true;
    env.server = McpServer::Create(std::move(pair.server), sopts);
    env.client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    env.client->Start();
    return env;
}

void CloseModernTaskEnv(ModernTaskEnv& env) {
    env.server->Close();
    env.client->Close();
    std::error_code ec;
    std::filesystem::remove(env.store_path, ec);
}

RequestMeta MetaWithMcpName(std::string_view name) {
    RequestMeta meta;
    JsonValue ext(JsonValue::object_tag);
    ext["Mcp-Name"] = JsonValue(std::string(name));
    meta.extensions = std::move(ext);
    ClientCapabilities caps;
    std::map<std::string, JsonValue> task_ext;
    task_ext[std::string(kTasksExtensionId)] = JsonValue(JsonValue::object_tag);
    caps.extensions = std::move(task_ext);
    meta.client_capabilities = std::move(caps);
    return meta;
}

RequestMeta MetaWithTasksExtension() {
    RequestMeta meta;
    ClientCapabilities caps;
    std::map<std::string, JsonValue> ext;
    ext[std::string(kTasksExtensionId)] = JsonValue(JsonValue::object_tag);
    caps.extensions = std::move(ext);
    meta.client_capabilities = std::move(caps);
    return meta;
}

RequestMeta MetaWithSampling() {
    RequestMeta meta;
    ClientCapabilities caps;
    caps.sampling = SamplingCapability{};
    meta.client_capabilities = std::move(caps);
    return meta;
}

ToolOptions MakeRequiredTaskToolOptions() {
    ToolOptions opts = MakeTaskToolOptions();
    opts.execution->task_support = TaskSupport::Required;
    return opts;
}

JsonValue SendBareRequest(
    const std::shared_ptr<McpSessionHandler>& client,
    std::string_view method,
    JsonValue params,
    RequestMeta meta = {})
{
    auto future = client->SendRequest(method, std::move(params), std::move(meta),
        std::chrono::milliseconds(2000));
    if (future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        ADD_FAILURE();
        JsonValue err(JsonValue::object_tag);
        err["code"] = JsonValue(static_cast<int64_t>(McpErrorCode::InternalError));
        return err;
    }
    return future.get();
}

const JsonValue* RequiredTasksExtensionEntry(const JsonValue& resp) {
    auto* data = resp.Find("data");
    if (!data) return nullptr;
    auto* required = data->Find("requiredCapabilities");
    if (!required) return nullptr;
    auto* extensions = required->Find("extensions");
    if (!extensions) return nullptr;
    return extensions->Find(std::string(kTasksExtensionId));
}

} // namespace

TEST(McpServerTest, UnsolicitedTaskCreation) {
    auto env = MakeModernTaskEnv();

    env.server->RegisterTool("quick_task", MakeTaskToolOptions(),
        [](const Ctx&) {
            CallToolResult r;
            r.content.push_back(TextContent{"text", "modern-done"});
            return r;
        });

    auto caps = env.server->GetCapabilities();
    ASSERT_TRUE(caps.extensions.has_value());
    EXPECT_TRUE(caps.extensions->find(std::string(kTasksExtensionId))
                != caps.extensions->end());

    DiscoverRequestParams discover_params;
    auto discover_future = env.client->SendRequest(methods::kDiscover,
        SerializeDiscoverRequestParams(discover_params), {},
        std::chrono::milliseconds(2000));
    ASSERT_EQ(discover_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    auto discover_resp = discover_future.get();
    ASSERT_FALSE(discover_resp.Contains("code"));
    auto discovered = DeserializeDiscoverResult(discover_resp);
    ASSERT_TRUE(discovered.capabilities.extensions.has_value());
    EXPECT_TRUE(discovered.capabilities.extensions->find(std::string(kTasksExtensionId))
                != discovered.capabilities.extensions->end());

    std::atomic<bool> legacy_notif_seen{false};
    const std::string_view kLegacyTaskNotifs[] = {
        notifications::kTaskStatus, notifications::kTaskWorking,
        notifications::kTaskCompleted, notifications::kTaskFailed,
        notifications::kTaskCancelled, notifications::kTaskInputRequired,
    };
    for (auto method : kLegacyTaskNotifs) {
        env.client->SetNotificationHandler(method,
            [&](const JsonRpcNotification&) { legacy_notif_seen.store(true); });
    }

    CallToolRequestParams params;
    params.name = "quick_task";
    auto future = env.client->SendRequest(methods::kCallTool,
        SerializeCallToolRequestParams(params), MetaWithTasksExtension(),
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));
    ASSERT_TRUE(resp.Contains("resultType"));
    EXPECT_EQ(resp["resultType"].GetString(), "task");
    auto created = DeserializeCreateTaskResult(resp);
    EXPECT_EQ(created.status, "working");
    EXPECT_FALSE(created.task_id.empty());

    GetTaskRequestParams get_params;
    get_params.task_id = created.task_id;
    std::optional<GetTaskResult> final_task;
    for (int i = 0; i < 200; ++i) {
        auto get_future = env.client->SendRequest(methods::kGetTask,
            SerializeGetTaskRequestParams(get_params), MetaWithTasksExtension(),
            std::chrono::milliseconds(2000));
        ASSERT_EQ(get_future.wait_for(std::chrono::seconds(3)),
                  std::future_status::ready);
        auto get_resp = get_future.get();
        ASSERT_FALSE(get_resp.Contains("code"));
        auto got = DeserializeGetTaskResult(get_resp);
        if (got.status == "completed") {
            final_task = got;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ASSERT_TRUE(final_task.has_value());
    EXPECT_EQ(final_task->task_id, created.task_id);
    ASSERT_TRUE(final_task->result.has_value());
    EXPECT_TRUE(final_task->result->Contains("content"));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(legacy_notif_seen.load());

    GetTaskRequestParams payload_params;
    payload_params.task_id = created.task_id;
    auto payload_future = env.client->SendRequest(methods::kGetTaskPayload,
        SerializeGetTaskRequestParams(payload_params), {},
        std::chrono::milliseconds(2000));
    ASSERT_EQ(payload_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    auto payload_resp = payload_future.get();
    ASSERT_TRUE(payload_resp.Contains("code"));
    EXPECT_EQ(payload_resp["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::MethodNotFound));

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, CreateTaskResultFlatNoNested) {
    auto env = MakeModernTaskEnv();

    env.server->RegisterTool("flat_task", MakeTaskToolOptions(),
        [](const Ctx&) {
            CallToolResult r;
            r.content.push_back(TextContent{"text", "flat-done"});
            return r;
        });

    CallToolRequestParams params;
    params.name = "flat_task";
    auto future = env.client->SendRequest(methods::kCallTool,
        SerializeCallToolRequestParams(params), MetaWithTasksExtension(),
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));
    ASSERT_TRUE(resp.Contains("resultType"));
    EXPECT_EQ(resp["resultType"].GetString(), "task");
    EXPECT_FALSE(resp.Contains("task"));
    ASSERT_TRUE(resp.Contains("taskId"));
    EXPECT_FALSE(resp["taskId"].GetString().empty());
    ASSERT_TRUE(resp.Contains("status"));
    EXPECT_EQ(resp["status"].GetString(), "working");
    ASSERT_TRUE(resp.Contains("createdAt"));
    EXPECT_FALSE(resp["createdAt"].GetString().empty());
    ASSERT_TRUE(resp.Contains("lastUpdatedAt"));
    EXPECT_FALSE(resp["lastUpdatedAt"].GetString().empty());
    EXPECT_FALSE(resp.Contains("result"));
    EXPECT_FALSE(resp.Contains("error"));
    EXPECT_FALSE(resp.Contains("inputRequests"));

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, TaskTimestampsPopulated) {
    auto env = MakeModernTaskEnv();

    env.server->RegisterTool("timed_task", MakeTaskToolOptions(),
        [](const Ctx&) {
            CallToolResult r;
            r.content.push_back(TextContent{"text", "timed-done"});
            return r;
        });

    CallToolRequestParams params;
    params.name = "timed_task";
    auto future = env.client->SendRequest(methods::kCallTool,
        SerializeCallToolRequestParams(params), MetaWithTasksExtension(),
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto created = DeserializeCreateTaskResult(future.get());
    EXPECT_FALSE(created.created_at.empty());
    ASSERT_TRUE(created.last_updated_at.has_value());
    EXPECT_FALSE(created.last_updated_at->empty());

    GetTaskRequestParams get_params;
    get_params.task_id = created.task_id;
    std::optional<GetTaskResult> final_task;
    for (int i = 0; i < 200; ++i) {
        auto get_future = env.client->SendRequest(methods::kGetTask,
            SerializeGetTaskRequestParams(get_params), MetaWithTasksExtension(),
            std::chrono::milliseconds(2000));
        ASSERT_EQ(get_future.wait_for(std::chrono::seconds(3)),
                  std::future_status::ready);
        auto got = DeserializeGetTaskResult(get_future.get());
        if (got.status == "completed") {
            final_task = got;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ASSERT_TRUE(final_task.has_value());
    ASSERT_TRUE(final_task->created_at.has_value());
    EXPECT_FALSE(final_task->created_at->empty());
    ASSERT_TRUE(final_task->last_updated_at.has_value());
    EXPECT_FALSE(final_task->last_updated_at->empty());

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, ToolErrorMapsToCompleted) {
    auto env = MakeModernTaskEnv();

    env.server->RegisterTool("err_tool", MakeTaskToolOptions(),
        [](const Ctx&) {
            CallToolResult r;
            r.is_error = true;
            r.content.push_back(TextContent{"text", "tool blew up"});
            return r;
        });

    CallToolRequestParams params;
    params.name = "err_tool";
    auto future = env.client->SendRequest(methods::kCallTool,
        SerializeCallToolRequestParams(params), MetaWithTasksExtension(),
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto created = DeserializeCreateTaskResult(future.get());
    ASSERT_FALSE(created.task_id.empty());

    GetTaskRequestParams get_params;
    get_params.task_id = created.task_id;
    std::optional<GetTaskResult> final_task;
    for (int i = 0; i < 200; ++i) {
        auto get_future = env.client->SendRequest(methods::kGetTask,
            SerializeGetTaskRequestParams(get_params), MetaWithTasksExtension(),
            std::chrono::milliseconds(2000));
        ASSERT_EQ(get_future.wait_for(std::chrono::seconds(3)),
                  std::future_status::ready);
        auto got = DeserializeGetTaskResult(get_future.get());
        if (got.status == "completed" || got.status == "failed"
                || got.status == "cancelled") {
            final_task = got;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ASSERT_TRUE(final_task.has_value());
    EXPECT_EQ(final_task->status, "completed");
    ASSERT_TRUE(final_task->result.has_value());
    auto tool_result = DeserializeCallToolResult(*final_task->result);
    EXPECT_TRUE(tool_result.is_error);
    ASSERT_FALSE(tool_result.content.empty());
    auto* text = std::get_if<TextContent>(&tool_result.content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "tool blew up");

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, ProtocolErrorMapsToFailedWithError) {
    auto env = MakeModernTaskEnv();

    env.server->RegisterTool("boom_task", MakeTaskToolOptions(),
        [](const Ctx&) -> CallToolResult {
            throw McpError(McpErrorCode::InternalError, "kaboom");
        });

    CallToolRequestParams params;
    params.name = "boom_task";
    auto future = env.client->SendRequest(methods::kCallTool,
        SerializeCallToolRequestParams(params), MetaWithTasksExtension(),
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto created = DeserializeCreateTaskResult(future.get());
    ASSERT_FALSE(created.task_id.empty());

    GetTaskRequestParams get_params;
    get_params.task_id = created.task_id;
    auto failed = WaitForTaskStatus(env.store, created.task_id, TaskStatus::Failed);
    ASSERT_TRUE(failed.has_value());
    EXPECT_EQ(failed->status, TaskStatus::Failed);

    auto get_future = env.client->SendRequest(methods::kGetTask,
        SerializeGetTaskRequestParams(get_params), MetaWithTasksExtension(),
        std::chrono::milliseconds(2000));
    ASSERT_EQ(get_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    auto final_task = DeserializeGetTaskResult(get_future.get());
    EXPECT_EQ(final_task.status, "failed");
    ASSERT_TRUE(final_task.error.has_value());
    auto* code = final_task.error->Find("code");
    ASSERT_NE(code, nullptr);
    EXPECT_TRUE(code->IsInt());
    auto* message = final_task.error->Find("message");
    ASSERT_NE(message, nullptr);
    EXPECT_TRUE(message->IsString());
    EXPECT_FALSE(final_task.result.has_value());

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, TasksUpdateWithInputResponses) {
    auto env = MakeModernTaskEnv();

    env.store->CreateTask("task-input");
    ASSERT_TRUE(env.store->SetTaskStatus("task-input", TaskStatus::InputRequired));

    UpdateTaskRequestParams up;
    up.task_id = "task-input";
    JsonValue responses(JsonValue::object_tag);
    JsonValue name_resp(JsonValue::object_tag);
    name_resp["user_name"] = JsonValue("Alice");
    responses["name"] = std::move(name_resp);
    up.input_responses = std::move(responses);
    auto up_future = env.client->SendRequest(methods::kUpdateTask,
        SerializeUpdateTaskRequestParams(up), MetaWithTasksExtension(),
        std::chrono::milliseconds(2000));
    ASSERT_EQ(up_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto up_resp = up_future.get();
    ASSERT_FALSE(up_resp.Contains("code"));

    auto advanced = env.store->GetTask("task-input");
    ASSERT_TRUE(advanced.has_value());
    EXPECT_EQ(advanced->status, TaskStatus::Working);
    ASSERT_TRUE(advanced->input_responses.has_value());
    auto* name_entry = advanced->input_responses->Find("name");
    ASSERT_NE(name_entry, nullptr);
    auto* user_name = name_entry->Find("user_name");
    ASSERT_NE(user_name, nullptr);
    EXPECT_EQ(user_name->GetString(), "Alice");

    GetTaskRequestParams get_params;
    get_params.task_id = "task-input";
    auto get_future = env.client->SendRequest(methods::kGetTask,
        SerializeGetTaskRequestParams(get_params), MetaWithMcpName("task-input"),
        std::chrono::milliseconds(2000));
    ASSERT_EQ(get_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto get_resp = get_future.get();
    ASSERT_FALSE(get_resp.Contains("code"));
    EXPECT_EQ(DeserializeGetTaskResult(get_resp).status, "working");

    auto mismatch_future = env.client->SendRequest(methods::kGetTask,
        SerializeGetTaskRequestParams(get_params), MetaWithMcpName("other-task"),
        std::chrono::milliseconds(2000));
    ASSERT_EQ(mismatch_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    auto mismatch_resp = mismatch_future.get();
    ASSERT_TRUE(mismatch_resp.Contains("code"));
    EXPECT_EQ(mismatch_resp["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::HeaderMismatch));

    UpdateTaskRequestParams repeat;
    repeat.task_id = "task-input";
    JsonValue again(JsonValue::object_tag);
    again["name"] = JsonValue("Bob");
    repeat.input_responses = std::move(again);
    auto repeat_future = env.client->SendRequest(methods::kUpdateTask,
        SerializeUpdateTaskRequestParams(repeat), MetaWithTasksExtension(),
        std::chrono::milliseconds(2000));
    ASSERT_EQ(repeat_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    auto repeat_resp = repeat_future.get();
    ASSERT_TRUE(repeat_resp.Contains("code"));
    EXPECT_EQ(repeat_resp["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::InvalidParams));

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, TasksMethodsGateWithoutExtension) {
    auto env = MakeBareModernTaskEnv();

    GetTaskRequestParams get_params;
    get_params.task_id = "gate-test";
    auto get_resp = SendBareRequest(env.client, methods::kGetTask,
        SerializeGetTaskRequestParams(get_params));
    ASSERT_TRUE(get_resp.Contains("code"));
    EXPECT_EQ(get_resp["code"].GetInt(), int64_t(-32021));
    ASSERT_NE(RequiredTasksExtensionEntry(get_resp), nullptr);

    UpdateTaskRequestParams up_params;
    up_params.task_id = "gate-test";
    up_params.input_responses = JsonValue(JsonValue::object_tag);
    auto up_resp = SendBareRequest(env.client, methods::kUpdateTask,
        SerializeUpdateTaskRequestParams(up_params));
    ASSERT_TRUE(up_resp.Contains("code"));
    EXPECT_EQ(up_resp["code"].GetInt(), int64_t(-32021));
    ASSERT_NE(RequiredTasksExtensionEntry(up_resp), nullptr);

    CancelTaskRequestParams cancel_params;
    cancel_params.task_id = "gate-test";
    auto cancel_resp = SendBareRequest(env.client, methods::kCancelTask,
        SerializeCancelTaskRequestParams(cancel_params));
    ASSERT_TRUE(cancel_resp.Contains("code"));
    EXPECT_EQ(cancel_resp["code"].GetInt(), int64_t(-32021));
    ASSERT_NE(RequiredTasksExtensionEntry(cancel_resp), nullptr);

    auto declared_get = SendBareRequest(env.client, methods::kGetTask,
        SerializeGetTaskRequestParams(get_params), MetaWithTasksExtension());
    ASSERT_TRUE(declared_get.Contains("code"));
    EXPECT_EQ(declared_get["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::InvalidParams));

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, OptionalTaskToolWithoutExtensionFallsThroughToSync) {
    auto env = MakeBareModernTaskEnv();

    env.server->RegisterTool("sync_fallthrough", MakeTaskToolOptions(),
        [](const Ctx&) {
            CallToolResult r;
            r.content.push_back(TextContent{"text", "sync-done"});
            return r;
        });

    CallToolRequestParams params;
    params.name = "sync_fallthrough";
    auto resp = SendBareRequest(env.client, methods::kCallTool,
        SerializeCallToolRequestParams(params));
    ASSERT_FALSE(resp.Contains("code"));
    EXPECT_EQ(resp["resultType"].GetString(), "complete");
    EXPECT_FALSE(resp.Contains("taskId"));
    EXPECT_FALSE(resp.Contains("task"));
    ASSERT_TRUE(resp.Contains("content"));
    ASSERT_TRUE(resp["content"].IsArray());
    EXPECT_TRUE(resp["content"].Size() >= 1u);

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, RequiredTaskToolWithoutExtensionRejects32021) {
    auto env = MakeBareModernTaskEnv();

    env.server->RegisterTool("required_job", MakeRequiredTaskToolOptions(),
        [](const Ctx&) {
            CallToolResult r;
            r.content.push_back(TextContent{"text", "should-not-run"});
            return r;
        });

    CallToolRequestParams params;
    params.name = "required_job";
    auto resp = SendBareRequest(env.client, methods::kCallTool,
        SerializeCallToolRequestParams(params));
    ASSERT_TRUE(resp.Contains("code"));
    EXPECT_EQ(resp["code"].GetInt(), int64_t(-32021));
    ASSERT_NE(RequiredTasksExtensionEntry(resp), nullptr);

    auto declared = SendBareRequest(env.client, methods::kCallTool,
        SerializeCallToolRequestParams(params), MetaWithTasksExtension());
    ASSERT_FALSE(declared.Contains("code"));
    EXPECT_EQ(declared["resultType"].GetString(), "task");
    ASSERT_TRUE(declared.Contains("taskId"));
    EXPECT_FALSE(declared["taskId"].GetString().empty());

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, PerRequestTasksOptInCreatesTaskWithTtl) {
    auto env = MakeBareModernTaskEnv();

    env.server->RegisterTool("opt_task", MakeTaskToolOptions(),
        [](const Ctx&) {
            CallToolResult r;
            r.content.push_back(TextContent{"text", "opt-done"});
            return r;
        });

    CallToolRequestParams params;
    params.name = "opt_task";
    auto resp = SendBareRequest(env.client, methods::kCallTool,
        SerializeCallToolRequestParams(params), MetaWithTasksExtension());
    ASSERT_FALSE(resp.Contains("code"));
    EXPECT_EQ(resp["resultType"].GetString(), "task");
    ASSERT_TRUE(resp.Contains("taskId"));
    EXPECT_FALSE(resp["taskId"].GetString().empty());
    ASSERT_TRUE(resp.Contains("ttlMs"));
    ASSERT_TRUE(resp["ttlMs"].IsInt());
    EXPECT_TRUE(resp["ttlMs"].GetInt() > int64_t(0));
    EXPECT_FALSE(resp.Contains("ttl"));
    EXPECT_FALSE(resp.Contains("task"));

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, TasksUpdateAckCarriesCompleteResultType) {
    auto env = MakeBareModernTaskEnv();

    env.store->CreateTask("ack-task");
    ASSERT_TRUE(env.store->SetTaskStatus("ack-task", TaskStatus::InputRequired));

    UpdateTaskRequestParams up;
    up.task_id = "ack-task";
    JsonValue responses(JsonValue::object_tag);
    responses["name"] = JsonValue("Alice");
    up.input_responses = std::move(responses);
    auto up_resp = SendBareRequest(env.client, methods::kUpdateTask,
        SerializeUpdateTaskRequestParams(up), MetaWithTasksExtension());
    ASSERT_FALSE(up_resp.Contains("code"));
    EXPECT_EQ(up_resp["resultType"].GetString(), "complete");
    EXPECT_FALSE(up_resp.Contains("taskId"));
    EXPECT_FALSE(up_resp.Contains("status"));
    EXPECT_FALSE(up_resp.Contains("result"));
    EXPECT_FALSE(up_resp.Contains("error"));
    EXPECT_FALSE(up_resp.Contains("inputRequests"));

    UpdateTaskRequestParams done;
    done.task_id = "ack-task";
    JsonValue payload(JsonValue::object_tag);
    payload["ok"] = JsonValue(true);
    done.result = std::move(payload);
    auto done_resp = SendBareRequest(env.client, methods::kUpdateTask,
        SerializeUpdateTaskRequestParams(done), MetaWithTasksExtension());
    ASSERT_FALSE(done_resp.Contains("code"));
    EXPECT_EQ(done_resp["resultType"].GetString(), "complete");

    CancelTaskRequestParams cancel;
    cancel.task_id = "ack-task";
    auto cancel_resp = SendBareRequest(env.client, methods::kCancelTask,
        SerializeCancelTaskRequestParams(cancel), MetaWithTasksExtension());
    ASSERT_FALSE(cancel_resp.Contains("code"));
    EXPECT_EQ(cancel_resp["resultType"].GetString(), "complete");
    EXPECT_FALSE(cancel_resp.Contains("taskId"));
    EXPECT_FALSE(cancel_resp.Contains("status"));

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, SamplingInputWithoutCapabilityRejects32021) {
    auto env = MakeBareModernTaskEnv();

    auto sampling_tool = [](const Ctx&) {
        SamplingMessage message;
        message.role = "user";
        TextContent content;
        content.text = "Reply with the single word: pong";
        message.content = std::move(content);
        CreateMessageRequestParams sampling_params;
        sampling_params.messages.push_back(std::move(message));
        sampling_params.max_tokens = 16;
        CallToolResult result;
        InputRequiredResult ir;
        ir.input_requests["message"] = MakeInputRequestForSampling(sampling_params);
        result.input_required = std::move(ir);
        return result;
    };
    env.server->RegisterTool("needs_sampling",
        ToolOptions{}.Description("requires sampling"),
        std::function<CallToolResult(const Ctx&)>(sampling_tool));

    CallToolRequestParams params;
    params.name = "needs_sampling";
    auto resp = SendBareRequest(env.client, methods::kCallTool,
        SerializeCallToolRequestParams(params));
    ASSERT_TRUE(resp.Contains("code"));
    EXPECT_EQ(resp["code"].GetInt(), int64_t(-32021));
    auto* data = resp.Find("data");
    ASSERT_NE(data, nullptr);
    auto* required = data->Find("requiredCapabilities");
    ASSERT_NE(required, nullptr);
    ASSERT_NE(required->Find("sampling"), nullptr);

    auto declared = SendBareRequest(env.client, methods::kCallTool,
        SerializeCallToolRequestParams(params), MetaWithSampling());
    ASSERT_FALSE(declared.Contains("code"));
    EXPECT_EQ(declared["resultType"].GetString(), "input_required");
    ASSERT_TRUE(declared.Contains("inputRequests"));

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, SamplingInputWithoutCapabilityPassesInLegacyEra) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLegacyProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), sopts);
    server->RegisterTool("legacy_sampling",
        ToolOptions{}.Description("legacy sampling"),
        std::function<CallToolResult(const Ctx&)>([](const Ctx&) {
            SamplingMessage message;
            message.role = "user";
            TextContent content;
            content.text = "Reply with the single word: pong";
            message.content = std::move(content);
            CreateMessageRequestParams sampling_params;
            sampling_params.messages.push_back(std::move(message));
            sampling_params.max_tokens = 16;
            CallToolResult result;
            InputRequiredResult ir;
            ir.input_requests["message"] =
                MakeInputRequestForSampling(sampling_params);
            result.input_required = std::move(ir);
            return result;
        }));

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    InitializeRequestParams iparams;
    iparams.protocol_version = std::string(kLegacyProtocolVersion);
    iparams.client_info = Implementation{"test-client", "1.0"};
    iparams.capabilities.roots = RootsCapability{};
    auto init_future = client->SendRequest(methods::kInitialize,
        SerializeInitializeRequestParams(iparams), {},
        std::chrono::milliseconds(2000));
    ASSERT_EQ(init_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    ASSERT_FALSE(init_future.get().Contains("code"));
    client->SendNotification(notifications::kInitialized);

    CallToolRequestParams params;
    params.name = "legacy_sampling";
    auto resp = SendBareRequest(client, methods::kCallTool,
        SerializeCallToolRequestParams(params));
    ASSERT_FALSE(resp.Contains("code"));
    EXPECT_EQ(resp["resultType"].GetString(), "input_required");

    server->Close();
    client->Close();
}

TEST(McpServerTest, TasksGetCarriesCompleteResultType) {
    auto env = MakeBareModernTaskEnv();

    env.server->RegisterTool("polled_task", MakeTaskToolOptions(),
        [](const Ctx&) {
            CallToolResult r;
            r.content.push_back(TextContent{"text", "polled-done"});
            return r;
        });

    CallToolRequestParams params;
    params.name = "polled_task";
    auto created_resp = SendBareRequest(env.client, methods::kCallTool,
        SerializeCallToolRequestParams(params), MetaWithTasksExtension());
    ASSERT_FALSE(created_resp.Contains("code"));
    auto created = DeserializeCreateTaskResult(created_resp);
    ASSERT_FALSE(created.task_id.empty());

    GetTaskRequestParams get_params;
    get_params.task_id = created.task_id;
    auto get_resp = SendBareRequest(env.client, methods::kGetTask,
        SerializeGetTaskRequestParams(get_params), MetaWithTasksExtension());
    ASSERT_FALSE(get_resp.Contains("code"));
    EXPECT_EQ(get_resp["resultType"].GetString(), "complete");
    EXPECT_EQ(get_resp["taskId"].GetString(), created.task_id);

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, LegacyTasksFlowWithoutExtensionUnaffected) {
    auto env = MakeTaskEraEnv();
    PerformLegacyHandshake(env.client);

    env.server->RegisterTool("legacy_task", MakeTaskToolOptions(),
        [](const Ctx&) {
            CallToolResult r;
            r.content.push_back(TextContent{"text", "legacy-done"});
            return r;
        });

    CallToolRequestParams params;
    params.name = "legacy_task";
    auto created_resp = SendBareRequest(env.client, methods::kCallTool,
        SerializeCallToolRequestParams(params));
    ASSERT_FALSE(created_resp.Contains("code"));
    auto created = DeserializeCreateTaskResult(created_resp);
    ASSERT_FALSE(created.task_id.empty());

    GetTaskRequestParams unknown;
    unknown.task_id = "no-such-task";
    auto unknown_resp = SendBareRequest(env.client, methods::kGetTask,
        SerializeGetTaskRequestParams(unknown));
    ASSERT_TRUE(unknown_resp.Contains("code"));
    EXPECT_EQ(unknown_resp["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::InvalidParams));

    GetTaskRequestParams get_params;
    get_params.task_id = created.task_id;
    auto get_resp = SendBareRequest(env.client, methods::kGetTask,
        SerializeGetTaskRequestParams(get_params));
    ASSERT_FALSE(get_resp.Contains("code"));

    CloseTaskEraEnv(env);
}

namespace {

struct SkillsEnv {
    std::unique_ptr<McpServer> server;
    std::shared_ptr<McpSessionHandler> client;
};

SkillsEnv MakeSkillsEnv(bool directory_read) {
    SkillsEnv env;
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    sopts.enable_skills_extension = true;
    env.server = McpServer::Create(std::move(pair.server), sopts);
    env.server->RegisterResource("code-review-skill", "skill://code-review/SKILL.md",
        ResourceOptions{}.MimeType("text/markdown"),
        [](const std::string& uri) -> ReadResourceResult {
            TextResourceContents trc;
            trc.uri = uri;
            trc.text = "# Code Review";
            trc.mime_type = "text/markdown";
            ReadResourceResult rr;
            rr.contents = {mcp::ResourceContents{trc}};
            return rr;
        });
    Skill review;
    review.uri = "skill://code-review/SKILL.md";
    JsonValue fm(JsonValue::object_tag);
    fm["name"] = JsonValue("code-review");
    fm["description"] = JsonValue("Review code.");
    review.frontmatter = fm;
    review.resources = SkillResources{std::vector<SkillResource>{
        SkillResource{"skill://code-review/SKILL.md", "sha256:aaa", int64_t(128)},
        SkillResource{"skill://code-review/references/checklist.md", "sha256:bbb", int64_t(64)},
        SkillResource{"skill://code-review/references/deep/guide.md", "sha256:ccc", int64_t(32)},
        SkillResource{"skill://code-review/scripts/run.py", "sha256:ddd", int64_t(16)},
    }};
    env.server->RegisterSkill(review);
    if (directory_read) env.server->SetSkillsDirectoryRead(true);
    env.client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    env.client->Start();
    NegotiateModernVersion(env.server);
    return env;
}

void CloseSkillsEnv(SkillsEnv& env) {
    env.server->Close();
    env.client->Close();
}

JsonValue SendSkillsRequest(
    const std::shared_ptr<McpSessionHandler>& client,
    std::string_view method,
    JsonValue params)
{
    auto future = client->SendRequest(method, std::move(params), {},
        std::chrono::milliseconds(2000));
    if (future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        ADD_FAILURE();
        JsonValue err(JsonValue::object_tag);
        err["code"] = JsonValue(static_cast<int64_t>(McpErrorCode::InternalError));
        return err;
    }
    return future.get();
}

} // namespace

TEST(McpServerTest, SkillsListGetDirectory) {
    auto env = MakeSkillsEnv(true);

    auto caps = env.server->GetCapabilities();
    EXPECT_TRUE(caps.extensions.has_value());
    ASSERT_TRUE(caps.extensions->find(
        std::string(kSkillsExtensionId)) !=
        caps.extensions->end());

    auto list_resp = SendSkillsRequest(env.client, methods::kListSkills,
        SerializePaginatedRequestParams(PaginatedRequestParams{}));
    ASSERT_FALSE(list_resp.Contains("code"));
    EXPECT_EQ(list_resp["resultType"].GetString(), "complete");
    auto* skills_arr = list_resp.Find("skills");
    ASSERT_NE(skills_arr, nullptr);
    ASSERT_TRUE(skills_arr->IsArray());
    ASSERT_EQ(skills_arr->Size(), 1u);
    const auto& entry = (*skills_arr)[0];
    EXPECT_EQ(entry.Find("uri")->GetString(), "skill://code-review/SKILL.md");
    EXPECT_EQ(entry.Find("frontmatter")->Find("name")->GetString(), "code-review");
    EXPECT_EQ(entry.Find("frontmatter")->Find("description")->GetString(), "Review code.");
    auto* manifest = entry.Find("resources");
    ASSERT_NE(manifest, nullptr);
    ASSERT_TRUE(manifest->IsArray());
    ASSERT_EQ(manifest->Size(), 4u);
    EXPECT_EQ((*manifest)[0].Find("digest")->GetString(), "sha256:aaa");

    PaginatedRequestParams second_page;
    second_page.cursor = "1";
    auto page_resp = SendSkillsRequest(env.client, methods::kListSkills,
        SerializePaginatedRequestParams(second_page));
    ASSERT_FALSE(page_resp.Contains("code"));
    ASSERT_TRUE(page_resp.Find("skills")->IsArray());
    EXPECT_EQ(page_resp.Find("skills")->Size(), 0u);
    EXPECT_FALSE(page_resp.Contains("nextCursor"));

    SkillsGetRequestParams get_params;
    get_params.uri = "skill://code-review/SKILL.md";
    auto get_resp = SendSkillsRequest(env.client, methods::kGetSkill,
        SerializeSkillsGetRequestParams(get_params));
    ASSERT_FALSE(get_resp.Contains("code"));
    EXPECT_EQ(get_resp["resultType"].GetString(), "complete");
    auto got = DeserializeSkillsGetResult(get_resp);
    EXPECT_EQ(got.skill.uri, "skill://code-review/SKILL.md");
    EXPECT_EQ(got.skill.frontmatter.Find("name")->GetString(), "code-review");
    ASSERT_TRUE(std::holds_alternative<std::vector<SkillResource>>(got.skill.resources));
    EXPECT_EQ(std::get<std::vector<SkillResource>>(got.skill.resources).size(), 4u);

    SkillsGetRequestParams missing_params;
    missing_params.uri = "skill://nope/SKILL.md";
    auto missing_resp = SendSkillsRequest(env.client, methods::kGetSkill,
        SerializeSkillsGetRequestParams(missing_params));
    ASSERT_TRUE(missing_resp.Contains("code"));
    EXPECT_EQ(missing_resp["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::InvalidParams));

    ReadDirectoryRequestParams root_params;
    root_params.uri = "skill://code-review";
    auto root_resp = SendSkillsRequest(env.client, methods::kReadDirectory,
        SerializeReadDirectoryRequestParams(root_params));
    ASSERT_FALSE(root_resp.Contains("code"));
    auto root = DeserializeReadDirectoryResult(root_resp);
    ASSERT_EQ(root.resources.size(), 3u);
    EXPECT_EQ(root.resources[0].uri, "skill://code-review/SKILL.md");
    EXPECT_EQ(root.resources[0].name, "SKILL.md");
    EXPECT_FALSE(root.resources[0].mime_type.has_value());
    EXPECT_EQ(root.resources[1].uri, "skill://code-review/references");
    EXPECT_EQ(root.resources[1].name, "references");
    ASSERT_TRUE(root.resources[1].mime_type.has_value());
    EXPECT_EQ(*root.resources[1].mime_type, "inode/directory");
    EXPECT_EQ(root.resources[2].uri, "skill://code-review/scripts");
    EXPECT_EQ(root.resources[2].name, "scripts");
    ASSERT_TRUE(root.resources[2].mime_type.has_value());
    EXPECT_EQ(*root.resources[2].mime_type, "inode/directory");
    EXPECT_FALSE(root.next_cursor.has_value());

    ReadDirectoryRequestParams sub_params;
    sub_params.uri = "skill://code-review/references";
    auto sub_resp = SendSkillsRequest(env.client, methods::kReadDirectory,
        SerializeReadDirectoryRequestParams(sub_params));
    ASSERT_FALSE(sub_resp.Contains("code"));
    auto sub = DeserializeReadDirectoryResult(sub_resp);
    ASSERT_EQ(sub.resources.size(), 2u);
    EXPECT_EQ(sub.resources[0].uri, "skill://code-review/references/checklist.md");
    EXPECT_EQ(sub.resources[0].name, "checklist.md");
    EXPECT_EQ(sub.resources[1].uri, "skill://code-review/references/deep");
    ASSERT_TRUE(sub.resources[1].mime_type.has_value());
    EXPECT_EQ(*sub.resources[1].mime_type, "inode/directory");

    ReadDirectoryRequestParams slash_params;
    slash_params.uri = "skill://code-review/";
    auto slash_resp = SendSkillsRequest(env.client, methods::kReadDirectory,
        SerializeReadDirectoryRequestParams(slash_params));
    ASSERT_FALSE(slash_resp.Contains("code"));
    EXPECT_EQ(DeserializeReadDirectoryResult(slash_resp).resources.size(), 3u);

    ReadDirectoryRequestParams file_params;
    file_params.uri = "skill://code-review/SKILL.md";
    auto file_resp = SendSkillsRequest(env.client, methods::kReadDirectory,
        SerializeReadDirectoryRequestParams(file_params));
    ASSERT_TRUE(file_resp.Contains("code"));
    EXPECT_EQ(file_resp["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::InvalidParams));

    ReadDirectoryRequestParams unknown_params;
    unknown_params.uri = "skill://code-review/nope";
    auto unknown_resp = SendSkillsRequest(env.client, methods::kReadDirectory,
        SerializeReadDirectoryRequestParams(unknown_params));
    ASSERT_TRUE(unknown_resp.Contains("code"));
    EXPECT_EQ(unknown_resp["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::InvalidParams));

    ResourceRequestParams read_params;
    read_params.uri = "skill://code-review/SKILL.md";
    auto read_resp = SendSkillsRequest(env.client, methods::kReadResource,
        SerializeResourceRequestParams(read_params));
    ASSERT_FALSE(read_resp.Contains("code"));
    auto* contents = read_resp.Find("contents");
    ASSERT_NE(contents, nullptr);
    ASSERT_EQ(contents->Size(), 1u);
    EXPECT_EQ((*contents)[0].Find("text")->GetString(), "# Code Review");

    Skill other_scheme;
    other_scheme.uri = "https://example.com/code-review/SKILL.md";
    other_scheme.frontmatter = JsonValue(JsonValue::object_tag);
    other_scheme.resources = SkillResources{std::vector<SkillResource>{}};
    EXPECT_NO_THROW(env.server->RegisterSkill(other_scheme));

    Skill nested;
    nested.uri = "skill://org/nested/deep/SKILL.md";
    JsonValue nested_fm(JsonValue::object_tag);
    nested_fm["name"] = JsonValue("deep");
    nested.frontmatter = nested_fm;
    nested.resources = SkillResources{std::vector<SkillResource>{
        SkillResource{"skill://org/nested/deep/SKILL.md", "sha256:eee", int64_t(64)},
    }};
    EXPECT_NO_THROW(env.server->RegisterSkill(nested));

    SkillsGetRequestParams nested_params;
    nested_params.uri = "skill://org/nested/deep/SKILL.md";
    auto nested_resp = SendSkillsRequest(env.client, methods::kGetSkill,
        SerializeSkillsGetRequestParams(nested_params));
    ASSERT_FALSE(nested_resp.Contains("code"));
    EXPECT_EQ(DeserializeSkillsGetResult(nested_resp).skill.uri,
        "skill://org/nested/deep/SKILL.md");

    ReadDirectoryRequestParams nested_dir_params;
    nested_dir_params.uri = "skill://org/nested";
    auto nested_dir_resp = SendSkillsRequest(env.client, methods::kReadDirectory,
        SerializeReadDirectoryRequestParams(nested_dir_params));
    ASSERT_FALSE(nested_dir_resp.Contains("code"));
    auto nested_dir = DeserializeReadDirectoryResult(nested_dir_resp);
    ASSERT_EQ(nested_dir.resources.size(), 1u);
    EXPECT_EQ(nested_dir.resources[0].uri, "skill://org/nested/deep");
    ASSERT_TRUE(nested_dir.resources[0].mime_type.has_value());
    EXPECT_EQ(*nested_dir.resources[0].mime_type, "inode/directory");

    Skill bad_shape;
    bad_shape.uri = "skill://code-review/other.md";
    EXPECT_THROW(env.server->RegisterSkill(bad_shape), mcp::McpError);

    CloseSkillsEnv(env);
}

TEST(McpServerTest, SkillsDirectoryPagination) {
    auto env = MakeSkillsEnv(true);

    Skill bulk;
    bulk.uri = "skill://bulk/SKILL.md";
    bulk.frontmatter = JsonValue(JsonValue::object_tag);
    std::vector<SkillResource> files;
    files.push_back(SkillResource{"skill://bulk/SKILL.md", "sha256:root", int64_t(8)});
    for (int i = 0; i < 105; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "skill://bulk/file-%03d.md", i);
        files.push_back(SkillResource{std::string(name), "sha256:x", int64_t(8)});
    }
    bulk.resources = SkillResources{std::move(files)};
    env.server->RegisterSkill(bulk);

    ReadDirectoryRequestParams first_params;
    first_params.uri = "skill://bulk";
    auto first_resp = SendSkillsRequest(env.client, methods::kReadDirectory,
        SerializeReadDirectoryRequestParams(first_params));
    ASSERT_FALSE(first_resp.Contains("code"));
    auto first = DeserializeReadDirectoryResult(first_resp);
    ASSERT_EQ(first.resources.size(), 100u);
    ASSERT_TRUE(first.next_cursor.has_value());
    EXPECT_EQ(*first.next_cursor, "100");
    EXPECT_EQ(first.resources[0].uri, "skill://bulk/SKILL.md");

    ReadDirectoryRequestParams second_params;
    second_params.uri = "skill://bulk";
    second_params.cursor = first.next_cursor;
    auto second_resp = SendSkillsRequest(env.client, methods::kReadDirectory,
        SerializeReadDirectoryRequestParams(second_params));
    ASSERT_FALSE(second_resp.Contains("code"));
    auto second = DeserializeReadDirectoryResult(second_resp);
    EXPECT_EQ(second.resources.size(), 6u);
    EXPECT_FALSE(second.next_cursor.has_value());

    std::vector<std::string> union_uris;
    for (const auto& c : first.resources) union_uris.push_back(c.uri);
    for (const auto& c : second.resources) union_uris.push_back(c.uri);
    std::sort(union_uris.begin(), union_uris.end());
    union_uris.erase(std::unique(union_uris.begin(), union_uris.end()), union_uris.end());
    EXPECT_EQ(union_uris.size(), 106u);
    EXPECT_EQ(union_uris.front(), "skill://bulk/SKILL.md");
    EXPECT_EQ(union_uris.back(), "skill://bulk/file-104.md");

    CloseSkillsEnv(env);
}

TEST(McpServerTest, SkillsListPaginationAtomic) {
    auto env = MakeSkillsEnv(false);

    for (int i = 0; i < 105; ++i) {
        char uri[48];
        std::snprintf(uri, sizeof(uri), "skill://page-%03d/SKILL.md", i);
        Skill entry;
        entry.uri = uri;
        entry.frontmatter = JsonValue(JsonValue::object_tag);
        entry.resources = SkillResources{std::vector<SkillResource>{}};
        env.server->RegisterSkill(std::move(entry));
    }

    auto first_resp = SendSkillsRequest(env.client, methods::kListSkills,
        SerializePaginatedRequestParams(PaginatedRequestParams{}));
    ASSERT_FALSE(first_resp.Contains("code"));
    auto first = DeserializeSkillsListResult(first_resp);
    ASSERT_EQ(first.skills.size(), 100u);
    ASSERT_TRUE(first.next_cursor.has_value());
    EXPECT_EQ(*first.next_cursor, "100");

    PaginatedRequestParams second_page;
    second_page.cursor = first.next_cursor;
    auto second_resp = SendSkillsRequest(env.client, methods::kListSkills,
        SerializePaginatedRequestParams(second_page));
    ASSERT_FALSE(second_resp.Contains("code"));
    auto second = DeserializeSkillsListResult(second_resp);
    EXPECT_EQ(second.skills.size(), 6u);
    EXPECT_FALSE(second.next_cursor.has_value());

    std::vector<std::string> union_uris;
    for (const auto& s : first.skills) union_uris.push_back(s.uri);
    for (const auto& s : second.skills) union_uris.push_back(s.uri);
    std::sort(union_uris.begin(), union_uris.end());
    union_uris.erase(std::unique(union_uris.begin(), union_uris.end()), union_uris.end());
    EXPECT_EQ(union_uris.size(), 106u);
    EXPECT_EQ(union_uris.front(), "skill://code-review/SKILL.md");
    EXPECT_EQ(union_uris.back(), "skill://page-104/SKILL.md");

    CloseSkillsEnv(env);
}

TEST(McpServerTest, DirectoryReadRequiresDeclaration) {
    auto env = MakeSkillsEnv(false);

    ReadDirectoryRequestParams params;
    params.uri = "skill://code-review";
    auto denied_resp = SendSkillsRequest(env.client, methods::kReadDirectory,
        SerializeReadDirectoryRequestParams(params));
    ASSERT_TRUE(denied_resp.Contains("code"));
    EXPECT_EQ(denied_resp["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::InvalidParams));

    DiscoverRequestParams discover_params;
    auto discover_resp = SendSkillsRequest(env.client, methods::kDiscover,
        SerializeDiscoverRequestParams(discover_params));
    ASSERT_FALSE(discover_resp.Contains("code"));
    auto* caps = discover_resp.Find("capabilities");
    ASSERT_NE(caps, nullptr);
    auto* exts = caps->Find("extensions");
    ASSERT_NE(exts, nullptr);
    auto* skills_ext = exts->Find(std::string(kSkillsExtensionId));
    ASSERT_NE(skills_ext, nullptr);
    EXPECT_TRUE(skills_ext->IsObject());
    EXPECT_TRUE(skills_ext->Empty());

    env.server->SetSkillsDirectoryRead(true);

    auto allowed_resp = SendSkillsRequest(env.client, methods::kReadDirectory,
        SerializeReadDirectoryRequestParams(params));
    ASSERT_FALSE(allowed_resp.Contains("code"));
    EXPECT_EQ(DeserializeReadDirectoryResult(allowed_resp).resources.size(), 3u);

    auto rediscover_resp = SendSkillsRequest(env.client, methods::kDiscover,
        SerializeDiscoverRequestParams(discover_params));
    ASSERT_FALSE(rediscover_resp.Contains("code"));
    auto* rediscover_exts = rediscover_resp.Find("capabilities")->Find("extensions");
    ASSERT_NE(rediscover_exts, nullptr);
    auto* rediscover_skills = rediscover_exts->Find(std::string(kSkillsExtensionId));
    ASSERT_NE(rediscover_skills, nullptr);
    ASSERT_TRUE(rediscover_skills->Find("directoryRead") != nullptr);
    EXPECT_TRUE(rediscover_skills->Find("directoryRead")->GetBool());

    CloseSkillsEnv(env);
}

TEST(McpServerTest, DynamicSkillPassthrough) {
    auto env = MakeSkillsEnv(true);

    Skill dynamic_skill;
    dynamic_skill.uri = "skill://gen/SKILL.md";
    JsonValue fm(JsonValue::object_tag);
    fm["name"] = JsonValue("gen");
    fm["description"] = JsonValue("Generated skill.");
    dynamic_skill.frontmatter = fm;
    dynamic_skill.resources = SkillResources{std::string("dynamic")};
    env.server->RegisterSkill(dynamic_skill);

    auto list_resp = SendSkillsRequest(env.client, methods::kListSkills,
        SerializePaginatedRequestParams(PaginatedRequestParams{}));
    ASSERT_FALSE(list_resp.Contains("code"));
    auto listed = DeserializeSkillsListResult(list_resp);
    ASSERT_EQ(listed.skills.size(), 2u);
    EXPECT_EQ(listed.skills[1].uri, "skill://gen/SKILL.md");
    EXPECT_EQ(listed.skills[1].frontmatter.Find("description")->GetString(), "Generated skill.");
    ASSERT_TRUE(std::holds_alternative<std::string>(listed.skills[1].resources));
    EXPECT_EQ(std::get<std::string>(listed.skills[1].resources), "dynamic");

    SkillsGetRequestParams get_params;
    get_params.uri = "skill://gen/SKILL.md";
    auto get_resp = SendSkillsRequest(env.client, methods::kGetSkill,
        SerializeSkillsGetRequestParams(get_params));
    ASSERT_FALSE(get_resp.Contains("code"));
    auto got = DeserializeSkillsGetResult(get_resp);
    EXPECT_EQ(got.skill.uri, "skill://gen/SKILL.md");
    ASSERT_TRUE(std::holds_alternative<std::string>(got.skill.resources));
    EXPECT_EQ(std::get<std::string>(got.skill.resources), "dynamic");

    ReadDirectoryRequestParams dir_params;
    dir_params.uri = "skill://gen";
    auto dir_resp = SendSkillsRequest(env.client, methods::kReadDirectory,
        SerializeReadDirectoryRequestParams(dir_params));
    ASSERT_FALSE(dir_resp.Contains("code"));
    auto dir = DeserializeReadDirectoryResult(dir_resp);
    EXPECT_TRUE(dir.resources.empty());
    EXPECT_FALSE(dir.next_cursor.has_value());

    CloseSkillsEnv(env);
}

TEST(McpServerTest, SkillsDiscoverIncludesExtensionsAndCache) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    sopts.enable_skills_extension = true;
    sopts.enable_tasks_extension = true;
    CacheHint discover_hint;
    discover_hint.ttl_ms = int64_t(60000);
    discover_hint.cache_scope = "public";
    sopts.cache_hints = std::map<std::string, CacheHint, std::less<>>{
        {"server/discover", discover_hint}};
    auto store_path = MakeTaskStorePath();
    std::error_code ec;
    std::filesystem::remove(store_path, ec);
    sopts.task_store = std::make_shared<FileTaskStore>(store_path);
    auto server = McpServer::Create(std::move(pair.server), sopts);
    server->RegisterResource("code-review-skill", "skill://code-review/SKILL.md",
        ResourceOptions{},
        [](const std::string& uri) -> ReadResourceResult {
            TextResourceContents trc;
            trc.uri = uri;
            trc.text = "skill";
            ReadResourceResult rr;
            rr.contents = {mcp::ResourceContents{trc}};
            return rr;
        });
    Skill review;
    review.uri = "skill://code-review/SKILL.md";
    review.frontmatter = JsonValue(JsonValue::object_tag);
    review.resources = SkillResources{std::string("dynamic")};
    server->RegisterSkill(review);
    server->SetSkillsDirectoryRead(true);

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();

    DiscoverRequestParams discover_params;
    auto future = client->SendRequest(methods::kDiscover,
        SerializeDiscoverRequestParams(discover_params), {},
        std::chrono::milliseconds(2000));
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));
    auto discovered = DeserializeDiscoverResult(resp);
    std::vector<std::string> expected_versions;
    for (auto v : kProtocolVersions) expected_versions.emplace_back(v);
    EXPECT_EQ(discovered.supported_versions, expected_versions);
    EXPECT_EQ(discovered.server_info.name, "test-server");
    ASSERT_TRUE(discovered.capabilities.extensions.has_value());
    EXPECT_TRUE(discovered.capabilities.extensions->find(std::string(kTasksExtensionId)) !=
        discovered.capabilities.extensions->end());
    auto skills_it = discovered.capabilities.extensions->find(std::string(kSkillsExtensionId));
    ASSERT_TRUE(skills_it != discovered.capabilities.extensions->end());
    EXPECT_TRUE(DeserializeSkillsExtensionSettings(skills_it->second).directory_read);
    EXPECT_EQ(resp["ttlMs"].GetInt(), int64_t(60000));
    EXPECT_EQ(resp["cacheScope"].GetString(), "public");

    server->Close();
    client->Close();
    std::filesystem::remove(store_path, ec);
}

TEST(McpServerTest, SkillsListChangedSubscriptionGating) {
    auto ctx = MakeModernServerWithClient();
    NegotiateModernVersion(ctx.server);

    std::promise<JsonRpcNotification> ack_first;
    auto ack_first_future = ack_first.get_future();
    ctx.client->SetNotificationHandler(notifications::kSubscriptionsAcknowledged,
        [&ack_first](const JsonRpcNotification& n) {
            ack_first.set_value(n);
        });

    SubscriptionsListenRequestParams tools_only;
    tools_only.notifications.tools_list_changed = true;
    auto listen_future = ctx.client->SendRequest(methods::kSubscribe,
        SerializeSubscriptionsListenRequestParams(tools_only),
        MetaWithSubscriptionId("sub-tools"),
        std::chrono::milliseconds(2000));
    ASSERT_EQ(ack_first_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    ASSERT_EQ(listen_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    ASSERT_FALSE(listen_future.get().Contains("code"));

    std::promise<JsonRpcNotification> prompt_skipped;
    auto prompt_skipped_future = prompt_skipped.get_future();
    ctx.client->SetNotificationHandler(notifications::kPromptListChanged,
        [&prompt_skipped](const JsonRpcNotification& n) {
            prompt_skipped.set_value(n);
        });
    ctx.server->SendPromptListChanged();
    EXPECT_EQ(prompt_skipped_future.wait_for(std::chrono::milliseconds(300)),
              std::future_status::timeout);

    std::promise<JsonRpcNotification> tools_received;
    auto tools_future = tools_received.get_future();
    ctx.client->SetNotificationHandler(notifications::kToolListChanged,
        [&tools_received](const JsonRpcNotification& n) {
            tools_received.set_value(n);
        });
    ctx.server->SendToolListChanged();
    ASSERT_EQ(tools_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto tools_notif = tools_future.get();
    ASSERT_TRUE(tools_notif.meta.has_value());
    auto* tools_sid = tools_notif.meta->Find("io.modelcontextprotocol/subscriptionId");
    ASSERT_NE(tools_sid, nullptr);
    EXPECT_EQ(tools_sid->GetString(), "sub-tools");

    std::promise<JsonRpcNotification> ack_second;
    auto ack_second_future = ack_second.get_future();
    ctx.client->SetNotificationHandler(notifications::kSubscriptionsAcknowledged,
        [&ack_second](const JsonRpcNotification& n) {
            ack_second.set_value(n);
        });
    SubscriptionsListenRequestParams prompts_and_resources;
    prompts_and_resources.notifications.prompts_list_changed = true;
    prompts_and_resources.notifications.resources_list_changed = true;
    auto listen2_future = ctx.client->SendRequest(methods::kSubscribe,
        SerializeSubscriptionsListenRequestParams(prompts_and_resources),
        MetaWithSubscriptionId("sub-pr"),
        std::chrono::milliseconds(2000));
    ASSERT_EQ(ack_second_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    ASSERT_EQ(listen2_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto ack2 = ack_second_future.get();
    auto* ack2_sid = ack2.meta->Find("io.modelcontextprotocol/subscriptionId");
    ASSERT_NE(ack2_sid, nullptr);
    EXPECT_EQ(ack2_sid->GetString(), "sub-pr");

    std::promise<JsonRpcNotification> prompt_received;
    auto prompt_future = prompt_received.get_future();
    ctx.client->SetNotificationHandler(notifications::kPromptListChanged,
        [&prompt_received](const JsonRpcNotification& n) {
            prompt_received.set_value(n);
        });
    ctx.server->SendPromptListChanged();
    ASSERT_EQ(prompt_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto prompt_notif = prompt_future.get();
    ASSERT_TRUE(prompt_notif.meta.has_value());
    auto* prompt_sid = prompt_notif.meta->Find("io.modelcontextprotocol/subscriptionId");
    ASSERT_NE(prompt_sid, nullptr);
    EXPECT_EQ(prompt_sid->GetString(), "sub-pr");

    std::promise<JsonRpcNotification> resources_received;
    auto resources_future = resources_received.get_future();
    ctx.client->SetNotificationHandler(notifications::kResourceListChanged,
        [&resources_received](const JsonRpcNotification& n) {
            resources_received.set_value(n);
        });
    ctx.server->SendResourceListChanged();
    ASSERT_EQ(resources_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resources_notif = resources_future.get();
    ASSERT_TRUE(resources_notif.meta.has_value());
    auto* resources_sid = resources_notif.meta->Find("io.modelcontextprotocol/subscriptionId");
    ASSERT_NE(resources_sid, nullptr);
    EXPECT_EQ(resources_sid->GetString(), "sub-pr");

    ctx.server->Close();
    ctx.client->Close();
}

TEST(McpServerTest, SkillsMethodsRejectLegacyEra) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLegacyProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), sopts);
    server->RegisterResource("code-review-skill", "skill://code-review/SKILL.md",
        ResourceOptions{},
        [](const std::string& uri) -> ReadResourceResult {
            TextResourceContents trc;
            trc.uri = uri;
            trc.text = "skill";
            ReadResourceResult rr;
            rr.contents = {mcp::ResourceContents{trc}};
            return rr;
        });
    Skill review;
    review.uri = "skill://code-review/SKILL.md";
    review.frontmatter = JsonValue(JsonValue::object_tag);
    review.resources = SkillResources{std::string("dynamic")};
    server->RegisterSkill(review);

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    client->Start();

    InitializeRequestParams iparams;
    iparams.protocol_version = std::string(kLegacyProtocolVersion);
    iparams.client_info = Implementation{"test-client", "1.0"};
    auto init_future = client->SendRequest(methods::kInitialize,
        SerializeInitializeRequestParams(iparams), {},
        std::chrono::milliseconds(2000));
    ASSERT_EQ(init_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    ASSERT_FALSE(init_future.get().Contains("code"));
    client->SendNotification(notifications::kInitialized);

    auto future = client->SendRequest(methods::kListSkills,
        SerializePaginatedRequestParams(PaginatedRequestParams{}), {},
        std::chrono::milliseconds(2000));
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_TRUE(resp.Contains("code"));
    EXPECT_EQ(resp["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::MethodNotFound));

    server->Close();
    client->Close();
}

TEST(McpServerTest, TaskInputRequiredParksWithInputRequests) {
    auto env = MakeModernTaskEnv();

    env.server->RegisterTool("park_tool", MakeTaskToolOptions(),
        [](const Ctx&) {
            CallToolResult result;
            InputRequiredResult ir;
            ElicitRequestParams first_params;
            first_params.message = "First input";
            ir.input_requests["first"] = MakeInputRequestForElicitation(first_params);
            ElicitRequestParams second_params;
            second_params.message = "Second input";
            ir.input_requests["second"] = MakeInputRequestForElicitation(second_params);
            result.input_required = std::move(ir);
            return result;
        });

    CallToolRequestParams params;
    params.name = "park_tool";
    auto created_resp = SendBareRequest(env.client, methods::kCallTool,
        SerializeCallToolRequestParams(params), MetaWithTasksExtension());
    ASSERT_FALSE(created_resp.Contains("code"));
    EXPECT_EQ(created_resp["resultType"].GetString(), "task");
    auto created = DeserializeCreateTaskResult(created_resp);
    ASSERT_FALSE(created.task_id.empty());

    auto parked = WaitForTaskStatus(env.store, created.task_id, TaskStatus::InputRequired);
    ASSERT_TRUE(parked.has_value());
    ASSERT_TRUE(parked->input_requests.has_value());
    EXPECT_EQ(parked->input_requests->size(), size_t{2});

    GetTaskRequestParams get_params;
    get_params.task_id = created.task_id;
    auto get_resp = SendBareRequest(env.client, methods::kGetTask,
        SerializeGetTaskRequestParams(get_params), MetaWithTasksExtension());
    ASSERT_FALSE(get_resp.Contains("code"));
    EXPECT_EQ(get_resp["status"].GetString(), "input_required");
    auto* requests = get_resp.Find("inputRequests");
    ASSERT_NE(requests, nullptr);
    ASSERT_TRUE(requests->IsObject());
    EXPECT_EQ(requests->GetObject().size(), size_t{2});
    for (const auto& entry : requests->GetObject()) {
        auto* method = entry.second.Find("method");
        ASSERT_NE(method, nullptr);
        EXPECT_TRUE(method->IsString());
    }

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, TasksUpdateResumeCompletesParkedTask) {
    auto env = MakeModernTaskEnv();

    env.server->RegisterTool("confirm_tool", MakeTaskToolOptions(),
        [](const Ctx& ctx) {
            auto* entry = ctx.Params().input_responses
                ? ctx.Params().input_responses->Find("confirm_delete") : nullptr;
            auto* action = entry && entry->IsObject() ? entry->Find("action") : nullptr;
            auto* content = entry && entry->IsObject() ? entry->Find("content") : nullptr;
            auto* confirm = content && content->IsObject()
                ? content->Find("confirm") : nullptr;
            if (action && action->IsString() && action->GetString() == "accept"
                && confirm && confirm->IsBool() && confirm->GetBool()) {
                CallToolResult done;
                done.content.push_back(TextContent{"text", "Deleted report.txt"});
                return done;
            }
            CallToolResult result;
            InputRequiredResult ir;
            ElicitRequestParams elicit_params;
            elicit_params.message = "Confirm deletion";
            ir.input_requests["confirm_delete"] =
                MakeInputRequestForElicitation(elicit_params);
            result.input_required = std::move(ir);
            return result;
        });

    CallToolRequestParams params;
    params.name = "confirm_tool";
    JsonValue args(JsonValue::object_tag);
    args["filename"] = JsonValue("report.txt");
    params.arguments = std::move(args);
    auto created_resp = SendBareRequest(env.client, methods::kCallTool,
        SerializeCallToolRequestParams(params), MetaWithTasksExtension());
    ASSERT_FALSE(created_resp.Contains("code"));
    auto created = DeserializeCreateTaskResult(created_resp);
    ASSERT_FALSE(created.task_id.empty());

    auto parked = WaitForTaskStatus(env.store, created.task_id, TaskStatus::InputRequired);
    ASSERT_TRUE(parked.has_value());
    ASSERT_TRUE(parked->tool_name.has_value());
    EXPECT_EQ(*parked->tool_name, "confirm_tool");
    ASSERT_TRUE(parked->tool_arguments.has_value());

    UpdateTaskRequestParams up;
    up.task_id = created.task_id;
    JsonValue responses(JsonValue::object_tag);
    JsonValue entry(JsonValue::object_tag);
    entry["action"] = JsonValue("accept");
    JsonValue content(JsonValue::object_tag);
    content["confirm"] = JsonValue(true);
    entry["content"] = std::move(content);
    responses["confirm_delete"] = std::move(entry);
    up.input_responses = std::move(responses);
    auto up_resp = SendBareRequest(env.client, methods::kUpdateTask,
        SerializeUpdateTaskRequestParams(up), MetaWithTasksExtension());
    ASSERT_FALSE(up_resp.Contains("code"));
    EXPECT_EQ(up_resp["resultType"].GetString(), "complete");
    EXPECT_FALSE(up_resp.Contains("taskId"));
    EXPECT_FALSE(up_resp.Contains("status"));
    EXPECT_FALSE(up_resp.Contains("result"));
    EXPECT_FALSE(up_resp.Contains("error"));
    EXPECT_FALSE(up_resp.Contains("inputRequests"));

    auto done = WaitForTaskStatus(env.store, created.task_id, TaskStatus::Completed);
    ASSERT_TRUE(done.has_value());
    ASSERT_TRUE(done->result.has_value());
    auto tool_result = DeserializeCallToolResult(*done->result);
    ASSERT_FALSE(tool_result.content.empty());
    auto* text = std::get_if<TextContent>(&tool_result.content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "Deleted report.txt");

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, TasksUpdatePartialKeepsPendingKey) {
    auto env = MakeModernTaskEnv();

    auto multi_handler = [](const Ctx& ctx) {
        auto names = [&ctx](const char* key) -> const JsonValue* {
            auto* entry = ctx.Params().input_responses
                ? ctx.Params().input_responses->Find(key) : nullptr;
            auto* action = entry && entry->IsObject() ? entry->Find("action") : nullptr;
            if (!action || !action->IsString() || action->GetString() != "accept")
                return nullptr;
            auto* content = entry->Find("content");
            return (content && content->IsObject()) ? content->Find("name") : nullptr;
        };
        auto* first = names("first");
        auto* second = names("second");
        if (first && first->IsString() && second && second->IsString()) {
            CallToolResult done;
            done.content.push_back(TextContent{"text", "Confirmed both inputs: "
                + first->GetString() + ", " + second->GetString()});
            return done;
        }
        CallToolResult result;
        InputRequiredResult ir;
        ElicitRequestParams elicit_params;
        elicit_params.message = "What is your name?";
        ir.input_requests["first"] = MakeInputRequestForElicitation(elicit_params);
        ir.input_requests["second"] = MakeInputRequestForElicitation(elicit_params);
        result.input_required = std::move(ir);
        return result;
    };
    env.server->RegisterTool("multi_tool", MakeTaskToolOptions(),
        std::function<CallToolResult(const Ctx&)>(multi_handler));

    CallToolRequestParams params;
    params.name = "multi_tool";
    auto created_resp = SendBareRequest(env.client, methods::kCallTool,
        SerializeCallToolRequestParams(params), MetaWithTasksExtension());
    ASSERT_FALSE(created_resp.Contains("code"));
    auto created = DeserializeCreateTaskResult(created_resp);
    ASSERT_FALSE(created.task_id.empty());

    auto parked = WaitForTaskStatus(env.store, created.task_id, TaskStatus::InputRequired);
    ASSERT_TRUE(parked.has_value());
    ASSERT_TRUE(parked->input_requests.has_value());
    EXPECT_EQ(parked->input_requests->size(), size_t{2});

    UpdateTaskRequestParams partial;
    partial.task_id = created.task_id;
    JsonValue partial_responses(JsonValue::object_tag);
    JsonValue first_entry(JsonValue::object_tag);
    first_entry["action"] = JsonValue("accept");
    JsonValue first_content(JsonValue::object_tag);
    first_content["name"] = JsonValue("partial-1");
    first_entry["content"] = std::move(first_content);
    partial_responses["first"] = std::move(first_entry);
    partial.input_responses = std::move(partial_responses);
    auto partial_resp = SendBareRequest(env.client, methods::kUpdateTask,
        SerializeUpdateTaskRequestParams(partial), MetaWithTasksExtension());
    ASSERT_FALSE(partial_resp.Contains("code"));
    EXPECT_EQ(partial_resp["resultType"].GetString(), "complete");

    GetTaskRequestParams get_params;
    get_params.task_id = created.task_id;
    auto get_resp = SendBareRequest(env.client, methods::kGetTask,
        SerializeGetTaskRequestParams(get_params), MetaWithTasksExtension());
    ASSERT_FALSE(get_resp.Contains("code"));
    EXPECT_EQ(get_resp["status"].GetString(), "input_required");
    auto* requests = get_resp.Find("inputRequests");
    ASSERT_NE(requests, nullptr);
    ASSERT_TRUE(requests->IsObject());
    EXPECT_EQ(requests->GetObject().size(), size_t{1});
    EXPECT_NE(requests->Find("second"), nullptr);
    EXPECT_EQ(requests->Find("first"), nullptr);

    UpdateTaskRequestParams rest;
    rest.task_id = created.task_id;
    JsonValue rest_responses(JsonValue::object_tag);
    JsonValue second_entry(JsonValue::object_tag);
    second_entry["action"] = JsonValue("accept");
    JsonValue second_content(JsonValue::object_tag);
    second_content["name"] = JsonValue("partial-2");
    second_entry["content"] = std::move(second_content);
    rest_responses["second"] = std::move(second_entry);
    rest.input_responses = std::move(rest_responses);
    auto rest_resp = SendBareRequest(env.client, methods::kUpdateTask,
        SerializeUpdateTaskRequestParams(rest), MetaWithTasksExtension());
    ASSERT_FALSE(rest_resp.Contains("code"));

    auto done = WaitForTaskStatus(env.store, created.task_id, TaskStatus::Completed);
    ASSERT_TRUE(done.has_value());
    ASSERT_TRUE(done->result.has_value());
    auto tool_result = DeserializeCallToolResult(*done->result);
    ASSERT_FALSE(tool_result.content.empty());
    auto* text = std::get_if<TextContent>(&tool_result.content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "Confirmed both inputs: partial-1, partial-2");

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, TasksUpdateUnknownKeyTolerated) {
    auto env = MakeModernTaskEnv();

    env.server->RegisterTool("parked_tool", MakeTaskToolOptions(),
        [](const Ctx&) {
            CallToolResult result;
            InputRequiredResult ir;
            ElicitRequestParams elicit_params;
            elicit_params.message = "Confirm";
            ir.input_requests["confirm_delete"] =
                MakeInputRequestForElicitation(elicit_params);
            result.input_required = std::move(ir);
            return result;
        });

    CallToolRequestParams params;
    params.name = "parked_tool";
    auto created_resp = SendBareRequest(env.client, methods::kCallTool,
        SerializeCallToolRequestParams(params), MetaWithTasksExtension());
    ASSERT_FALSE(created_resp.Contains("code"));
    auto created = DeserializeCreateTaskResult(created_resp);
    ASSERT_FALSE(created.task_id.empty());
    ASSERT_TRUE(WaitForTaskStatus(
        env.store, created.task_id, TaskStatus::InputRequired).has_value());

    UpdateTaskRequestParams up;
    up.task_id = created.task_id;
    JsonValue responses(JsonValue::object_tag);
    JsonValue unknown(JsonValue::object_tag);
    unknown["ignored"] = JsonValue(true);
    responses["unknown-key"] = std::move(unknown);
    up.input_responses = std::move(responses);
    auto up_resp = SendBareRequest(env.client, methods::kUpdateTask,
        SerializeUpdateTaskRequestParams(up), MetaWithTasksExtension());
    ASSERT_FALSE(up_resp.Contains("code"));
    EXPECT_EQ(up_resp["resultType"].GetString(), "complete");

    GetTaskRequestParams get_params;
    get_params.task_id = created.task_id;
    auto get_resp = SendBareRequest(env.client, methods::kGetTask,
        SerializeGetTaskRequestParams(get_params), MetaWithTasksExtension());
    ASSERT_FALSE(get_resp.Contains("code"));
    EXPECT_EQ(get_resp["status"].GetString(), "input_required");
    auto* requests = get_resp.Find("inputRequests");
    ASSERT_NE(requests, nullptr);
    EXPECT_NE(requests->Find("confirm_delete"), nullptr);

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, RequiredTaskToolSyncFirstThenTask) {
    auto env = MakeModernTaskEnv();

    env.server->RegisterTool("greet_task", MakeRequiredTaskToolOptions(),
        [](const Ctx& ctx) {
            auto* entry = ctx.Params().input_responses
                ? ctx.Params().input_responses->Find("user_name") : nullptr;
            auto* action = entry && entry->IsObject() ? entry->Find("action") : nullptr;
            auto* content = entry && entry->IsObject() ? entry->Find("content") : nullptr;
            auto* name = content && content->IsObject() ? content->Find("name") : nullptr;
            if (action && action->IsString() && action->GetString() == "accept"
                && name && name->IsString()) {
                CallToolResult done;
                done.content.push_back(
                    TextContent{"text", "Hello, " + name->GetString() + "!"});
                return done;
            }
            CallToolResult result;
            InputRequiredResult ir;
            ElicitRequestParams elicit_params;
            elicit_params.message = "What is your name?";
            ir.input_requests["user_name"] =
                MakeInputRequestForElicitation(elicit_params);
            result.input_required = std::move(ir);
            return result;
        });

    CallToolRequestParams round_one;
    round_one.name = "greet_task";
    auto first_resp = SendBareRequest(env.client, methods::kCallTool,
        SerializeCallToolRequestParams(round_one), MetaWithTasksExtension());
    ASSERT_FALSE(first_resp.Contains("code"));
    EXPECT_EQ(first_resp["resultType"].GetString(), "input_required");
    EXPECT_FALSE(first_resp.Contains("taskId"));
    auto* first_requests = first_resp.Find("inputRequests");
    ASSERT_NE(first_requests, nullptr);
    ASSERT_TRUE(first_requests->IsObject());
    ASSERT_EQ(first_requests->GetObject().size(), size_t{1});
    std::string input_key = first_requests->GetObject().begin()->first;

    CallToolRequestParams round_two;
    round_two.name = "greet_task";
    JsonValue responses(JsonValue::object_tag);
    JsonValue entry(JsonValue::object_tag);
    entry["action"] = JsonValue("accept");
    JsonValue content(JsonValue::object_tag);
    content["name"] = JsonValue("Alice");
    entry["content"] = std::move(content);
    responses[input_key] = std::move(entry);
    round_two.input_responses = std::move(responses);
    auto second_resp = SendBareRequest(env.client, methods::kCallTool,
        SerializeCallToolRequestParams(round_two), MetaWithTasksExtension());
    ASSERT_FALSE(second_resp.Contains("code"));
    EXPECT_EQ(second_resp["resultType"].GetString(), "task");
    auto created = DeserializeCreateTaskResult(second_resp);
    ASSERT_FALSE(created.task_id.empty());
    EXPECT_FALSE(second_resp.Contains("requestState"));
    EXPECT_FALSE(second_resp.Contains("inputRequests"));

    auto done = WaitForTaskStatus(env.store, created.task_id, TaskStatus::Completed);
    ASSERT_TRUE(done.has_value());
    ASSERT_TRUE(done->result.has_value());
    auto tool_result = DeserializeCallToolResult(*done->result);
    ASSERT_FALSE(tool_result.content.empty());
    auto* text = std::get_if<TextContent>(&tool_result.content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "Hello, Alice!");

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, RequiredTaskToolSyncThrowMapsToFailed) {
    auto env = MakeModernTaskEnv();

    env.server->RegisterTool("sync_boom", MakeRequiredTaskToolOptions(),
        [](const Ctx&) -> CallToolResult {
            throw McpError(McpErrorCode::InternalError, "sync-boom");
        });

    CallToolRequestParams params;
    params.name = "sync_boom";
    auto created_resp = SendBareRequest(env.client, methods::kCallTool,
        SerializeCallToolRequestParams(params), MetaWithTasksExtension());
    ASSERT_FALSE(created_resp.Contains("code"));
    auto created = DeserializeCreateTaskResult(created_resp);
    ASSERT_FALSE(created.task_id.empty());

    auto failed = WaitForTaskStatus(env.store, created.task_id, TaskStatus::Failed);
    ASSERT_TRUE(failed.has_value());

    GetTaskRequestParams get_params;
    get_params.task_id = created.task_id;
    auto get_resp = SendBareRequest(env.client, methods::kGetTask,
        SerializeGetTaskRequestParams(get_params), MetaWithTasksExtension());
    ASSERT_FALSE(get_resp.Contains("code"));
    EXPECT_EQ(get_resp["status"].GetString(), "failed");
    auto* error = get_resp.Find("error");
    ASSERT_NE(error, nullptr);
    EXPECT_FALSE(get_resp.Contains("result"));

    CloseModernTaskEnv(env);
}

TEST(McpServerTest, MetaReachesHandler) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), sopts);

    std::promise<RequestMeta> captured;
    auto captured_future = captured.get_future();
    server->RegisterTool("meta_probe",
        ToolOptions{}.Description("meta probe"),
        std::function<CallToolResult(const Ctx&)>(
            [&captured](const Ctx& ctx) {
                if (ctx.Params().meta) {
                    captured.set_value(*ctx.Params().meta);
                } else {
                    captured.set_value(RequestMeta{});
                }
                return CallToolResult{};
            }));

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();

    RequestMeta meta;
    meta.protocol_version = std::string(kLatestProtocolVersion);
    meta.progress_token = ProgressToken{std::string("tok-123")};
    meta.log_level = LoggingLevel::Warning;
    ClientCapabilities caps;
    caps.sampling = SamplingCapability{};
    caps.roots = RootsCapability{};
    meta.client_capabilities = caps;
    meta.client_info = Implementation{"meta-client", "9.9"};
    meta.traceparent = std::string("00-abc-def-01");

    CallToolRequestParams params;
    params.name = "meta_probe";
    auto future = client->SendRequest(methods::kCallTool,
        SerializeCallToolRequestParams(params), meta,
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code"));

    ASSERT_EQ(captured_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    auto got = captured_future.get();
    ASSERT_TRUE(got.progress_token.has_value());
    EXPECT_EQ(std::get<std::string>(*got.progress_token), "tok-123");
    ASSERT_TRUE(got.log_level.has_value());
    EXPECT_EQ(*got.log_level, LoggingLevel::Warning);
    ASSERT_TRUE(got.client_capabilities.has_value());
    EXPECT_TRUE(got.client_capabilities->sampling.has_value());
    EXPECT_TRUE(got.client_capabilities->roots.has_value());
    ASSERT_TRUE(got.client_info.has_value());
    EXPECT_EQ(got.client_info->name, "meta-client");
    ASSERT_TRUE(got.traceparent.has_value());
    EXPECT_EQ(*got.traceparent, "00-abc-def-01");

    server->Close();
    client->Close();
}

namespace {

std::string WriteFileInputTemp(
    const std::string& name,
    const std::string& data,
    std::vector<std::filesystem::path>& tracked)
{
    auto path = std::filesystem::temp_directory_path() / name;
    tracked.push_back(path);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    FILE* f = std::fopen(path.string().c_str(), "wb");
    if (f) {
        if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
    }
    return std::string("file://") + path.generic_string();
}

struct FileInputEnv {
    std::unique_ptr<McpServer> server;
    std::shared_ptr<McpSessionHandler> client;
    std::shared_ptr<std::optional<JsonValue>> captured;
    std::vector<std::filesystem::path> files;
};

FileInputEnv MakeFileInputEnv(bool enabled) {
    FileInputEnv env;
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    sopts.enable_file_input = enabled;
    env.server = McpServer::Create(std::move(pair.server), sopts);
    env.captured = std::make_shared<std::optional<JsonValue>>();
    auto captured = env.captured;
    env.server->RegisterTool(McpServerTool::Create("file_probe",
        std::function<CallToolResult(const Ctx&)>(
            [captured](const Ctx& ctx) {
                if (ctx.Params().arguments) {
                    *captured = *ctx.Params().arguments;
                } else {
                    *captured = std::nullopt;
                }
                CallToolResult r;
                r.content.push_back(TextContent{"text", "probed"});
                return r;
            }),
        ToolOptions{}.Description("File input probe")));
    env.client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    env.client->Start();
    return env;
}

JsonValue SendFileInputToolCall(
    const std::shared_ptr<McpSessionHandler>& client, JsonValue args)
{
    CallToolRequestParams params;
    params.name = "file_probe";
    params.arguments = std::move(args);
    auto future = client->SendRequest(methods::kCallTool,
        SerializeCallToolRequestParams(params), {},
        std::chrono::milliseconds(2000));
    if (future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        ADD_FAILURE();
        return JsonValue(JsonValue::object_tag);
    }
    return future.get();
}

std::string FileInputErrorText(const CallToolResult& result) {
    std::string text;
    for (const auto& c : result.content) {
        if (auto* t = std::get_if<TextContent>(&c)) text += t->text;
    }
    return text;
}

void CloseFileInputEnv(FileInputEnv& env) {
    env.server->Close();
    env.client->Close();
    std::error_code ec;
    for (const auto& p : env.files) std::filesystem::remove(p, ec);
}

} // namespace

TEST(McpServerTest, FileInputRangeReadInlinesSlice) {
    auto env = MakeFileInputEnv(true);
    std::string uri = WriteFileInputTemp(
        "mcp_file_input_range.txt", "0123456789abcdef", env.files);
    env.server->RegisterResource("file-input-doc", "resource://file-input-doc",
        ResourceOptions{},
        [](const std::string& doc_uri) -> ReadResourceResult {
            TextResourceContents trc;
            trc.uri = doc_uri;
            trc.text = "HelloWorld";
            ReadResourceResult rr;
            rr.contents = {mcp::ResourceContents{trc}};
            return rr;
        });

    JsonValue args(JsonValue::object_tag);
    JsonValue file_ref(JsonValue::object_tag);
    file_ref["uri"] = JsonValue(uri);
    file_ref["offset"] = JsonValue(int64_t(4));
    file_ref["length"] = JsonValue(int64_t(4));
    args["file"] = std::move(file_ref);
    JsonValue doc_ref(JsonValue::object_tag);
    doc_ref["uri"] = JsonValue("resource://file-input-doc");
    doc_ref["offset"] = JsonValue(int64_t(5));
    doc_ref["length"] = JsonValue(int64_t(5));
    args["doc"] = std::move(doc_ref);

    auto resp = SendFileInputToolCall(env.client, std::move(args));
    ASSERT_FALSE(resp.Contains("code"));
    auto result = DeserializeCallToolResult(resp);
    EXPECT_FALSE(result.is_error);

    ASSERT_TRUE(env.captured->has_value());
    auto* file_arg = (*env.captured)->Find("file");
    ASSERT_NE(file_arg, nullptr);
    ASSERT_TRUE(file_arg->IsObject());
    auto* file_content = file_arg->Find("content");
    ASSERT_NE(file_content, nullptr);
    EXPECT_EQ(file_content->GetString(), "4567");
    auto* doc_arg = (*env.captured)->Find("doc");
    ASSERT_NE(doc_arg, nullptr);
    ASSERT_TRUE(doc_arg->IsObject());
    auto* doc_content = doc_arg->Find("content");
    ASSERT_NE(doc_content, nullptr);
    EXPECT_EQ(doc_content->GetString(), "World");

    CloseFileInputEnv(env);
}

TEST(McpServerTest, FileInputOutOfRangeReturnsIsError) {
    auto env = MakeFileInputEnv(true);
    std::string uri = WriteFileInputTemp(
        "mcp_file_input_range_oob.txt", "abc", env.files);

    JsonValue args(JsonValue::object_tag);
    JsonValue file_ref(JsonValue::object_tag);
    file_ref["uri"] = JsonValue(uri);
    file_ref["offset"] = JsonValue(int64_t(100));
    args["file"] = std::move(file_ref);

    auto resp = SendFileInputToolCall(env.client, std::move(args));
    ASSERT_FALSE(resp.Contains("code"));
    auto result = DeserializeCallToolResult(resp);
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(FileInputErrorText(result).find("out of bounds"),
              std::string::npos);
    EXPECT_FALSE(env.captured->has_value());

    CloseFileInputEnv(env);
}

TEST(McpServerTest, FileInputOutsideWhitelistRejected) {
    auto env = MakeFileInputEnv(true);
    auto temp_base = std::filesystem::temp_directory_path();
    if (temp_base.filename().empty()) temp_base = temp_base.parent_path();
    auto outside = temp_base.parent_path() /
        "mcp_file_input_outside_xyz" / "f.txt";
    std::string uri = std::string("file://") + outside.generic_string();

    JsonValue args(JsonValue::object_tag);
    JsonValue file_ref(JsonValue::object_tag);
    file_ref["uri"] = JsonValue(uri);
    args["file"] = std::move(file_ref);

    auto resp = SendFileInputToolCall(env.client, std::move(args));
    ASSERT_FALSE(resp.Contains("code"));
    auto result = DeserializeCallToolResult(resp);
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(FileInputErrorText(result).find("not allowed"),
              std::string::npos);
    EXPECT_FALSE(env.captured->has_value());

    CloseFileInputEnv(env);
}

TEST(McpServerTest, FileInputHttpsPassthrough) {
    auto env = MakeFileInputEnv(true);

    JsonValue args(JsonValue::object_tag);
    JsonValue link_ref(JsonValue::object_tag);
    link_ref["uri"] = JsonValue("https://example.com/data");
    args["link"] = std::move(link_ref);

    auto resp = SendFileInputToolCall(env.client, std::move(args));
    ASSERT_FALSE(resp.Contains("code"));
    auto result = DeserializeCallToolResult(resp);
    EXPECT_FALSE(result.is_error);

    ASSERT_TRUE(env.captured->has_value());
    auto* link_arg = (*env.captured)->Find("link");
    ASSERT_NE(link_arg, nullptr);
    ASSERT_TRUE(link_arg->IsObject());
    auto* link_uri = link_arg->Find("uri");
    ASSERT_NE(link_uri, nullptr);
    EXPECT_EQ(link_uri->GetString(), "https://example.com/data");
    EXPECT_FALSE(link_arg->Contains("content"));

    CloseFileInputEnv(env);
}

TEST(McpServerTest, FileInputDisabledByDefault) {
    auto env = MakeFileInputEnv(false);
    std::string uri = WriteFileInputTemp(
        "mcp_file_input_disabled.txt", "hello", env.files);

    JsonValue args(JsonValue::object_tag);
    JsonValue file_ref(JsonValue::object_tag);
    file_ref["uri"] = JsonValue(uri);
    args["file"] = std::move(file_ref);

    auto resp = SendFileInputToolCall(env.client, std::move(args));
    ASSERT_FALSE(resp.Contains("code"));
    auto result = DeserializeCallToolResult(resp);
    EXPECT_FALSE(result.is_error);

    ASSERT_TRUE(env.captured->has_value());
    auto* file_arg = (*env.captured)->Find("file");
    ASSERT_NE(file_arg, nullptr);
    ASSERT_TRUE(file_arg->IsObject());
    auto* file_uri = file_arg->Find("uri");
    ASSERT_NE(file_uri, nullptr);
    EXPECT_EQ(file_uri->GetString(), uri);
    EXPECT_FALSE(file_arg->Contains("content"));

    CloseFileInputEnv(env);
}

namespace {

struct TriggerEmitEnv {
    std::unique_ptr<McpServer> server;
    std::shared_ptr<McpSessionHandler> client;
};

TriggerEmitEnv MakeTriggerEmitEnv(bool enabled) {
    TriggerEmitEnv env;
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    sopts.enable_triggers_events = enabled;
    env.server = McpServer::Create(std::move(pair.server), sopts);
    env.client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    env.client->Start();
    return env;
}

bool SubscribeTriggerStream(TriggerEmitEnv& env, std::string_view subscription_id) {
    SubscriptionsListenRequestParams params;
    params.notifications.tools_list_changed = true;
    auto future = env.client->SendRequest(methods::kSubscribe,
        SerializeSubscriptionsListenRequestParams(params),
        MetaWithSubscriptionId(subscription_id),
        std::chrono::milliseconds(2000));
    if (future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) return false;
    return !future.get().Contains("code");
}

void CloseTriggerEmitEnv(TriggerEmitEnv& env) {
    env.server->Close();
    env.client->Close();
}

} // namespace

TEST(McpServerTest, TriggerEmitDeliversToListeningSubscriber) {
    auto env = MakeTriggerEmitEnv(true);
    env.server->RegisterTrigger("trig-1", {"deploy.finished"}, "");
    ASSERT_TRUE(SubscribeTriggerStream(env, "sub-trig-1"));

    std::promise<JsonRpcNotification> received;
    auto received_future = received.get_future();
    env.client->SetNotificationHandler(notifications::kTriggersEvent,
        [&received](const JsonRpcNotification& n) {
            received.set_value(n);
        });

    JsonValue data(JsonValue::object_tag);
    data["status"] = JsonValue("ok");
    env.server->EmitTriggerEvent("deploy.finished", std::move(data));

    ASSERT_EQ(received_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto notif = received_future.get();
    EXPECT_EQ(notif.method, std::string(notifications::kTriggersEvent));
    ASSERT_TRUE(notif.params.has_value());
    auto* type = notif.params->Find("type");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->GetString(), "deploy.finished");
    auto* payload = notif.params->Find("data");
    ASSERT_NE(payload, nullptr);
    auto* status = payload->Find("status");
    ASSERT_NE(status, nullptr);
    EXPECT_EQ(status->GetString(), "ok");
    ASSERT_TRUE(notif.meta.has_value());
    auto* sid = notif.meta->Find("io.modelcontextprotocol/subscriptionId");
    ASSERT_NE(sid, nullptr);
    EXPECT_EQ(sid->GetString(), "sub-trig-1");

    CloseTriggerEmitEnv(env);
}

TEST(McpServerTest, TriggerEmitDropsWithoutSubscription) {
    auto env = MakeTriggerEmitEnv(true);
    env.server->RegisterTrigger("trig-1", {"deploy.finished"}, "");

    std::promise<JsonRpcNotification> received;
    auto received_future = received.get_future();
    env.client->SetNotificationHandler(notifications::kTriggersEvent,
        [&received](const JsonRpcNotification& n) {
            received.set_value(n);
        });

    JsonValue data(JsonValue::object_tag);
    data["status"] = JsonValue("ok");
    env.server->EmitTriggerEvent("deploy.finished", std::move(data));

    EXPECT_EQ(received_future.wait_for(std::chrono::milliseconds(300)),
              std::future_status::timeout);

    CloseTriggerEmitEnv(env);
}

TEST(McpServerTest, TriggerEmitDisabledByDefault) {
    auto env = MakeTriggerEmitEnv(false);
    env.server->RegisterTrigger("trig-1", {"deploy.finished"}, "");
    ASSERT_TRUE(SubscribeTriggerStream(env, "sub-trig-off"));

    std::promise<JsonRpcNotification> received;
    auto received_future = received.get_future();
    env.client->SetNotificationHandler(notifications::kTriggersEvent,
        [&received](const JsonRpcNotification& n) {
            received.set_value(n);
        });

    JsonValue data(JsonValue::object_tag);
    data["status"] = JsonValue("ok");
    env.server->EmitTriggerEvent("deploy.finished", std::move(data));

    EXPECT_EQ(received_future.wait_for(std::chrono::milliseconds(300)),
              std::future_status::timeout);

    CloseTriggerEmitEnv(env);
}

TEST(McpServerTest, ListChangedDropsWithoutSubscription) {
    auto ctx = MakeModernServerWithClient();
    NegotiateModernVersion(ctx.server);

    std::mutex observed_mutex;
    std::vector<std::string> observed;
    ctx.client->SetNotificationHandler(notifications::kToolListChanged,
        [&observed_mutex, &observed](const JsonRpcNotification& n) {
            std::lock_guard<std::mutex> lock(observed_mutex);
            observed.push_back(n.method);
        });
    ctx.client->SetNotificationHandler(notifications::kPromptListChanged,
        [&observed_mutex, &observed](const JsonRpcNotification& n) {
            std::lock_guard<std::mutex> lock(observed_mutex);
            observed.push_back(n.method);
        });

    ctx.server->SendToolListChanged();
    ctx.server->SendPromptListChanged();

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::lock_guard<std::mutex> lock(observed_mutex);
    EXPECT_TRUE(observed.empty());

    ctx.server->Close();
    ctx.client->Close();
}

namespace {

struct AppsCardEnv {
    std::unique_ptr<McpServer> server;
    std::shared_ptr<McpSessionHandler> client;
};

AppsCardEnv MakeAppsCardEnv(bool apps, bool card,
    std::optional<ext::ServerCard> explicit_card = std::nullopt) {
    AppsCardEnv env;
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    sopts.enable_apps_extension = apps;
    sopts.enable_server_card = card;
    if (explicit_card) sopts.server_card = std::move(*explicit_card);
    env.server = McpServer::Create(std::move(pair.server), sopts);
    env.server->RegisterResource("app-view", "ui://app/view",
        ResourceOptions{}.MimeType("text/html;profile=mcp-app"),
        [](const std::string& uri) -> ReadResourceResult {
            TextResourceContents trc;
            trc.uri = uri;
            trc.text = "<html></html>";
            trc.mime_type = "text/html;profile=mcp-app";
            ReadResourceResult rr;
            rr.contents = {mcp::ResourceContents{trc}};
            return rr;
        });
    env.client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    env.client->Start();
    return env;
}

void CloseAppsCardEnv(AppsCardEnv& env) {
    env.server->Close();
    env.client->Close();
}

bool CapabilityDeclares(const ServerCapabilities& caps, std::string_view id) {
    if (!caps.extensions) return false;
    return caps.extensions->find(std::string(id)) != caps.extensions->end();
}

JsonValue ReadUiView(const std::shared_ptr<McpSessionHandler>& client) {
    ResourceRequestParams params;
    params.uri = "ui://app/view";
    return SendBareRequest(client, methods::kReadResource,
        SerializeResourceRequestParams(params));
}

ext::ServerCard MakeExplicitCard() {
    ext::ServerCard card;
    card.name = "custom-card";
    card.version = "9.9.9";
    ext::ServerCardEndpoint endpoint;
    endpoint.url = "https://example.com/mcp";
    card.endpoints.push_back(std::move(endpoint));
    card.supported_protocol_versions = {std::string(kLatestProtocolVersion)};
    return card;
}

void ExpectServerCardDisabled(McpServer& server) {
    try {
        server.GetServerCard();
        EXPECT_TRUE(false);
    } catch (const McpError& e) {
        EXPECT_EQ(e.Code(), McpErrorCode::HandlerError);
    }
}

} // namespace

TEST(McpServerTest, AppsAndServerCardGatesMatrix) {
    {
        auto env = MakeAppsCardEnv(false, false);
        EXPECT_FALSE(CapabilityDeclares(
            env.server->GetCapabilities(), ext::kAppsExtensionId));
        EXPECT_FALSE(CapabilityDeclares(
            env.server->GetCapabilities(), ext::kServerCardExtensionId));
        auto resp = ReadUiView(env.client);
        ASSERT_TRUE(resp.Contains("code"));
        EXPECT_EQ(resp["code"].GetInt(),
                  static_cast<int64_t>(McpErrorCode::HandlerError));
        ExpectServerCardDisabled(*env.server);
        CloseAppsCardEnv(env);
    }
    {
        auto env = MakeAppsCardEnv(true, false);
        EXPECT_TRUE(CapabilityDeclares(
            env.server->GetCapabilities(), ext::kAppsExtensionId));
        EXPECT_FALSE(CapabilityDeclares(
            env.server->GetCapabilities(), ext::kServerCardExtensionId));
        auto resp = ReadUiView(env.client);
        ASSERT_FALSE(resp.Contains("code"));
        auto* contents = resp.Find("contents");
        ASSERT_NE(contents, nullptr);
        ASSERT_TRUE(contents->IsArray());
        ASSERT_EQ(contents->Size(), 1u);
        auto* text = (*contents)[0].Find("text");
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(text->GetString(), "<html></html>");
        ExpectServerCardDisabled(*env.server);
        CloseAppsCardEnv(env);
    }
    {
        auto env = MakeAppsCardEnv(false, true, MakeExplicitCard());
        EXPECT_FALSE(CapabilityDeclares(
            env.server->GetCapabilities(), ext::kAppsExtensionId));
        EXPECT_TRUE(CapabilityDeclares(
            env.server->GetCapabilities(), ext::kServerCardExtensionId));
        auto resp = ReadUiView(env.client);
        ASSERT_TRUE(resp.Contains("code"));
        EXPECT_EQ(resp["code"].GetInt(),
                  static_cast<int64_t>(McpErrorCode::HandlerError));
        auto card = env.server->GetServerCard();
        EXPECT_EQ(card.name, "custom-card");
        EXPECT_EQ(card.version, "9.9.9");
        ASSERT_EQ(card.endpoints.size(), 1u);
        EXPECT_EQ(card.endpoints[0].url, "https://example.com/mcp");
        CloseAppsCardEnv(env);
    }
    {
        auto env = MakeAppsCardEnv(true, true);
        EXPECT_TRUE(CapabilityDeclares(
            env.server->GetCapabilities(), ext::kAppsExtensionId));
        EXPECT_TRUE(CapabilityDeclares(
            env.server->GetCapabilities(), ext::kServerCardExtensionId));
        auto resp = ReadUiView(env.client);
        ASSERT_FALSE(resp.Contains("code"));
        auto card = env.server->GetServerCard();
        EXPECT_EQ(card.name, "test-server");
        EXPECT_EQ(card.version, "1.0.0");
        EXPECT_FALSE(card.endpoints.empty());
        EXPECT_FALSE(card.supported_protocol_versions.empty());
        CloseAppsCardEnv(env);
    }
}

TEST(McpServerTest, TasksRequireExplicitExtensionFlag) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    auto store_path = MakeTaskStorePath();
    std::error_code ec;
    std::filesystem::remove(store_path, ec);
    auto store = std::make_shared<FileTaskStore>(store_path);
    sopts.task_store = store;
    auto server = McpServer::Create(std::move(pair.server), sopts);
    EXPECT_FALSE(CapabilityDeclares(
        server->GetCapabilities(), std::string_view(kTasksExtensionId)));

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();

    GetTaskRequestParams get_params;
    get_params.task_id = "explicit-gate";
    auto resp = SendBareRequest(client, methods::kGetTask,
        SerializeGetTaskRequestParams(get_params), MetaWithTasksExtension());
    ASSERT_TRUE(resp.Contains("code"));
    EXPECT_EQ(resp["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::MethodNotFound));
    ASSERT_TRUE(resp.Contains("message"));
    EXPECT_EQ(resp["message"].GetString(), "tasks extension is disabled");

    server->Close();
    client->Close();
    std::filesystem::remove(store_path, ec);
}

namespace {

struct SharedStoreSession {
    std::unique_ptr<McpServer> server;
    std::shared_ptr<McpSessionHandler> client;
};

SharedStoreSession MakeSharedStoreSession(
    const std::shared_ptr<FileTaskStore>& store)
{
    SharedStoreSession session;
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLegacyProtocolVersion);
    sopts.task_store = store;
    sopts.enable_tasks_extension = true;
    session.server = McpServer::Create(std::move(pair.server), sopts);
    session.client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    session.client->Start();
    return session;
}

} // namespace

TEST(McpServerTest, TerminalTaskCannotBeRegressedByUpdateOrCancel) {
    auto env = MakeTaskEraEnv();
    PerformLegacyHandshake(env.client);
    env.store->CreateTask("task-terminal");

    UpdateTaskRequestParams update_params;
    update_params.task_id = "task-terminal";
    JsonValue result(JsonValue::object_tag);
    result["answer"] = JsonValue(42);
    update_params.result = result;
    auto ok_resp = SendBareRequest(env.client, methods::kUpdateTask,
        SerializeUpdateTaskRequestParams(update_params));
    ASSERT_FALSE(ok_resp.Contains("code"));

    auto completed = env.store->GetTask("task-terminal");
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, TaskStatus::Completed);

    UpdateTaskRequestParams regress_params;
    regress_params.task_id = "task-terminal";
    auto regress_resp = SendBareRequest(env.client, methods::kUpdateTask,
        SerializeUpdateTaskRequestParams(regress_params));
    ASSERT_TRUE(regress_resp.Contains("code"));
    EXPECT_EQ(regress_resp["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::InvalidParams));

    CancelTaskRequestParams cancel_params;
    cancel_params.task_id = "task-terminal";
    auto cancel_resp = SendBareRequest(env.client, methods::kCancelTask,
        SerializeCancelTaskRequestParams(cancel_params));
    ASSERT_FALSE(cancel_resp.Contains("code"));

    auto after = env.store->GetTask("task-terminal");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->status, TaskStatus::Completed);
    ASSERT_TRUE(after->result.has_value());
    EXPECT_EQ(after->result->At("answer").GetInt(), 42);

    CloseTaskEraEnv(env);
}

TEST(McpServerTest, CrossSessionTaskAccessIsRejected) {
    auto store_path = MakeTaskStorePath();
    std::error_code ec;
    std::filesystem::remove(store_path, ec);
    auto store = std::make_shared<FileTaskStore>(store_path);

    auto owner = MakeSharedStoreSession(store);
    auto intruder = MakeSharedStoreSession(store);
    PerformLegacyHandshake(owner.client);
    PerformLegacyHandshake(intruder.client);

    EXPECT_NE(owner.server->SessionId(), intruder.server->SessionId());
    store->CreateTask("task-owned", owner.server->SessionId(), 60000);

    GetTaskRequestParams get_params;
    get_params.task_id = "task-owned";
    auto intruder_get = SendBareRequest(intruder.client, methods::kGetTask,
        SerializeGetTaskRequestParams(get_params));
    ASSERT_TRUE(intruder_get.Contains("code"));
    EXPECT_EQ(intruder_get["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::InvalidParams));

    CancelTaskRequestParams cancel_params;
    cancel_params.task_id = "task-owned";
    auto intruder_cancel = SendBareRequest(intruder.client, methods::kCancelTask,
        SerializeCancelTaskRequestParams(cancel_params));
    ASSERT_TRUE(intruder_cancel.Contains("code"));
    EXPECT_EQ(intruder_cancel["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::InvalidParams));

    UpdateTaskRequestParams update_params;
    update_params.task_id = "task-owned";
    JsonValue payload(JsonValue::object_tag);
    payload["answer"] = JsonValue(1);
    update_params.result = payload;
    auto intruder_update = SendBareRequest(intruder.client, methods::kUpdateTask,
        SerializeUpdateTaskRequestParams(update_params));
    ASSERT_TRUE(intruder_update.Contains("code"));
    EXPECT_EQ(intruder_update["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::InvalidParams));

    auto untouched = store->GetTask("task-owned");
    ASSERT_TRUE(untouched.has_value());
    EXPECT_EQ(untouched->status, TaskStatus::Pending);
    EXPECT_FALSE(untouched->result.has_value());

    auto owner_get = SendBareRequest(owner.client, methods::kGetTask,
        SerializeGetTaskRequestParams(get_params));
    EXPECT_FALSE(owner_get.Contains("code"));

    owner.server->Close();
    owner.client->Close();
    intruder.server->Close();
    intruder.client->Close();
    std::filesystem::remove(store_path, ec);
}

TEST(McpServerTest, TaskIdentifiersAreSessionScopedAndNotSequential) {
    auto env = MakeTaskEraEnv();
    PerformLegacyHandshake(env.client);
    env.server->RegisterTool("task_tool", MakeTaskToolOptions(),
        [](const Ctx&) {
            CallToolResult r;
            r.content.push_back(TextContent{"text", "done"});
            return r;
        });

    std::vector<std::string> task_ids;
    for (int i = 0; i < 2; ++i) {
        CallToolRequestParams params;
        params.name = "task_tool";
        auto resp = SendBareRequest(env.client, methods::kCallTool,
            SerializeCallToolRequestParams(params));
        ASSERT_FALSE(resp.Contains("code"));
        auto created = DeserializeCreateTaskResult(resp);
        task_ids.push_back(created.task_id);
    }

    ASSERT_EQ(task_ids.size(), size_t{2});
    EXPECT_NE(task_ids[0], task_ids[1]);
    for (const auto& task_id : task_ids) {
        EXPECT_EQ(task_id.rfind("task-", 0), 0u);
        EXPECT_NE(task_id, "task-1");
        EXPECT_NE(task_id, "task-2");
        EXPECT_GT(task_id.size(), size_t{20});
        EXPECT_NE(task_id.find(env.server->SessionId()), std::string::npos);
    }

    auto stored = env.store->GetTask(task_ids.front());
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->owner, env.server->SessionId());
    ASSERT_TRUE(stored->ttl_ms.has_value());
    EXPECT_EQ(*stored->ttl_ms, 300000);

    CloseTaskEraEnv(env);
}

TEST(McpServerTest, ExpiredTasksAreReclaimedWhenNewTaskIsCreated) {
    auto env = MakeTaskEraEnv();
    PerformLegacyHandshake(env.client);
    env.server->RegisterTool("task_tool", MakeTaskToolOptions(),
        [](const Ctx&) {
            CallToolResult r;
            r.content.push_back(TextContent{"text", "done"});
            return r;
        });

    env.store->CreateTask("task-stale", env.server->SessionId(), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_TRUE(env.store->GetTask("task-stale").has_value());

    CallToolRequestParams params;
    params.name = "task_tool";
    auto resp = SendBareRequest(env.client, methods::kCallTool,
        SerializeCallToolRequestParams(params));
    ASSERT_FALSE(resp.Contains("code"));

    EXPECT_FALSE(env.store->GetTask("task-stale").has_value());
    CloseTaskEraEnv(env);
}

TEST(McpServerTest, ResourceHandlerMayRegisterResourceWithoutDeadlock) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), sopts);

    MCP_RUN_WITH_TIMEOUT([&]() {
        server->RegisterResource("dynamic", "resource://dynamic",
            ResourceOptions{},
            [&](const std::string& uri) -> ReadResourceResult {
                server->RegisterResource("dynamic-extra", "resource://dynamic-extra",
                    ResourceOptions{},
                    [](const std::string& extra_uri) -> ReadResourceResult {
                        TextResourceContents trc;
                        trc.uri = extra_uri;
                        trc.text = "extra";
                        ReadResourceResult rr;
                        rr.contents = {mcp::ResourceContents{trc}};
                        return rr;
                    });
                TextResourceContents trc;
                trc.uri = uri;
                trc.text = "dynamic content";
                ReadResourceResult rr;
                rr.contents = {mcp::ResourceContents{trc}};
                return rr;
            });

        auto client = std::make_shared<McpSessionHandler>(
            std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
        client->Start();
        InitializeRequestParams iparams;
        iparams.protocol_version = std::string(kLatestProtocolVersion);
        iparams.client_info = Implementation{"test-client", "1.0"};
        auto init_future = client->SendRequest(methods::kInitialize,
            SerializeInitializeRequestParams(iparams), {},
            std::chrono::milliseconds(2000));
        ASSERT_EQ(init_future.wait_for(std::chrono::seconds(3)),
                  std::future_status::ready);
        (void)init_future.get();
        client->SendNotification(notifications::kInitialized);

        ReadResourceRequestParams params;
        params.uri = "resource://dynamic";
        auto resp = SendBareRequest(client, methods::kReadResource,
            SerializeResourceRequestParams(params));
        ASSERT_FALSE(resp.Contains("code"));
        auto read_result = DeserializeReadResourceResult(resp);
        ASSERT_EQ(read_result.contents.size(), size_t{1});

        client->Close();
    });

    server->Close();
}

TEST(McpServerTest, DuplicateRegistrationThrowsInvalidParams) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server));

    auto make_tool = [](const char* name) {
        return McpServerTool::Create(name,
            std::function<CallToolResult(const Ctx&)>(
                [](const Ctx&) { return CallToolResult{}; }),
            ToolOptions{});
    };

    server->RegisterTool(make_tool("dup_tool"));
    EXPECT_THROW(server->RegisterTool(make_tool("dup_tool")), McpError);
    try {
        server->RegisterTool(make_tool("dup_tool"));
        FAIL();
    } catch (const McpError& e) {
        EXPECT_EQ(e.Code(), McpErrorCode::InvalidParams);
    }

    server->RegisterResource("dup_res", "resource://dup",
        ResourceOptions{},
        [](const std::string&) -> ReadResourceResult { return ReadResourceResult{}; });
    EXPECT_THROW(server->RegisterResource("dup_res", "resource://other",
        ResourceOptions{},
        [](const std::string&) -> ReadResourceResult { return ReadResourceResult{}; }),
        McpError);
    EXPECT_THROW(server->RegisterResource("other_res", "resource://dup",
        ResourceOptions{},
        [](const std::string&) -> ReadResourceResult { return ReadResourceResult{}; }),
        McpError);
    EXPECT_THROW(server->RegisterResource("empty_res", "resource://empty",
        ResourceOptions{}, std::function<ReadResourceResult(const std::string&)>()),
        McpError);

    server->RegisterResourceTemplate("dup_tpl", "resource://tpl/{id}",
        ResourceOptions{},
        [](const std::string&, const std::map<std::string, std::string>&)
            -> ReadResourceResult { return ReadResourceResult{}; });
    EXPECT_THROW(server->RegisterResourceTemplate("dup_tpl", "resource://tpl2/{id}",
        ResourceOptions{},
        [](const std::string&, const std::map<std::string, std::string>&)
            -> ReadResourceResult { return ReadResourceResult{}; }),
        McpError);
    EXPECT_THROW(server->RegisterResourceTemplate("empty_tpl", "resource://tpl3/{id}",
        ResourceOptions{},
        std::function<ReadResourceResult(const std::string&,
            const std::map<std::string, std::string>&)>()),
        McpError);

    server->RegisterPrompt("dup_prompt", PromptOptions{},
        [](const std::string&, const std::optional<JsonValue>&) -> GetPromptResult {
            return GetPromptResult{};
        });
    EXPECT_THROW(server->RegisterPrompt("dup_prompt", PromptOptions{},
        [](const std::string&, const std::optional<JsonValue>&) -> GetPromptResult {
            return GetPromptResult{};
        }), McpError);
    EXPECT_THROW(server->RegisterPrompt("empty_prompt", PromptOptions{},
        std::function<GetPromptResult(const std::string&, const std::optional<JsonValue>&)>()),
        McpError);

    Skill skill;
    skill.uri = "skill://code-review/SKILL.md";
    server->RegisterSkill(skill);
    EXPECT_THROW(server->RegisterSkill(skill), McpError);

    EXPECT_THROW(server->SetCompletionHandler(McpServer::CompletionHandler{}), McpError);
    try {
        server->SetCompletionHandler(McpServer::CompletionHandler{});
        FAIL();
    } catch (const McpError& e) {
        EXPECT_EQ(e.Code(), McpErrorCode::InvalidParams);
    }

    server->Close();
}

TEST(McpServerTest, ModernElicitRoutesThroughMrtrInputRoundTrip) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), sopts);

    server->RegisterTool(McpServerTool::Create("asker",
        std::function<CallToolResult(const Ctx&)>(
            [&](const Ctx& ctx) -> CallToolResult {
                if (ctx.Params().input_responses) {
                    auto* elicit = ctx.Params().input_responses->Find("elicitation");
                    auto* content = elicit ? elicit->Find("content") : nullptr;
                    auto* name = content ? content->Find("user_name") : nullptr;
                    if (name && name->IsString()) {
                        CallToolResult done;
                        done.content.push_back(
                            TextContent{"text", "Hello, " + name->GetString() + "!"});
                        return done;
                    }
                }
                ElicitRequestParams elicit_params;
                elicit_params.message = "What is your name?";
                JsonValue schema(JsonValue::object_tag);
                schema["type"] = JsonValue("object");
                JsonValue properties(JsonValue::object_tag);
                JsonValue field(JsonValue::object_tag);
                field["type"] = JsonValue("string");
                properties["user_name"] = std::move(field);
                schema["properties"] = std::move(properties);
                elicit_params.requested_schema = std::move(schema);
                auto future = server->Elicit(elicit_params);
                auto result = future.get();
                CallToolResult out;
                out.content.push_back(TextContent{"text", result.action});
                return out;
            }),
        ToolOptions{}.Description("modern elicit fixture")));

    ClientOptions cops;
    cops.connect_mode = ConnectMode::Pin;
    cops.pin_protocol_version = std::string(kLatestProtocolVersion);
    cops.input_required_config = ClientOptions::InputRequiredConfig{};
    auto client = McpClient::Create(std::move(pair.client), cops);
    client->SetElicitationHandler([](const ElicitRequestParams&) {
        ElicitResult result;
        result.action = "accept";
        JsonValue content(JsonValue::object_tag);
        content["user_name"] = JsonValue("Alice");
        result.content = std::move(content);
        return result;
    });

    auto done = client->CallTool("asker", JsonValue(JsonValue::object_tag));
    EXPECT_FALSE(done.is_error);
    ASSERT_EQ(done.content.size(), size_t{1});
    auto* text = std::get_if<TextContent>(&done.content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "Hello, Alice!");

    client->Close();
    server->Close();
}

namespace {

struct CacheHintTestEnv {
    std::unique_ptr<McpServer> server;
    std::shared_ptr<McpSessionHandler> client;
};

CacheHintTestEnv MakeCacheHintTestEnv(
    std::string_view version,
    std::optional<std::map<std::string, CacheHint, std::less<>>> hints = std::nullopt) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"cache-test-server", "1.0.0"};
    sopts.protocol_version = std::string(version);
    sopts.cache_hints = std::move(hints);
    auto server = McpServer::Create(std::move(pair.server), sopts);
    server->RegisterTool("cache_tool", ToolOptions{},
        [](const Ctx&) { return CallToolResult{}; });
    server->RegisterResource("cache_resource", "test://cache/static",
        ResourceOptions{},
        [](const std::string& uri) -> ReadResourceResult {
            TextResourceContents contents;
            contents.uri = uri;
            contents.text = "cached";
            ReadResourceResult result;
            result.contents.push_back(ResourceContents{std::move(contents)});
            return result;
        });
    server->RegisterResourceTemplate("cache_template", "test://cache/{id}",
        ResourceOptions{},
        [](const std::string& uri,
           const std::map<std::string, std::string>&) -> ReadResourceResult {
            TextResourceContents contents;
            contents.uri = uri;
            contents.text = "template";
            ReadResourceResult result;
            result.contents.push_back(ResourceContents{std::move(contents)});
            return result;
        });
    server->RegisterPrompt("cache_prompt", PromptOptions{},
        [](const std::string&, const std::optional<JsonValue>&) -> GetPromptResult {
            return GetPromptResult{};
        });
    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(version));
    client->Start();
    return CacheHintTestEnv{std::move(server), std::move(client)};
}

JsonValue SendCacheProbe(const std::shared_ptr<McpSessionHandler>& client,
                         std::string_view method) {
    JsonValue params(JsonValue::object_tag);
    if (method == methods::kDiscover) {
        params = SerializeDiscoverRequestParams(DiscoverRequestParams{});
    } else if (method == methods::kReadResource) {
        ResourceRequestParams read_params;
        read_params.uri = "test://cache/static";
        params = SerializeResourceRequestParams(read_params);
    } else {
        params = SerializePaginatedRequestParams(PaginatedRequestParams{});
    }
    auto future = client->SendRequest(method, std::move(params), {},
        std::chrono::milliseconds(2000));
    if (future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        ADD_FAILURE() << "timed out waiting for " << std::string(method);
        return JsonValue(JsonValue::object_tag);
    }
    return future.get();
}

void ExpectInvalidCacheHint(const std::shared_ptr<McpSessionHandler>& client,
                            std::string_view method, std::string_view needle) {
    auto resp = SendCacheProbe(client, method);
    ASSERT_TRUE(resp.Contains("code")) << std::string(method);
    EXPECT_EQ(resp["code"].GetInt(), static_cast<int64_t>(McpErrorCode::InvalidParams))
        << std::string(method);
    auto* message = resp.Find("message");
    ASSERT_NE(message, nullptr) << std::string(method);
    EXPECT_NE(message->GetString().find(needle), std::string::npos)
        << message->GetString();
}

void CloseCacheHintTestEnv(CacheHintTestEnv& env) {
    env.server->Close();
    env.client->Close();
}

} // namespace

TEST(McpServerTest, ModernDefaultServerEmitsCacheHintOnSixResults) {
    auto env = MakeCacheHintTestEnv(kLatestProtocolVersion);

    const std::string_view probes[] = {
        methods::kDiscover,
        methods::kListTools,
        methods::kListResources,
        methods::kListResourceTemplates,
        methods::kReadResource,
        methods::kListPrompts,
    };
    for (auto method : probes) {
        auto resp = SendCacheProbe(env.client, method);
        ASSERT_FALSE(resp.Contains("code")) << std::string(method);
        ASSERT_TRUE(resp.Contains("ttlMs")) << std::string(method);
        EXPECT_EQ(resp["ttlMs"].GetInt(), int64_t{0}) << std::string(method);
        ASSERT_TRUE(resp.Contains("cacheScope")) << std::string(method);
        EXPECT_EQ(resp["cacheScope"].GetString(), "private") << std::string(method);
        EXPECT_FALSE(resp.Contains("cacheHint")) << std::string(method);
    }

    CloseCacheHintTestEnv(env);
}

TEST(McpServerTest, LegacyDefaultServerKeepsExistingCacheFields) {
    auto env = MakeCacheHintTestEnv(kLegacyProtocolVersion);
    PerformLegacyHandshake(env.client);

    const std::string_view probes[] = {
        methods::kListTools,
        methods::kListResources,
        methods::kListResourceTemplates,
        methods::kReadResource,
        methods::kListPrompts,
    };
    for (auto method : probes) {
        auto resp = SendCacheProbe(env.client, method);
        ASSERT_FALSE(resp.Contains("code")) << std::string(method);
        ASSERT_TRUE(resp.Contains("ttlMs")) << std::string(method);
        EXPECT_EQ(resp["ttlMs"].GetInt(), int64_t{0}) << std::string(method);
        ASSERT_TRUE(resp.Contains("cacheScope")) << std::string(method);
        EXPECT_EQ(resp["cacheScope"].GetString(), "private") << std::string(method);
        EXPECT_FALSE(resp.Contains("cacheHint")) << std::string(method);
    }

    auto discover = SendCacheProbe(env.client, methods::kDiscover);
    ASSERT_TRUE(discover.Contains("code"));
    EXPECT_EQ(discover["code"].GetInt(),
        static_cast<int64_t>(McpErrorCode::MethodNotFound));

    CloseCacheHintTestEnv(env);
}

TEST(McpServerTest, ModernInvalidCacheHintReturnsInvalidParams) {
    CacheHint missing_scope;
    missing_scope.ttl_ms = int64_t{1000};
    CacheHint missing_ttl;
    missing_ttl.cache_scope = "public";
    CacheHint negative_ttl;
    negative_ttl.ttl_ms = int64_t{-1};
    negative_ttl.cache_scope = "private";
    CacheHint bad_scope;
    bad_scope.ttl_ms = int64_t{0};
    bad_scope.cache_scope = "shared";
    CacheHint negative_public;
    negative_public.ttl_ms = int64_t{-120};
    negative_public.cache_scope = "public";
    CacheHint empty_scope;
    empty_scope.ttl_ms = int64_t{0};
    empty_scope.cache_scope = "";

    auto env = MakeCacheHintTestEnv(kLatestProtocolVersion,
        std::map<std::string, CacheHint, std::less<>>{
            {"tools/list", missing_scope},
            {"resources/list", missing_ttl},
            {"resources/templates/list", negative_ttl},
            {"resources/read", bad_scope},
            {"prompts/list", negative_public},
            {"server/discover", empty_scope},
        });

    ExpectInvalidCacheHint(env.client, methods::kListTools, "cacheScope");
    ExpectInvalidCacheHint(env.client, methods::kListResources, "ttlMs");
    ExpectInvalidCacheHint(env.client, methods::kListResourceTemplates, "-1");
    ExpectInvalidCacheHint(env.client, methods::kReadResource, "shared");
    ExpectInvalidCacheHint(env.client, methods::kListPrompts, "-120");
    ExpectInvalidCacheHint(env.client, methods::kDiscover, "got \"\"");

    CloseCacheHintTestEnv(env);
}

TEST(McpServerTest, DiscoverRejectedWhenRequestDeclaresLegacyVersion) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), sopts);

    JsonRpcRequest req;
    req.id = RequestId{int64_t{4100}};
    req.method = std::string(methods::kDiscover);
    req.params = JsonValue(JsonValue::object_tag);
    JsonValue meta(JsonValue::object_tag);
    meta["io.modelcontextprotocol/protocolVersion"] =
        JsonValue(std::string(kLegacyProtocolVersion));
    req.meta = std::move(meta);
    pair.client->SendMessageAsync(JsonRpcMessage{std::move(req)});

    std::optional<ErrorData> error;
    MCP_RUN_WITH_TIMEOUT([&]() {
        pair.client->GetMessageChannel().AsyncReceive(
            [&](std::error_code ec, JsonRpcMessage msg) {
                if (ec) return;
                if (auto* r = std::get_if<JsonRpcErrorResponse>(&msg))
                    error = r->error;
            });
    });
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, McpErrorCode::MethodNotFound);

    server->Close();
}

TEST(McpServerTest, CompleteRequestCarriesContextArguments) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    sopts.protocol_version = std::string(kLatestProtocolVersion);
    auto server = McpServer::Create(std::move(pair.server), sopts);

    std::mutex received_mutex;
    std::map<std::string, std::string, std::less<>> received;
    server->SetCompletionHandler(
        [&](const CompleteRequestParams& params) -> CompleteResult {
            {
                std::lock_guard<std::mutex> lock(received_mutex);
                received = params.context_arguments;
            }
            CompleteResult result;
            JsonValue completion(JsonValue::object_tag);
            JsonValue values(JsonValue::array_tag);
            values.PushBack(JsonValue("c++"));
            completion["values"] = std::move(values);
            completion["total"] = JsonValue(int64_t{1});
            completion["hasMore"] = JsonValue(false);
            result.completion = std::move(completion);
            return result;
        });

    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();

    CompleteRequestParams params;
    params.ref = JsonValue::Parse(
        "{\"type\":\"ref/prompt\",\"name\":\"code_review\"}");
    params.argument_name = "framework";
    params.argument_value = "qt";
    params.context_arguments = {
        {"language", "cpp"},
        {"framework", "qt"},
    };

    auto future = client->SendRequest(methods::kComplete,
        SerializeCompleteRequestParams(params), {}, std::chrono::milliseconds(2000));
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto resp = future.get();
    ASSERT_FALSE(resp.Contains("code")) << resp.Dump();

    std::map<std::string, std::string, std::less<>> captured;
    {
        std::lock_guard<std::mutex> lock(received_mutex);
        captured = received;
    }
    EXPECT_EQ(captured, params.context_arguments);

    server->Close();
    client->Close();
}
