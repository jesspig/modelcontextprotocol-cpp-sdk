#include <mcp/client/McpClient.hpp>
#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/McpTypes.hpp>

#include <gtest/gtest.h>
#include <mcp/test/McpTimeout.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

using namespace mcp;

using Ctx = RequestContext<CallToolRequestParams>;

TEST(McpClientTest, CreateAndDestroy) {
    auto pair = InMemoryTransport::CreatePair();
    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    auto client = McpClient::Create(std::move(pair.client), opts);
    ASSERT_NE(client, nullptr);
    EXPECT_TRUE(client->IsModernProtocol());
    client->Close();
}

TEST(McpClientTest, InitialState) {
    auto pair = InMemoryTransport::CreatePair();
    ClientOptions opts;
    opts.client_info = Implementation{"my-client", "2.0"};
    opts.connect_mode = ConnectMode::Pin;

    auto client = McpClient::Create(std::move(pair.client), opts);
    EXPECT_TRUE(client->GetServerInfo().name.empty());
    client->Close();
}

TEST(McpClientTest, ClientOptionsDefaults) {
    ClientOptions opts;
    EXPECT_EQ(opts.connect_mode, ConnectMode::Auto);
    EXPECT_EQ(opts.client_info.name, "mcp-cpp-client");
    EXPECT_EQ(opts.initialization_timeout.count(), 60);
    EXPECT_EQ(opts.discover_probe_timeout.count(), 5);
}

TEST(McpClientTest, DeriveClientCapabilitiesNoHandlersReturnsNullopt) {
    ClientOptions opts;
    EXPECT_FALSE(DeriveClientCapabilities(opts).has_value());

    opts.capabilities = ClientCapabilities{};
    auto derived = DeriveClientCapabilities(opts);
    ASSERT_TRUE(derived.has_value());
    EXPECT_FALSE(derived->elicitation.has_value());
    EXPECT_FALSE(derived->sampling.has_value());
    EXPECT_FALSE(derived->roots.has_value());
}

TEST(McpClientTest, DeriveClientCapabilitiesDeclaresHandlers) {
    ClientOptions form_opts;
    form_opts.elicitation_handler = [](const ElicitRequestParams&) {
        return ElicitResult{};
    };
    auto form_caps = DeriveClientCapabilities(form_opts);
    ASSERT_TRUE(form_caps.has_value());
    ASSERT_TRUE(form_caps->elicitation.has_value());
    ASSERT_TRUE(form_caps->elicitation->form.has_value());
    EXPECT_TRUE(form_caps->elicitation->form->IsObject());
    EXPECT_TRUE(form_caps->elicitation->form->Empty());
    EXPECT_FALSE(form_caps->elicitation->url.has_value());

    ClientOptions url_opts;
    url_opts.url_elicitation_handler = [](const ElicitRequestParams&) {
        return ElicitResult{};
    };
    auto url_caps = DeriveClientCapabilities(url_opts);
    ASSERT_TRUE(url_caps.has_value());
    ASSERT_TRUE(url_caps->elicitation.has_value());
    ASSERT_TRUE(url_caps->elicitation->url.has_value());
    EXPECT_TRUE(url_caps->elicitation->url->IsObject());
    EXPECT_TRUE(url_caps->elicitation->url->Empty());
    EXPECT_FALSE(url_caps->elicitation->form.has_value());

    ClientOptions sampling_opts;
    sampling_opts.sampling_handler = [](const CreateMessageRequestParams&) {
        return CreateMessageResult{};
    };
    auto sampling_caps = DeriveClientCapabilities(sampling_opts);
    ASSERT_TRUE(sampling_caps.has_value());
    EXPECT_TRUE(sampling_caps->sampling.has_value());

    ClientOptions roots_opts;
    roots_opts.roots_handler = [](const ListRootsRequestParams&) {
        return ListRootsResult{};
    };
    auto roots_caps = DeriveClientCapabilities(roots_opts);
    ASSERT_TRUE(roots_caps.has_value());
    ASSERT_TRUE(roots_caps->roots.has_value());
    ASSERT_TRUE(roots_caps->roots->list_changed.has_value());
    EXPECT_TRUE(*roots_caps->roots->list_changed);
}

TEST(McpClientTest, DeriveClientCapabilitiesMergesExplicitCapabilities) {
    ClientOptions opts;
    ClientCapabilities explicit_caps;
    ElicitationCapability elicitation;
    elicitation.form = JsonValue(JsonValue::Object{{"applyDefaults", JsonValue(true)}});
    explicit_caps.elicitation = elicitation;
    RootsCapability roots;
    roots.list_changed = false;
    explicit_caps.roots = roots;
    explicit_caps.sampling = SamplingCapability{};
    explicit_caps.experimental = JsonValue(JsonValue::Object{{"feature", JsonValue(true)}});
    std::map<std::string, JsonValue> extensions;
    extensions.emplace("io.test/ext", JsonValue(true));
    explicit_caps.extensions = std::move(extensions);
    opts.capabilities = explicit_caps;

    opts.elicitation_handler = [](const ElicitRequestParams&) {
        return ElicitResult{};
    };
    opts.url_elicitation_handler = [](const ElicitRequestParams&) {
        return ElicitResult{};
    };
    opts.sampling_handler = [](const CreateMessageRequestParams&) {
        return CreateMessageResult{};
    };
    opts.roots_handler = [](const ListRootsRequestParams&) {
        return ListRootsResult{};
    };

    auto caps = DeriveClientCapabilities(opts);
    ASSERT_TRUE(caps.has_value());
    ASSERT_TRUE(caps->elicitation.has_value());
    ASSERT_TRUE(caps->elicitation->form.has_value());
    EXPECT_TRUE((*caps->elicitation->form)["applyDefaults"].GetBool());
    ASSERT_TRUE(caps->elicitation->url.has_value());
    EXPECT_TRUE(caps->elicitation->url->IsObject());
    EXPECT_TRUE(caps->elicitation->url->Empty());
    EXPECT_TRUE(caps->sampling.has_value());
    ASSERT_TRUE(caps->roots.has_value());
    ASSERT_TRUE(caps->roots->list_changed.has_value());
    EXPECT_FALSE(*caps->roots->list_changed);
    ASSERT_TRUE(caps->experimental.has_value());
    EXPECT_TRUE((*caps->experimental)["feature"].GetBool());
    ASSERT_TRUE(caps->extensions.has_value());
    EXPECT_EQ(caps->extensions->count("io.test/ext"), 1u);
}

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

TEST(McpClientTest, ClientOptionsElicitationHandlerDeclaredOnInitialize) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), sopts);

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Legacy;
    opts.initialization_timeout = std::chrono::seconds(5);
    opts.elicitation_handler = [](const ElicitRequestParams&) {
        return ElicitResult{};
    };
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto caps = server->GetClientCapabilities();
    ASSERT_NE(caps, nullptr);
    ASSERT_TRUE(caps->elicitation.has_value());
    ASSERT_TRUE(caps->elicitation->form.has_value());
    EXPECT_TRUE(caps->elicitation->form->IsObject());
    EXPECT_FALSE(caps->sampling.has_value());
    EXPECT_FALSE(caps->roots.has_value());

    client->Close();
    server->Close();
}

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

TEST(McpClientTest, AutoNegotiationFallsBackToInitialize) {
    auto pair = InMemoryTransport::CreatePair();

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

TEST(McpClientTest, AutoNegotiationCorrectsVersionOnSharedVersion) {
    auto pair = InMemoryTransport::CreatePair();

    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> discover_calls{0};
    server_handler->SetRequestHandler(methods::kDiscover,
        [&discover_calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            if (discover_calls.fetch_add(1) == 0) {
                JsonValue err(JsonValue::object_tag);
                err["code"] = JsonValue(static_cast<int64_t>(McpErrorCode::UnsupportedProtocolVersion));
                err["message"] = JsonValue("unsupported protocol version");
                JsonValue data(JsonValue::object_tag);
                JsonValue supported(JsonValue::array_tag);
                supported.PushBack(JsonValue("2025-11-25"));
                supported.PushBack(JsonValue("2026-07-28"));
                data["supported"] = std::move(supported);
                err["data"] = std::move(data);
                p.set_value(std::move(err));
                return;
            }
            JsonValue result(JsonValue::object_tag);
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            result["serverInfo"] = SerializeImplementation(Implementation{"modern-server", "1.0"});
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Auto;
    opts.discover_probe_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), opts);

    EXPECT_EQ(discover_calls.load(), 2);
    EXPECT_TRUE(client->IsModernProtocol());
    EXPECT_EQ(client->GetNegotiatedProtocolVersion(), std::string(kLatestProtocolVersion));
    EXPECT_EQ(client->GetServerInfo().name, "modern-server");
    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, AutoNegotiationFallsBackWhenOnlyLegacySupported) {
    auto pair = InMemoryTransport::CreatePair();

    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> discover_calls{0};
    server_handler->SetRequestHandler(methods::kDiscover,
        [&discover_calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            discover_calls.fetch_add(1);
            JsonValue err(JsonValue::object_tag);
            err["code"] = JsonValue(static_cast<int64_t>(McpErrorCode::UnsupportedProtocolVersion));
            err["message"] = JsonValue("unsupported protocol version");
            JsonValue data(JsonValue::object_tag);
            JsonValue supported(JsonValue::array_tag);
            supported.PushBack(JsonValue("2025-11-25"));
            data["supported"] = std::move(supported);
            err["data"] = std::move(data);
            p.set_value(std::move(err));
        });
    server_handler->SetRequestHandler(methods::kInitialize,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue result(JsonValue::object_tag);
            result["protocolVersion"] = JsonValue(std::string(kLegacyProtocolVersion));
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            result["serverInfo"] = SerializeImplementation(Implementation{"legacy-server", "1.0"});
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Auto;
    opts.discover_probe_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), opts);

    EXPECT_EQ(discover_calls.load(), 1);
    EXPECT_FALSE(client->IsModernProtocol());
    EXPECT_EQ(client->GetNegotiatedProtocolVersion(), std::string(kLegacyProtocolVersion));
    EXPECT_EQ(client->GetServerInfo().name, "legacy-server");
    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, AutoNegotiationAcceptsMissingServerInfoWithMetaServerInfo) {
    auto pair = InMemoryTransport::CreatePair();

    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    server_handler->SetRequestHandler(methods::kDiscover,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue result(JsonValue::object_tag);
            JsonValue supported(JsonValue::array_tag);
            supported.PushBack(JsonValue(std::string(kLatestProtocolVersion)));
            result["supportedVersions"] = std::move(supported);
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            JsonValue meta(JsonValue::object_tag);
            meta["io.modelcontextprotocol/serverInfo"] =
                SerializeImplementation(Implementation{"ts-server", "1.0"});
            result["_meta"] = std::move(meta);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Auto;
    opts.discover_probe_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), opts);

    EXPECT_TRUE(client->IsModernProtocol());
    EXPECT_EQ(client->GetNegotiatedProtocolVersion(), std::string(kLatestProtocolVersion));
    EXPECT_EQ(client->GetServerInfo().name, "ts-server");
    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, AutoNegotiationAcceptsMissingServerInfoEntirely) {
    auto pair = InMemoryTransport::CreatePair();

    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    server_handler->SetRequestHandler(methods::kDiscover,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue result(JsonValue::object_tag);
            JsonValue supported(JsonValue::array_tag);
            supported.PushBack(JsonValue(std::string(kLatestProtocolVersion)));
            result["supportedVersions"] = std::move(supported);
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Auto;
    opts.discover_probe_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), opts);

    EXPECT_TRUE(client->IsModernProtocol());
    EXPECT_EQ(client->GetNegotiatedProtocolVersion(), std::string(kLatestProtocolVersion));
    EXPECT_TRUE(client->GetServerInfo().name.empty());
    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, AutoNegotiationUsesSharedVersionFromSuccessfulDiscover) {
    auto pair = InMemoryTransport::CreatePair();

    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    server_handler->SetRequestHandler(methods::kDiscover,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue result(JsonValue::object_tag);
            JsonValue supported(JsonValue::array_tag);
            supported.PushBack(JsonValue(std::string(kLegacyProtocolVersion)));
            result["supportedVersions"] = std::move(supported);
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            result["serverInfo"] = SerializeImplementation(Implementation{"shared-server", "1.0"});
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Auto;
    opts.discover_probe_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), opts);

    EXPECT_TRUE(client->IsModernProtocol());
    EXPECT_EQ(client->GetNegotiatedProtocolVersion(), std::string(kLegacyProtocolVersion));
    EXPECT_EQ(client->GetServerInfo().name, "shared-server");
    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, AutoNegotiationFallsBackWhenSupportedVersionsEmpty) {
    auto pair = InMemoryTransport::CreatePair();

    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> discover_calls{0};
    server_handler->SetRequestHandler(methods::kDiscover,
        [&discover_calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            discover_calls.fetch_add(1);
            JsonValue result(JsonValue::object_tag);
            result["supportedVersions"] = JsonValue(JsonValue::array_tag);
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            result["serverInfo"] = SerializeImplementation(Implementation{"empty-server", "1.0"});
            p.set_value(std::move(result));
        });
    server_handler->SetRequestHandler(methods::kInitialize,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue result(JsonValue::object_tag);
            result["protocolVersion"] = JsonValue(std::string(kLegacyProtocolVersion));
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            result["serverInfo"] = SerializeImplementation(Implementation{"legacy-server", "1.0"});
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Auto;
    opts.discover_probe_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), opts);

    EXPECT_EQ(discover_calls.load(), 1);
    EXPECT_FALSE(client->IsModernProtocol());
    EXPECT_EQ(client->GetNegotiatedProtocolVersion(), std::string(kLegacyProtocolVersion));
    EXPECT_EQ(client->GetServerInfo().name, "legacy-server");
    client->Close();
    server_handler->Close();
}

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

TEST(McpClientTest, CreateAndClose) {
    auto pair = InMemoryTransport::CreatePair();
    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    auto client = McpClient::Create(std::move(pair.client), opts);
    ASSERT_NE(client, nullptr);
    client->Close();
}

TEST(McpClientTest, ListToolsAutoPaginates) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));

    server_handler->SetRequestHandler(methods::kListTools,
        [](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            std::string cursor;
            if (req.params && req.params->IsObject()) {
                if (auto* c = req.params->Find("cursor"); c && c->IsString())
                    cursor = c->GetString();
            }
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            if (cursor.empty()) {
                JsonValue t1(JsonValue::object_tag);
                t1["name"] = JsonValue("tool-1");
                t1["inputSchema"] = JsonValue(JsonValue::object_tag);
                arr.PushBack(std::move(t1));
                JsonValue t2(JsonValue::object_tag);
                t2["name"] = JsonValue("tool-2");
                t2["inputSchema"] = JsonValue(JsonValue::object_tag);
                arr.PushBack(std::move(t2));
                result["nextCursor"] = JsonValue("c1");
            } else {
                JsonValue t3(JsonValue::object_tag);
                t3["name"] = JsonValue("tool-3");
                t3["inputSchema"] = JsonValue(JsonValue::object_tag);
                arr.PushBack(std::move(t3));
            }
            result["tools"] = JsonValue(std::move(arr));
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto all = client->ListTools();
    ASSERT_EQ(all.tools.size(), 3u);
    EXPECT_EQ(all.tools[0].name, "tool-1");
    EXPECT_EQ(all.tools[2].name, "tool-3");

    auto page = client->ListTools("c1");
    ASSERT_EQ(page.tools.size(), 1u);
    EXPECT_EQ(page.tools[0].name, "tool-3");

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, MrtrMaxTotalTimeoutCapsRoundTimeout) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::shared_ptr<std::promise<JsonValue>> held;
    server_handler->SetRequestHandler(methods::kCallTool,
        [&held](const JsonRpcRequest&, std::promise<JsonValue> p) {
            held = std::make_shared<std::promise<JsonValue>>(std::move(p));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.input_required_config = ClientOptions::InputRequiredConfig{};
    opts.input_required_config->max_rounds = 0;
    opts.input_required_config->round_timeout = std::chrono::seconds(30);
    opts.input_required_config->max_total_timeout = std::chrono::seconds(1);
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto start = std::chrono::steady_clock::now();
    EXPECT_THROW(
        client->CallTool("echo", JsonValue(JsonValue::object_tag)),
        McpError);
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start);
    EXPECT_LT(elapsed.count(), 10);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, MrtrFulfillsSamplingEmbeddedRequest) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> round{0};
    server_handler->SetRequestHandler(methods::kCallTool,
        [&round](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (round.load() == 0) {
                round.store(1);
                CreateMessageRequestParams params;
                SamplingMessage msg;
                msg.role = "user";
                TextContent content;
                content.text = "hello";
                msg.content = std::move(content);
                params.messages.push_back(std::move(msg));
                params.max_tokens = 64;
                InputRequiredResult ir;
                ir.input_requests["message"] = MakeInputRequestForSampling(params);
                ir.request_state = "st-1";
                p.set_value(SerializeInputRequiredResult(ir));
                return;
            }
            round.store(2);
            auto* ir_resp = req.params ? req.params->Find("inputResponses") : nullptr;
            ASSERT_NE(ir_resp, nullptr);
            auto* sampling = ir_resp->Find("message");
            ASSERT_NE(sampling, nullptr);
            EXPECT_EQ((*sampling)["role"].GetString(), "assistant");
            JsonValue result(JsonValue::object_tag);
            result["structuredContent"] = JsonValue(JsonValue::object_tag);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.input_required_config = ClientOptions::InputRequiredConfig{};
    auto client = McpClient::Create(std::move(pair.client), opts);

    bool sampled = false;
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    client->SetSamplingHandler([&sampled](const CreateMessageRequestParams& params) {
        sampled = true;
        EXPECT_EQ(params.messages.size(), 1u);
        EXPECT_EQ(params.messages[0].role, "user");
        EXPECT_EQ(params.max_tokens, 64);
        CreateMessageResult r;
        r.role = "assistant";
        TextContent c;
        c.text = "hi";
        r.content = std::move(c);
        r.model = "test-model";
        return r;
    });
#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

    client->CallTool("echo", JsonValue(JsonValue::object_tag));
    EXPECT_TRUE(sampled);
    EXPECT_EQ(round.load(), 2);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, MrtrFulfillsRootsEmbeddedRequest) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> round{0};
    server_handler->SetRequestHandler(methods::kCallTool,
        [&round](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (round.load() == 0) {
                round.store(1);
                InputRequiredResult ir;
                ir.input_requests["paths"] = MakeInputRequestForRoots(ListRootsRequestParams{});
                ir.request_state = "st-roots";
                p.set_value(SerializeInputRequiredResult(ir));
                return;
            }
            round.store(2);
            auto* ir_resp = req.params ? req.params->Find("inputResponses") : nullptr;
            ASSERT_NE(ir_resp, nullptr);
            auto* roots = ir_resp->Find("paths");
            ASSERT_NE(roots, nullptr);
            auto* list = roots->Find("roots");
            ASSERT_NE(list, nullptr);
            ASSERT_TRUE(list->IsArray());
            ASSERT_EQ(list->GetArray().size(), 1u);
            EXPECT_EQ(list->GetArray()[0]["uri"].GetString(), "file:///tmp");
            JsonValue result(JsonValue::object_tag);
            result["structuredContent"] = JsonValue(JsonValue::object_tag);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.input_required_config = ClientOptions::InputRequiredConfig{};
    auto client = McpClient::Create(std::move(pair.client), opts);

    bool listed = false;
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    client->SetRootsHandler([&listed](const ListRootsRequestParams&) {
        listed = true;
        ListRootsResult r;
        Root root;
        root.uri = "file:///tmp";
        root.name = "tmp";
        r.roots.push_back(std::move(root));
        return r;
    });
#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

    client->CallTool("echo", JsonValue(JsonValue::object_tag));
    EXPECT_TRUE(listed);
    EXPECT_EQ(round.load(), 2);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, MrtrEchoesServerAssignedInputRequestKeys) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> round{0};
    std::atomic<bool> keys_match{false};
    std::atomic<bool> elicitation_named{false};
    server_handler->SetRequestHandler(methods::kCallTool,
        [&round, &keys_match](
            const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (round.load() == 0) {
                round.store(1);
                InputRequiredResult ir;
                ElicitRequestParams elicit_params;
                elicit_params.message = "What is your name?";
                ir.input_requests["name"] = MakeInputRequestForElicitation(elicit_params);
                ir.input_requests["paths"] = MakeInputRequestForRoots(ListRootsRequestParams{});
                p.set_value(SerializeInputRequiredResult(ir));
                return;
            }
            round.store(2);
            auto* responses = req.params ? req.params->Find("inputResponses") : nullptr;
            keys_match.store(responses != nullptr && responses->IsObject()
                && responses->GetObject().size() == 2u
                && responses->Contains("name") && responses->Contains("paths")
                && !responses->Contains("elicit") && !responses->Contains("roots"));
            JsonValue result(JsonValue::object_tag);
            result["structuredContent"] = JsonValue(JsonValue::object_tag);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.input_required_config = ClientOptions::InputRequiredConfig{};
    auto client = McpClient::Create(std::move(pair.client), opts);

    client->SetElicitationHandler([&elicitation_named](const ElicitRequestParams& params) {
        elicitation_named.store(params.message == "What is your name?");
        ElicitResult r;
        r.action = "accept";
        JsonValue content(JsonValue::object_tag);
        content["user_name"] = JsonValue("Alice");
        r.content = std::move(content);
        return r;
    });
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    client->SetRootsHandler([](const ListRootsRequestParams&) {
        ListRootsResult r;
        Root root;
        root.uri = "file:///tmp";
        r.roots.push_back(std::move(root));
        return r;
    });
#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

    client->CallTool("echo", JsonValue(JsonValue::object_tag));
    EXPECT_TRUE(keys_match.load());
    EXPECT_TRUE(elicitation_named.load());
    EXPECT_EQ(round.load(), 2);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, MrtrUnknownInputRequestMethodThrows) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> calls{0};
    server_handler->SetRequestHandler(methods::kCallTool,
        [&calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            calls.fetch_add(1);
            JsonValue request(JsonValue::object_tag);
            request["method"] = JsonValue("tools/call");
            request["params"] = JsonValue(JsonValue::object_tag);
            JsonValue ir(JsonValue::object_tag);
            ir["resultType"] = JsonValue("input_required");
            ir["inputRequests"] = JsonValue(JsonValue::object_tag);
            ir["inputRequests"]["nested"] = std::move(request);
            p.set_value(std::move(ir));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.input_required_config = ClientOptions::InputRequiredConfig{};
    auto client = McpClient::Create(std::move(pair.client), opts);

    try {
        client->CallTool("echo", JsonValue(JsonValue::object_tag));
        FAIL();
    } catch (const McpError& e) {
        EXPECT_EQ(e.Code(), McpErrorCode::HandlerError);
        EXPECT_NE(std::string(e.what()).find("unsupported input request method"),
            std::string::npos);
    }
    EXPECT_EQ(calls.load(), 1);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, MrtrForgedRequestStateRejectionCarriesReason) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> calls{0};
    server_handler->SetRequestHandler(methods::kCallTool,
        [&calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            if (calls.fetch_add(1) == 0) {
                JsonValue ir(JsonValue::object_tag);
                ir["resultType"] = JsonValue("input_required");
                ir["requestState"] = JsonValue("forged-state-token");
                p.set_value(std::move(ir));
                return;
            }
            JsonValue err(JsonValue::object_tag);
            err["code"] = JsonValue(static_cast<int64_t>(McpErrorCode::InvalidParams));
            err["message"] = JsonValue("invalid requestState");
            JsonValue data(JsonValue::object_tag);
            data["reason"] = JsonValue("invalid_request_state");
            err["data"] = std::move(data);
            p.set_value(std::move(err));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.input_required_config = ClientOptions::InputRequiredConfig{};
    auto client = McpClient::Create(std::move(pair.client), opts);

    try {
        client->CallTool("echo", JsonValue(JsonValue::object_tag));
        FAIL();
    } catch (const McpError& e) {
        EXPECT_EQ(e.Code(), McpErrorCode::InvalidParams);
        EXPECT_NE(std::string(e.what()).find("invalid requestState"),
            std::string::npos);
        ASSERT_TRUE(e.Data().has_value());
        ASSERT_TRUE(e.Data()->IsObject());
        auto* reason = e.Data()->Find("reason");
        ASSERT_NE(reason, nullptr);
        EXPECT_EQ(reason->GetString(), "invalid_request_state");
    }
    EXPECT_EQ(calls.load(), 2);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, MrtrStateOnlyRoundBacksOff) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> round{0};
    std::atomic<bool> saw_request_state{false};
    std::atomic<bool> saw_input_responses{false};
    server_handler->SetRequestHandler(methods::kCallTool,
        [&round, &saw_request_state, &saw_input_responses](
            const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (round.load() == 0) {
                round.store(1);
                JsonValue ir(JsonValue::object_tag);
                ir["resultType"] = JsonValue("input_required");
                ir["requestState"] = JsonValue("state-only-1");
                p.set_value(std::move(ir));
                return;
            }
            round.store(2);
            if (req.params) {
                saw_request_state.store(req.params->Contains("requestState"));
                saw_input_responses.store(req.params->Contains("inputResponses"));
            }
            JsonValue result(JsonValue::object_tag);
            result["structuredContent"] = JsonValue(JsonValue::object_tag);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.input_required_config = ClientOptions::InputRequiredConfig{};
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto start = std::chrono::steady_clock::now();
    client->CallTool("echo", JsonValue(JsonValue::object_tag));
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_GE(elapsed_ms, 50);
    EXPECT_TRUE(saw_request_state.load());
    EXPECT_FALSE(saw_input_responses.load());
    EXPECT_EQ(round.load(), 2);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, MrtrBackoffGrowsAndResetsAfterFulfilledRound) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::mutex arrival_mutex;
    std::vector<std::chrono::steady_clock::time_point> arrivals;
    int round = 0;
    server_handler->SetRequestHandler(methods::kCallTool,
        [&arrival_mutex, &arrivals, &round](
            const JsonRpcRequest&, std::promise<JsonValue> p) {
            {
                std::lock_guard<std::mutex> lock(arrival_mutex);
                arrivals.push_back(std::chrono::steady_clock::now());
            }
            switch (round) {
            case 0:
            case 1:
            case 3:
                {
                    JsonValue ir(JsonValue::object_tag);
                    ir["resultType"] = JsonValue("input_required");
                    ir["requestState"] = JsonValue("st");
                    p.set_value(std::move(ir));
                }
                break;
            case 2:
                {
                    InputRequiredResult ir;
                    ElicitRequestParams params;
                    params.message = "more";
                    ir.input_requests["more"] = MakeInputRequestForElicitation(params);
                    p.set_value(SerializeInputRequiredResult(ir));
                }
                break;
            default:
                {
                    JsonValue result(JsonValue::object_tag);
                    result["structuredContent"] = JsonValue(JsonValue::object_tag);
                    p.set_value(std::move(result));
                }
                break;
            }
            ++round;
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.input_required_config = ClientOptions::InputRequiredConfig{};
    auto client = McpClient::Create(std::move(pair.client), opts);

    client->SetElicitationHandler([](const ElicitRequestParams&) {
        ElicitResult r;
        r.content = JsonValue("ok");
        return r;
    });

    client->CallTool("echo", JsonValue(JsonValue::object_tag));

    std::vector<int64_t> gaps;
    {
        std::lock_guard<std::mutex> lock(arrival_mutex);
        ASSERT_GE(arrivals.size(), 5u);
        for (size_t i = 1; i < arrivals.size(); ++i) {
            gaps.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(
                arrivals[i] - arrivals[i - 1]).count());
        }
    }
    ASSERT_EQ(gaps.size(), 4u);
    EXPECT_GE(gaps[0], 50);
    EXPECT_GE(gaps[1], 100);
    EXPECT_GE(gaps[3], 50);
    EXPECT_LT(gaps[3], 200);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, MrtrMaxRoundsDefaultTen) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> calls{0};
    server_handler->SetRequestHandler(methods::kCallTool,
        [&calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            calls.fetch_add(1);
            InputRequiredResult ir;
            ElicitRequestParams params;
            params.message = "more input";
            ir.input_requests["more"] = MakeInputRequestForElicitation(params);
            p.set_value(SerializeInputRequiredResult(ir));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.input_required_config = ClientOptions::InputRequiredConfig{};
    auto client = McpClient::Create(std::move(pair.client), opts);

    client->SetElicitationHandler([](const ElicitRequestParams&) {
        ElicitResult r;
        r.content = JsonValue("ok");
        return r;
    });

    EXPECT_THROW(
        client->CallTool("echo", JsonValue(JsonValue::object_tag)),
        McpError);
    EXPECT_EQ(calls.load(), 11);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, MrtrDefaultAutoFulfillsEmbeddedElicitation) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> round{0};
    std::atomic<bool> saw_response{false};
    server_handler->SetRequestHandler(methods::kCallTool,
        [&round, &saw_response](
            const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (round.load() == 0) {
                round.store(1);
                InputRequiredResult ir;
                ElicitRequestParams elicit_params;
                elicit_params.message = "What is your name?";
                ir.input_requests["user_name"] =
                    MakeInputRequestForElicitation(elicit_params);
                ir.request_state = "st-default";
                p.set_value(SerializeInputRequiredResult(ir));
                return;
            }
            round.store(2);
            auto* responses = req.params ? req.params->Find("inputResponses") : nullptr;
            auto* entry = responses && responses->IsObject()
                ? responses->Find("user_name") : nullptr;
            auto* action = entry && entry->IsObject() ? entry->Find("action") : nullptr;
            auto* content = entry && entry->IsObject() ? entry->Find("content") : nullptr;
            auto* name = content && content->IsObject() ? content->Find("name") : nullptr;
            saw_response.store(action && action->IsString()
                && action->GetString() == "accept"
                && name && name->IsString() && name->GetString() == "Alice");
            JsonValue result(JsonValue::object_tag);
            result["structuredContent"] = JsonValue(JsonValue::object_tag);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    std::atomic<int> handler_calls{0};
    client->SetElicitationHandler([&handler_calls](const ElicitRequestParams&) {
        handler_calls.fetch_add(1);
        ElicitResult r;
        r.action = "accept";
        JsonValue content(JsonValue::object_tag);
        content["name"] = JsonValue("Alice");
        r.content = std::move(content);
        return r;
    });

    auto result = client->CallTool("echo", JsonValue(JsonValue::object_tag));
    EXPECT_EQ(handler_calls.load(), 1);
    EXPECT_TRUE(saw_response.load());
    EXPECT_EQ(round.load(), 2);
    EXPECT_FALSE(result.input_required.has_value());
    EXPECT_EQ(result.result_type, ResultType::Complete);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, MrtrUrlElicitationPropagatesHandlerAction) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> round{0};
    std::atomic<bool> saw_decline{false};
    server_handler->SetRequestHandler(methods::kCallTool,
        [&round, &saw_decline](
            const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (round.load() == 0) {
                round.store(1);
                ElicitRequestParams params;
                params.message = "Open the URL and approve access";
                params.mode = "url";
                params.url = "https://example.com/authorize";
                InputRequiredResult ir;
                ir.input_requests["consent"] =
                    MakeInputRequestForElicitation(params);
                p.set_value(SerializeInputRequiredResult(ir));
                return;
            }
            round.store(2);
            auto* responses = req.params ? req.params->Find("inputResponses") : nullptr;
            auto* entry = responses && responses->IsObject()
                ? responses->Find("consent") : nullptr;
            auto* action = entry && entry->IsObject() ? entry->Find("action") : nullptr;
            saw_decline.store(action && action->IsString() &&
                action->GetString() == "decline");
            JsonValue result(JsonValue::object_tag);
            result["structuredContent"] = JsonValue(JsonValue::object_tag);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    client->SetUrlElicitationHandler([](const ElicitRequestParams&) {
        ElicitResult result;
        result.action = "decline";
        return result;
    });

    auto result = client->CallTool("echo", JsonValue(JsonValue::object_tag));
    EXPECT_TRUE(saw_decline.load());
    EXPECT_EQ(round.load(), 2);
    EXPECT_EQ(result.result_type, ResultType::Complete);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, MrtrUrlElicitationInvalidActionThrowsHandlerError) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> server_calls{0};
    server_handler->SetRequestHandler(methods::kCallTool,
        [&server_calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            server_calls.fetch_add(1);
            ElicitRequestParams params;
            params.message = "Open the URL and approve access";
            params.mode = "url";
            params.url = "https://example.com/authorize";
            InputRequiredResult ir;
            ir.input_requests["consent"] = MakeInputRequestForElicitation(params);
            p.set_value(SerializeInputRequiredResult(ir));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    const std::vector<std::string> actions{"maybe", ""};
    std::atomic<size_t> handler_calls{0};
    client->SetUrlElicitationHandler(
        [&actions, &handler_calls](const ElicitRequestParams&) {
            ElicitResult result;
            result.action = actions[handler_calls.fetch_add(1) % actions.size()];
            return result;
        });

    for (size_t i = 0; i < actions.size(); ++i) {
        try {
            client->CallTool("echo", JsonValue(JsonValue::object_tag));
            FAIL();
        } catch (const McpError& e) {
            EXPECT_EQ(e.Code(), McpErrorCode::HandlerError);
            EXPECT_NE(std::string(e.what()).find("invalid url elicitation action"),
                std::string::npos);
        }
    }
    EXPECT_EQ(server_calls.load(), 2);
    EXPECT_EQ(handler_calls.load(), static_cast<size_t>(2));

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, MrtrDefaultWithoutHandlersReturnsInputRequired) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> calls{0};
    server_handler->SetRequestHandler(methods::kCallTool,
        [&calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            calls.fetch_add(1);
            InputRequiredResult ir;
            ElicitRequestParams elicit_params;
            elicit_params.message = "What is your name?";
            ir.input_requests["user_name"] =
                MakeInputRequestForElicitation(elicit_params);
            ir.request_state = "st-manual";
            p.set_value(SerializeInputRequiredResult(ir));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    CallToolResult result;
    EXPECT_NO_THROW(result = client->CallTool(
        "echo", JsonValue(JsonValue::object_tag)));
    EXPECT_EQ(calls.load(), 1);
    ASSERT_TRUE(result.input_required.has_value());
    EXPECT_EQ(result.result_type, ResultType::InputRequired);
    ASSERT_TRUE(result.input_required->request_state.has_value());
    EXPECT_EQ(*result.input_required->request_state, "st-manual");
    ASSERT_EQ(result.input_required->input_requests.size(), size_t{1});
    EXPECT_EQ(result.input_required->input_requests.count("user_name"), size_t{1});

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, MrtrExplicitNulloptKeepsManualMode) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> calls{0};
    server_handler->SetRequestHandler(methods::kCallTool,
        [&calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            calls.fetch_add(1);
            InputRequiredResult ir;
            ElicitRequestParams elicit_params;
            elicit_params.message = "What is your name?";
            ir.input_requests["user_name"] =
                MakeInputRequestForElicitation(elicit_params);
            p.set_value(SerializeInputRequiredResult(ir));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.input_required_config = std::nullopt;
    auto client = McpClient::Create(std::move(pair.client), opts);

    std::atomic<int> handler_calls{0};
    client->SetElicitationHandler([&handler_calls](const ElicitRequestParams&) {
        handler_calls.fetch_add(1);
        return ElicitResult{};
    });

    CallToolResult result;
    EXPECT_NO_THROW(result = client->CallTool(
        "echo", JsonValue(JsonValue::object_tag)));
    EXPECT_EQ(handler_calls.load(), 0);
    EXPECT_EQ(calls.load(), 1);
    ASSERT_TRUE(result.input_required.has_value());
    EXPECT_EQ(result.result_type, ResultType::InputRequired);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, ListToolsCachesAndInvalidatesOnListChanged) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> calls{0};
    server_handler->SetRequestHandler(methods::kListTools,
        [&calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            calls.fetch_add(1);
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            JsonValue t(JsonValue::object_tag);
            t["name"] = JsonValue("tool-x");
            t["inputSchema"] = JsonValue(JsonValue::object_tag);
            arr.PushBack(std::move(t));
            result["tools"] = JsonValue(std::move(arr));
            JsonValue hint(JsonValue::object_tag);
            hint["ttlMs"] = JsonValue(int64_t(60000));
            result["cacheHint"] = std::move(hint);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto r1 = client->ListTools();
    ASSERT_EQ(r1.tools.size(), 1u);
    EXPECT_EQ(calls.load(), 1);

    auto r2 = client->ListTools();
    ASSERT_EQ(r2.tools.size(), 1u);
    EXPECT_EQ(calls.load(), 1);

    server_handler->SendNotification(notifications::kToolListChanged,
        JsonValue(JsonValue::object_tag));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto r3 = client->ListTools();
    ASSERT_EQ(r3.tools.size(), 1u);
    EXPECT_EQ(calls.load(), 2);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, ToolOutputSchemaValidation) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), sopts);

    ToolOptions topts;
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = "object";
    JsonValue props(JsonValue::object_tag);
    JsonValue name_s(JsonValue::object_tag);
    name_s["type"] = "string";
    props["name"] = std::move(name_s);
    schema["properties"] = std::move(props);
    JsonValue req(JsonValue::array_tag);
    req.PushBack(JsonValue("name"));
    schema["required"] = std::move(req);
    topts.output_schema = std::move(schema);
    server->RegisterTool("schema-tool",
        topts,
        std::function<CallToolResult(const Ctx&)>(
            [](const Ctx&) -> CallToolResult {
                CallToolResult r;
                r.structured_content = JsonValue(JsonValue::object_tag);
                (*r.structured_content)["other"] = JsonValue("x");
                return r;
            }));

    ClientOptions cops;
    cops.connect_mode = ConnectMode::Pin;
    cops.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), cops);

    EXPECT_THROW(
        client->CallTool("schema-tool", JsonValue(JsonValue::object_tag)),
        McpError);

    client->Close();
    server->Close();
}

TEST(McpClientTest, ListToolsCachesCursorPagesSeparately) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> plain_calls{0};
    std::atomic<int> cursor_calls{0};
    server_handler->SetRequestHandler(methods::kListTools,
        [&plain_calls, &cursor_calls](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            bool has_cursor = req.params && req.params->IsObject() &&
                req.params->Contains("cursor");
            if (has_cursor) cursor_calls.fetch_add(1); else plain_calls.fetch_add(1);
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            JsonValue t(JsonValue::object_tag);
            t["name"] = JsonValue(has_cursor ? "page-tool" : "all-tool");
            t["inputSchema"] = JsonValue(JsonValue::object_tag);
            arr.PushBack(std::move(t));
            result["tools"] = JsonValue(std::move(arr));
            JsonValue hint(JsonValue::object_tag);
            hint["ttlMs"] = JsonValue(int64_t(60000));
            result["cacheHint"] = std::move(hint);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto all = client->ListTools();
    ASSERT_EQ(all.tools.size(), 1u);
    EXPECT_EQ(all.tools[0].name, "all-tool");
    auto page1 = client->ListTools("c1");
    ASSERT_EQ(page1.tools.size(), 1u);
    EXPECT_EQ(page1.tools[0].name, "page-tool");
    auto all2 = client->ListTools();
    EXPECT_EQ(all2.tools[0].name, "all-tool");
    auto page1_again = client->ListTools("c1");
    EXPECT_EQ(page1_again.tools[0].name, "page-tool");
    auto page2 = client->ListTools("c2");
    EXPECT_EQ(page2.tools[0].name, "page-tool");

    EXPECT_EQ(plain_calls.load(), 1);
    EXPECT_EQ(cursor_calls.load(), 2);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, ReadResourceCachesPerUri) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> calls_a{0};
    std::atomic<int> calls_b{0};
    server_handler->SetRequestHandler(methods::kReadResource,
        [&calls_a, &calls_b](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            auto uri = req.params->Find("uri")->GetString();
            if (uri == "uri-a") calls_a.fetch_add(1); else calls_b.fetch_add(1);
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            JsonValue c(JsonValue::object_tag);
            c["uri"] = JsonValue(uri);
            c["text"] = JsonValue(uri);
            arr.PushBack(std::move(c));
            result["contents"] = JsonValue(std::move(arr));
            JsonValue hint(JsonValue::object_tag);
            hint["ttlMs"] = JsonValue(int64_t(60000));
            result["cacheHint"] = std::move(hint);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto r1 = client->ReadResource("uri-a");
    ASSERT_EQ(r1.contents.size(), 1u);
    auto r2 = client->ReadResource("uri-b");
    ASSERT_EQ(r2.contents.size(), 1u);
    auto r3 = client->ReadResource("uri-a");
    ASSERT_EQ(r3.contents.size(), 1u);
    auto r4 = client->ReadResource("uri-b");
    ASSERT_EQ(r4.contents.size(), 1u);

    EXPECT_EQ(calls_a.load(), 1);
    EXPECT_EQ(calls_b.load(), 1);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, CacheClampsTtlTo24Hours) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> calls{0};
    server_handler->SetRequestHandler(methods::kListTools,
        [&calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            calls.fetch_add(1);
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            JsonValue t(JsonValue::object_tag);
            t["name"] = JsonValue("tool-x");
            t["inputSchema"] = JsonValue(JsonValue::object_tag);
            arr.PushBack(std::move(t));
            result["tools"] = JsonValue(std::move(arr));
            JsonValue hint(JsonValue::object_tag);
            hint["ttlMs"] = JsonValue(int64_t(25LL * 3600 * 1000));
            result["cacheHint"] = std::move(hint);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto r1 = client->ListTools();
    ASSERT_EQ(r1.tools.size(), 1u);
    auto r2 = client->ListTools();
    ASSERT_EQ(r2.tools.size(), 1u);
    EXPECT_EQ(calls.load(), 1);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, CachePrivateEntryHitsInSameConnection) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> calls_a{0};
    std::atomic<int> calls_b{0};
    server_handler->SetRequestHandler(methods::kReadResource,
        [&calls_a, &calls_b](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            auto uri = req.params->Find("uri")->GetString();
            if (uri == "uri-a") calls_a.fetch_add(1); else calls_b.fetch_add(1);
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            JsonValue c(JsonValue::object_tag);
            c["uri"] = JsonValue(uri);
            c["text"] = JsonValue(uri);
            arr.PushBack(std::move(c));
            result["contents"] = JsonValue(std::move(arr));
            JsonValue hint(JsonValue::object_tag);
            hint["ttlMs"] = JsonValue(int64_t(60000));
            if (uri == "uri-b") hint["cacheScope"] = JsonValue("private");
            result["cacheHint"] = std::move(hint);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto r1 = client->ReadResource("uri-a");
    ASSERT_EQ(r1.contents.size(), 1u);
    EXPECT_EQ(calls_a.load(), 1);
    auto r2 = client->ReadResource("uri-a");
    ASSERT_EQ(r2.contents.size(), 1u);
    EXPECT_EQ(calls_a.load(), 1);

    auto r3 = client->ReadResource("uri-b");
    ASSERT_EQ(r3.contents.size(), 1u);
    EXPECT_EQ(calls_b.load(), 1);
    auto r4 = client->ReadResource("uri-b");
    ASSERT_EQ(r4.contents.size(), 1u);
    EXPECT_EQ(calls_b.load(), 1);

    auto r5 = client->ReadResource("uri-a");
    ASSERT_EQ(r5.contents.size(), 1u);
    EXPECT_EQ(calls_a.load(), 1);
    auto r6 = client->ReadResource("uri-b");
    ASSERT_EQ(r6.contents.size(), 1u);
    EXPECT_EQ(calls_b.load(), 1);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, PrivateCachePartitionedByAuthContext) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<const char*> auth_context{"user-a"};
    std::atomic<int> calls{0};
    server_handler->SetRequestHandler(methods::kReadResource,
        [&calls, &auth_context](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            calls.fetch_add(1);
            auto uri = req.params->Find("uri")->GetString();
            std::string text = std::string("content-") + auth_context.load();
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            JsonValue c(JsonValue::object_tag);
            c["uri"] = JsonValue(uri);
            c["text"] = JsonValue(text);
            arr.PushBack(std::move(c));
            result["contents"] = JsonValue(std::move(arr));
            JsonValue hint(JsonValue::object_tag);
            hint["ttlMs"] = JsonValue(int64_t(60000));
            hint["cacheScope"] = JsonValue("private");
            result["cacheHint"] = std::move(hint);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.cache_auth_context = [&auth_context] {
        return std::string(auth_context.load());
    };
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto r1 = client->ReadResource("uri-a");
    auto r2 = client->ReadResource("uri-a");
    ASSERT_EQ(r1.contents.size(), 1u);
    ASSERT_EQ(r2.contents.size(), 1u);
    EXPECT_EQ(std::get<TextResourceContents>(r1.contents[0]).text,
        "content-user-a");
    EXPECT_EQ(std::get<TextResourceContents>(r2.contents[0]).text,
        "content-user-a");
    EXPECT_EQ(calls.load(), 1);

    auth_context.store("user-b");
    auto r3 = client->ReadResource("uri-a");
    auto r4 = client->ReadResource("uri-a");
    ASSERT_EQ(r3.contents.size(), 1u);
    ASSERT_EQ(r4.contents.size(), 1u);
    EXPECT_EQ(std::get<TextResourceContents>(r3.contents[0]).text,
        "content-user-b");
    EXPECT_EQ(std::get<TextResourceContents>(r4.contents[0]).text,
        "content-user-b");
    EXPECT_EQ(calls.load(), 2);

    auth_context.store("user-a");
    auto r5 = client->ReadResource("uri-a");
    ASSERT_EQ(r5.contents.size(), 1u);
    EXPECT_EQ(std::get<TextResourceContents>(r5.contents[0]).text,
        "content-user-a");
    EXPECT_EQ(calls.load(), 3);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, CloseDropsPrivateCacheEntries) {
    auto pair_a = InMemoryTransport::CreatePair();
    auto server_a = std::make_shared<McpSessionHandler>(
        std::move(pair_a.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    auto pair_b = InMemoryTransport::CreatePair();
    auto server_b = std::make_shared<McpSessionHandler>(
        std::move(pair_b.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> calls{0};
    auto private_handler = [&calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
        calls.fetch_add(1);
        JsonValue result(JsonValue::object_tag);
        JsonValue arr(JsonValue::array_tag);
        JsonValue t(JsonValue::object_tag);
        t["name"] = JsonValue("tool-x");
        t["inputSchema"] = JsonValue(JsonValue::object_tag);
        arr.PushBack(std::move(t));
        result["tools"] = JsonValue(std::move(arr));
        JsonValue hint(JsonValue::object_tag);
        hint["ttlMs"] = JsonValue(int64_t(60000));
        hint["cacheScope"] = JsonValue("private");
        result["cacheHint"] = std::move(hint);
        p.set_value(std::move(result));
    };
    server_a->SetRequestHandler(methods::kListTools, private_handler);
    server_b->SetRequestHandler(methods::kListTools, private_handler);
    server_a->Start();
    server_b->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);

    auto client_a = McpClient::Create(std::move(pair_a.client), opts);
    auto r1 = client_a->ListTools();
    ASSERT_EQ(r1.tools.size(), 1u);
    EXPECT_EQ(calls.load(), 1);
    auto r2 = client_a->ListTools();
    ASSERT_EQ(r2.tools.size(), 1u);
    EXPECT_EQ(calls.load(), 1);
    client_a->Close();

    auto client_b = McpClient::Create(std::move(pair_b.client), opts);
    auto r3 = client_b->ListTools();
    ASSERT_EQ(r3.tools.size(), 1u);
    EXPECT_EQ(calls.load(), 2);

    client_b->Close();
    server_a->Close();
    server_b->Close();
}

TEST(McpClientTest, ResourceUpdatedInvalidatesOnlyThatUri) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> calls_a{0};
    std::atomic<int> calls_b{0};
    server_handler->SetRequestHandler(methods::kReadResource,
        [&calls_a, &calls_b](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            auto uri = req.params->Find("uri")->GetString();
            if (uri == "uri-a") calls_a.fetch_add(1); else calls_b.fetch_add(1);
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            JsonValue c(JsonValue::object_tag);
            c["uri"] = JsonValue(uri);
            c["text"] = JsonValue(uri);
            arr.PushBack(std::move(c));
            result["contents"] = JsonValue(std::move(arr));
            JsonValue hint(JsonValue::object_tag);
            hint["ttlMs"] = JsonValue(int64_t(60000));
            result["cacheHint"] = std::move(hint);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    client->ReadResource("uri-a");
    client->ReadResource("uri-b");
    EXPECT_EQ(calls_a.load(), 1);
    EXPECT_EQ(calls_b.load(), 1);

    JsonValue params(JsonValue::object_tag);
    params["uri"] = JsonValue("uri-a");
    server_handler->SendNotification(notifications::kResourceUpdated, std::move(params));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client->ReadResource("uri-a");
    client->ReadResource("uri-b");
    EXPECT_EQ(calls_a.load(), 2);
    EXPECT_EQ(calls_b.load(), 1);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, SubscribeAsyncWaitsForAcknowledged) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), sopts);

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Auto;
    opts.discover_probe_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), opts);
    ASSERT_TRUE(client->IsModernProtocol());

    SubscriptionsListenRequestParams params;
    params.notifications.tools_list_changed = true;
    EXPECT_NO_THROW(client->SubscribeAsync(params));

    client->Close();
    server->Close();
}

TEST(McpClientTest, SubscribeAsyncTimesOutWithoutAcknowledged) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    server_handler->SetRequestHandler(methods::kDiscover,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue result(JsonValue::object_tag);
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            result["serverInfo"] = SerializeImplementation(Implementation{"silent-server", "1.0"});
            p.set_value(std::move(result));
        });
    auto held_response = std::make_shared<std::promise<JsonValue>>();
    server_handler->SetRequestHandler(methods::kSubscribe,
        [held_response](const JsonRpcRequest&, std::promise<JsonValue> p) {
            *held_response = std::move(p);
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Auto;
    opts.discover_probe_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), opts);
    ASSERT_TRUE(client->IsModernProtocol());

    SubscriptionsListenRequestParams params;
    params.notifications.tools_list_changed = true;
    EXPECT_THROW(client->SubscribeAsync(params), McpError);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, SubscribeAsyncReturnsOnAcknowledgedWithoutResult) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    auto held_response = std::make_shared<std::promise<JsonValue>>();
    std::weak_ptr<McpSessionHandler> weak_server = server_handler;
    server_handler->SetRequestHandler(methods::kSubscribe,
        [held_response, weak_server](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            *held_response = std::move(p);
            std::string subscription_id;
            if (req.meta) {
                if (auto* v = req.meta->Find("io.modelcontextprotocol/subscriptionId");
                    v && v->IsString()) {
                    subscription_id = v->GetString();
                }
            }
            JsonRpcNotification notif;
            notif.method = std::string(notifications::kSubscriptionsAcknowledged);
            notif.params = SerializeSubscriptionsAcknowledgedNotificationParams(
                SubscriptionsAcknowledgedNotificationParams{});
            JsonValue meta(JsonValue::object_tag);
            meta["io.modelcontextprotocol/protocolVersion"] =
                JsonValue(std::string(kLatestProtocolVersion));
            meta["io.modelcontextprotocol/subscriptionId"] =
                JsonValue(subscription_id);
            notif.meta = std::move(meta);
            if (auto server = weak_server.lock()) {
                server->SendMessage(JsonRpcMessage{std::move(notif)});
            }
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);
    ASSERT_TRUE(client->IsModernProtocol());

    SubscriptionsListenRequestParams params;
    params.notifications.tools_list_changed = true;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_NO_THROW(client->SubscribeAsync(params));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, SendRootsListChangedAtOrAfterMinVersion) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    std::promise<void> received;
    auto received_future = received.get_future();
    server_handler->SetNotificationHandler(notifications::kRootsListChanged,
        [&received](const JsonRpcNotification&) {
            received.set_value();
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string("2025-06-18");
    auto client = McpClient::Create(std::move(pair.client), opts);
    ASSERT_EQ(client->GetNegotiatedProtocolVersion(), std::string("2025-06-18"));

    EXPECT_NO_THROW(client->SendRootsListChanged());
    EXPECT_EQ(received_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);

    client->Close();
    server_handler->Close();

    auto pair2 = InMemoryTransport::CreatePair();
    auto server_handler2 = std::make_shared<McpSessionHandler>(
        std::move(pair2.server), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    std::promise<void> received2;
    auto received2_future = received2.get_future();
    server_handler2->SetNotificationHandler(notifications::kRootsListChanged,
        [&received2](const JsonRpcNotification&) {
            received2.set_value();
        });
    server_handler2->SetRequestHandler(methods::kInitialize,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue result(JsonValue::object_tag);
            result["protocolVersion"] = JsonValue(std::string(kLegacyProtocolVersion));
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            result["serverInfo"] = SerializeImplementation(Implementation{"legacy-server", "1.0"});
            p.set_value(std::move(result));
        });
    server_handler2->Start();

    ClientOptions legacy_opts;
    legacy_opts.connect_mode = ConnectMode::Legacy;
    legacy_opts.initialization_timeout = std::chrono::seconds(5);
    auto legacy_client = McpClient::Create(std::move(pair2.client), legacy_opts);
    ASSERT_EQ(legacy_client->GetNegotiatedProtocolVersion(),
              std::string(kLegacyProtocolVersion));

    EXPECT_NO_THROW(legacy_client->SendRootsListChanged());
    EXPECT_EQ(received2_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);

    legacy_client->Close();
    server_handler2->Close();
}

TEST(McpClientTest, SendRootsListChangedRejectedBelowMinVersion) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    server_handler->SetRequestHandler(methods::kInitialize,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue result(JsonValue::object_tag);
            result["protocolVersion"] = JsonValue(std::string("2025-03-26"));
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            result["serverInfo"] = SerializeImplementation(Implementation{"old-server", "1.0"});
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Auto;
    opts.discover_probe_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), opts);
    ASSERT_EQ(client->GetNegotiatedProtocolVersion(), std::string("2025-03-26"));

    bool threw = false;
    try {
        client->SendRootsListChanged();
    } catch (const McpError& e) {
        threw = true;
        EXPECT_NE(std::string(e.ErrorMessage()).find("2025-06-18"),
                  std::string::npos);
    }
    EXPECT_TRUE(threw);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, SendRootsListChangedRejectedForModernVersion) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    std::promise<void> received;
    auto received_future = received.get_future();
    server_handler->SetNotificationHandler(notifications::kRootsListChanged,
        [&received](const JsonRpcNotification&) {
            received.set_value();
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);
    ASSERT_EQ(client->GetNegotiatedProtocolVersion(), std::string(kLatestProtocolVersion));

    bool threw = false;
    try {
        client->SendRootsListChanged();
    } catch (const McpError& e) {
        threw = true;
        EXPECT_EQ(e.Code(), McpErrorCode::ProtocolViolation);
        EXPECT_NE(std::string(e.ErrorMessage()).find("2026-07-28"),
                  std::string::npos);
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(received_future.wait_for(std::chrono::milliseconds(200)),
              std::future_status::timeout);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, CallToolReceivesProgressNotifications) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), sopts);

    server->RegisterTool("progress-tool",
        ToolOptions{},
        std::function<CallToolResult(const Ctx&)>(
            [](const Ctx& ctx) -> CallToolResult {
                const auto& req = ctx.GetRequest();
                ProgressToken token{std::string("unknown")};
                if (req.meta) {
                    if (auto* pt = req.meta->Find("progressToken")) {
                        if (pt->IsInt()) token = ProgressToken{pt->GetInt()};
                        else if (pt->IsString())
                            token = ProgressToken{pt->GetString()};
                    }
                }
                ctx.Server().SendProgress(token, 0.5, 1.0, "half");
                CallToolResult r;
                r.structured_content = JsonValue(JsonValue::object_tag);
                return r;
            }));

    ClientOptions cops;
    cops.connect_mode = ConnectMode::Auto;
    cops.discover_probe_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), cops);

    std::promise<void> got_progress;
    auto got_future = got_progress.get_future();
    std::atomic<bool> callback_called{false};

    RequestOptions ropts;
    ropts.on_progress = [&](const ProgressNotificationParams& p) {
        ASSERT_TRUE(std::holds_alternative<int64_t>(p.progress_token));
        EXPECT_EQ(std::get<int64_t>(p.progress_token), int64_t(1));
        EXPECT_DOUBLE_EQ(p.progress, 0.5);
        ASSERT_TRUE(p.total.has_value());
        EXPECT_DOUBLE_EQ(*p.total, 1.0);
        ASSERT_TRUE(p.message.has_value());
        EXPECT_EQ(*p.message, "half");
        callback_called.store(true);
        got_progress.set_value();
    };

    auto result =
        client->CallTool("progress-tool", JsonValue(JsonValue::object_tag), ropts);

    EXPECT_EQ(got_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    EXPECT_TRUE(callback_called.load());
    EXPECT_FALSE(result.is_error);

    client->Close();
    server->Close();
}

TEST(McpClientTest, CallToolWithoutProgressCallbackIgnoresProgress) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), sopts);

    server->RegisterTool("plain-tool",
        ToolOptions{},
        std::function<CallToolResult(const Ctx&)>(
            [](const Ctx& ctx) -> CallToolResult {
                ctx.Server().SendProgress(ProgressToken{std::string("stray")}, 0.25);
                CallToolResult r;
                r.structured_content = JsonValue(JsonValue::object_tag);
                return r;
            }));

    ClientOptions cops;
    cops.connect_mode = ConnectMode::Auto;
    cops.discover_probe_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), cops);

    EXPECT_NO_THROW(
        client->CallTool("plain-tool", JsonValue(JsonValue::object_tag)));

    client->Close();
    server->Close();
}

TEST(McpClientTest, ProgressCallbackRemovedAfterRequestCompletes) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"test-server", "1.0.0"};
    auto server = McpServer::Create(std::move(pair.server), sopts);

    std::atomic<int64_t> seen_token{0};
    std::atomic<bool> has_token{false};
    server->RegisterTool("progress-tool",
        ToolOptions{},
        std::function<CallToolResult(const Ctx&)>(
            [&](const Ctx& ctx) -> CallToolResult {
                const auto& req = ctx.GetRequest();
                ProgressToken token{std::string("unknown")};
                if (req.meta) {
                    if (auto* pt = req.meta->Find("progressToken")) {
                        if (pt->IsInt()) {
                            token = ProgressToken{pt->GetInt()};
                            seen_token.store(pt->GetInt());
                            has_token.store(true);
                        } else if (pt->IsString()) {
                            token = ProgressToken{pt->GetString()};
                        }
                    }
                }
                ctx.Server().SendProgress(token, 0.5);
                CallToolResult r;
                r.structured_content = JsonValue(JsonValue::object_tag);
                return r;
            }));

    ClientOptions cops;
    cops.connect_mode = ConnectMode::Auto;
    cops.discover_probe_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), cops);

    std::atomic<int> progress_count{0};
    RequestOptions ropts;
    ropts.on_progress = [&progress_count](const ProgressNotificationParams&) {
        progress_count.fetch_add(1);
    };

    client->CallTool("progress-tool", JsonValue(JsonValue::object_tag), ropts);
    EXPECT_TRUE(has_token.load());
    EXPECT_EQ(progress_count.load(), 1);

    server->SendProgress(ProgressToken{seen_token.load()}, 0.9);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(progress_count.load(), 1);

    client->Close();
    server->Close();
}

TEST(McpClientTest, ListToolsAllAggregatesCursorChain) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));

    server_handler->SetRequestHandler(methods::kListTools,
        [](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            std::string cursor;
            if (req.params && req.params->IsObject()) {
                if (auto* c = req.params->Find("cursor"); c && c->IsString())
                    cursor = c->GetString();
            }
            auto tool = [](const char* name) {
                JsonValue t(JsonValue::object_tag);
                t["name"] = JsonValue(name);
                t["inputSchema"] = JsonValue(JsonValue::object_tag);
                return t;
            };
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            if (cursor.empty()) {
                arr.PushBack(tool("tool-1"));
                arr.PushBack(tool("tool-2"));
                result["nextCursor"] = JsonValue("p1");
            } else if (cursor == "p1") {
                arr.PushBack(tool("tool-3"));
                arr.PushBack(tool("tool-4"));
                result["nextCursor"] = JsonValue("p2");
            } else {
                arr.PushBack(tool("tool-5"));
            }
            result["tools"] = JsonValue(std::move(arr));
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto all = client->ListToolsAll();
    ASSERT_EQ(all.tools.size(), 5u);
    EXPECT_EQ(all.tools[0].name, "tool-1");
    EXPECT_EQ(all.tools[1].name, "tool-2");
    EXPECT_EQ(all.tools[2].name, "tool-3");
    EXPECT_EQ(all.tools[3].name, "tool-4");
    EXPECT_EQ(all.tools[4].name, "tool-5");
    EXPECT_FALSE(all.next_cursor.has_value());

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, ListToolsAllThrowsWhenPaginationNeverConverges) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));

    std::atomic<int> page_calls{0};
    server_handler->SetRequestHandler(methods::kListTools,
        [&page_calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            page_calls.fetch_add(1);
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            JsonValue t(JsonValue::object_tag);
            t["name"] = JsonValue("tool");
            t["inputSchema"] = JsonValue(JsonValue::object_tag);
            arr.PushBack(std::move(t));
            result["tools"] = JsonValue(std::move(arr));
            result["nextCursor"] = JsonValue("never-ending");
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    bool thrown = false;
    try {
        client->ListToolsAll();
    } catch (const McpError& e) {
        thrown = true;
        EXPECT_EQ(e.Code(), McpErrorCode::ProtocolViolation);
    }
    EXPECT_TRUE(thrown);
    EXPECT_EQ(page_calls.load(), 64);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, ListToolsAllSinglePageEqualsPlainList) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));

    std::atomic<int> list_calls{0};
    server_handler->SetRequestHandler(methods::kListTools,
        [&list_calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            list_calls.fetch_add(1);
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            JsonValue t1(JsonValue::object_tag);
            t1["name"] = JsonValue("tool-1");
            t1["inputSchema"] = JsonValue(JsonValue::object_tag);
            arr.PushBack(std::move(t1));
            JsonValue t2(JsonValue::object_tag);
            t2["name"] = JsonValue("tool-2");
            t2["inputSchema"] = JsonValue(JsonValue::object_tag);
            arr.PushBack(std::move(t2));
            result["tools"] = JsonValue(std::move(arr));
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto all = client->ListToolsAll();
    ASSERT_EQ(all.tools.size(), 2u);
    EXPECT_EQ(all.tools[0].name, "tool-1");
    EXPECT_EQ(all.tools[1].name, "tool-2");
    EXPECT_FALSE(all.next_cursor.has_value());
    EXPECT_EQ(list_calls.load(), 1);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, ListResourcesTemplatesPromptsAllAggregate) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));

    server_handler->SetRequestHandler(methods::kListResources,
        [](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            bool has_cursor = req.params && req.params->IsObject() &&
                req.params->Contains("cursor");
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            JsonValue r(JsonValue::object_tag);
            r["uri"] = JsonValue(has_cursor ? "mem://r2" : "mem://r1");
            r["name"] = JsonValue(has_cursor ? "r2" : "r1");
            arr.PushBack(std::move(r));
            result["resources"] = JsonValue(std::move(arr));
            if (!has_cursor) result["nextCursor"] = JsonValue("rc1");
            p.set_value(std::move(result));
        });
    server_handler->SetRequestHandler(methods::kListResourceTemplates,
        [](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            bool has_cursor = req.params && req.params->IsObject() &&
                req.params->Contains("cursor");
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            JsonValue t(JsonValue::object_tag);
            t["uriTemplate"] = JsonValue(has_cursor ? "mem://{id}/t2" : "mem://{id}/t1");
            t["name"] = JsonValue(has_cursor ? "t2" : "t1");
            arr.PushBack(std::move(t));
            result["resourceTemplates"] = JsonValue(std::move(arr));
            if (!has_cursor) result["nextCursor"] = JsonValue("tc1");
            p.set_value(std::move(result));
        });
    server_handler->SetRequestHandler(methods::kListPrompts,
        [](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            bool has_cursor = req.params && req.params->IsObject() &&
                req.params->Contains("cursor");
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            JsonValue pr(JsonValue::object_tag);
            pr["name"] = JsonValue(has_cursor ? "prompt-2" : "prompt-1");
            arr.PushBack(std::move(pr));
            result["prompts"] = JsonValue(std::move(arr));
            if (!has_cursor) result["nextCursor"] = JsonValue("pc1");
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto resources = client->ListResourcesAll();
    ASSERT_EQ(resources.resources.size(), 2u);
    EXPECT_EQ(resources.resources[0].name, "r1");
    EXPECT_EQ(resources.resources[1].name, "r2");
    EXPECT_FALSE(resources.next_cursor.has_value());

    auto templates = client->ListResourceTemplatesAll();
    ASSERT_EQ(templates.resource_templates.size(), 2u);
    EXPECT_EQ(templates.resource_templates[0].name, "t1");
    EXPECT_EQ(templates.resource_templates[1].name, "t2");
    EXPECT_FALSE(templates.next_cursor.has_value());

    auto prompts = client->ListPromptsAll();
    ASSERT_EQ(prompts.prompts.size(), 2u);
    EXPECT_EQ(prompts.prompts[0].name, "prompt-1");
    EXPECT_EQ(prompts.prompts[1].name, "prompt-2");
    EXPECT_FALSE(prompts.next_cursor.has_value());

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, CallToolAsTaskReturnsTaskHandle) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string("2025-11-25")));
    server_handler->SetRequestHandler(methods::kCallTool,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue task(JsonValue::object_tag);
            task["taskId"] = JsonValue("t-1");
            task["status"] = JsonValue("working");
            task["createdAt"] = JsonValue("2026-01-01T00:00:00Z");
            JsonValue result(JsonValue::object_tag);
            result["resultType"] = JsonValue("task");
            result["task"] = std::move(task);
            p.set_value(std::move(result));
        });
    server_handler->SetRequestHandler(methods::kGetTask,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue content(JsonValue::object_tag);
            content["type"] = JsonValue("text");
            content["text"] = JsonValue("task-done");
            JsonValue arr(JsonValue::array_tag);
            arr.PushBack(std::move(content));
            JsonValue payload(JsonValue::object_tag);
            payload["content"] = JsonValue(std::move(arr));
            JsonValue result(JsonValue::object_tag);
            result["taskId"] = JsonValue("t-1");
            result["status"] = JsonValue("completed");
            result["resultType"] = JsonValue("task");
            result["result"] = std::move(payload);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string("2025-11-25");
    opts.enable_tasks_extension = true;
    JsonValue task_ext(JsonValue::object_tag);
    task_ext[std::string(kTasksExtensionId)] = JsonValue(JsonValue::object_tag);
    opts.extensions = std::move(task_ext);
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto handle = client->CallToolAsTask("long-tool");
    EXPECT_EQ(handle.task_id, "t-1");
    EXPECT_EQ(handle.status, "working");

    auto done = client->PollTaskToCompletion("t-1");
    EXPECT_EQ(done.task_id, "t-1");
    EXPECT_EQ(done.status, "completed");
    ASSERT_TRUE(done.result.has_value());
    auto tool_result = DeserializeCallToolResult(*done.result);
    ASSERT_EQ(tool_result.content.size(), 1u);
    auto* text = std::get_if<TextContent>(&tool_result.content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "task-done");

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, FlatTaskResultPreferredOverNested) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    server_handler->SetRequestHandler(methods::kCallTool,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue nested(JsonValue::object_tag);
            nested["taskId"] = JsonValue("t-nested");
            nested["status"] = JsonValue("working");
            nested["createdAt"] = JsonValue("2026-01-01T00:00:00Z");
            JsonValue result(JsonValue::object_tag);
            result["resultType"] = JsonValue("task");
            result["taskId"] = JsonValue("t-flat");
            result["status"] = JsonValue("working");
            result["createdAt"] = JsonValue("2026-01-01T00:00:00Z");
            result["task"] = std::move(nested);
            p.set_value(std::move(result));
        });
    server_handler->SetRequestHandler(methods::kGetTask,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue content(JsonValue::object_tag);
            content["type"] = JsonValue("text");
            content["text"] = JsonValue("flat-done");
            JsonValue arr(JsonValue::array_tag);
            arr.PushBack(std::move(content));
            JsonValue payload(JsonValue::object_tag);
            payload["content"] = JsonValue(std::move(arr));
            JsonValue result(JsonValue::object_tag);
            result["taskId"] = JsonValue("t-flat");
            result["status"] = JsonValue("completed");
            result["resultType"] = JsonValue("task");
            result["result"] = std::move(payload);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.enable_tasks_extension = true;
    JsonValue task_ext(JsonValue::object_tag);
    task_ext[std::string(kTasksExtensionId)] = JsonValue(JsonValue::object_tag);
    opts.extensions = std::move(task_ext);
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto handle = client->CallToolAsTask("flat-tool");
    EXPECT_EQ(handle.task_id, "t-flat");
    EXPECT_EQ(handle.status, "working");

    auto done = client->PollTaskToCompletion("t-flat");
    EXPECT_EQ(done.task_id, "t-flat");
    EXPECT_EQ(done.status, "completed");
    ASSERT_TRUE(done.result.has_value());
    auto tool_result = DeserializeCallToolResult(*done.result);
    ASSERT_EQ(tool_result.content.size(), 1u);
    auto* text = std::get_if<TextContent>(&tool_result.content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "flat-done");

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, CallToolAsTaskDegradesSynchronousResult) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    server_handler->SetRequestHandler(methods::kCallTool,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue content(JsonValue::object_tag);
            content["type"] = JsonValue("text");
            content["text"] = JsonValue("sync-hello");
            JsonValue arr(JsonValue::array_tag);
            arr.PushBack(std::move(content));
            JsonValue result(JsonValue::object_tag);
            result["content"] = JsonValue(std::move(arr));
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto task = client->CallToolAsTask("sync-tool");
    EXPECT_EQ(task.task_id, "<none>");
    EXPECT_EQ(task.status, "completed");
    ASSERT_TRUE(task.result.has_value());
    auto tool_result = DeserializeCallToolResult(*task.result);
    ASSERT_EQ(tool_result.content.size(), 1u);
    auto* text = std::get_if<TextContent>(&tool_result.content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "sync-hello");
    EXPECT_FALSE(tool_result.is_error);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, ClientMaxTotalTimeoutCapsProgressExtensions) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int64_t> seen_token{0};
    std::atomic<bool> has_token{false};
    std::shared_ptr<std::promise<JsonValue>> held_promise;
    server_handler->SetRequestHandler(methods::kCallTool,
        [&](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (req.meta && req.meta->IsObject()) {
                if (auto* pt = req.meta->Find("progressToken"); pt && pt->IsInt()) {
                    seen_token.store(pt->GetInt());
                    has_token.store(true);
                }
            }
            held_promise =
                std::make_shared<std::promise<JsonValue>>(std::move(p));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.max_total_timeout = std::chrono::seconds(1);
    auto client = McpClient::Create(std::move(pair.client), opts);

    RequestOptions ropts;
    ropts.on_progress = [](const ProgressNotificationParams&) {};

    auto start = std::chrono::steady_clock::now();
    auto call = std::async(std::launch::async, [&] {
        try {
            client->CallTool("slow-tool", JsonValue(JsonValue::object_tag), ropts);
            return std::exception_ptr{};
        } catch (...) {
            return std::current_exception();
        }
    });

    int extensions = 0;
    while (call.wait_for(std::chrono::milliseconds(150)) ==
           std::future_status::timeout) {
        ++extensions;
        if (has_token.load()) {
            JsonValue progress_params(JsonValue::object_tag);
            progress_params["progressToken"] =
                JsonValue(seen_token.load());
            progress_params["progress"] = JsonValue(extensions);
            server_handler->SendNotification(
                notifications::kProgress, std::move(progress_params));
        }
        ASSERT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start).count(), 10000);
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_GE(extensions, 3);
    EXPECT_GE(elapsed_ms, static_cast<int64_t>(900));
    auto exc = call.get();
    ASSERT_TRUE(exc != nullptr);
    try {
        std::rethrow_exception(exc);
    } catch (const McpError& e) {
        EXPECT_EQ(e.Code(), McpErrorCode::RequestTimeout);
    }

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, UrlElicitationInvokesHandlerAndSendsComplete) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string("2025-11-25")));

    std::promise<JsonValue> complete_received;
    auto complete_future = complete_received.get_future();
    server_handler->SetNotificationHandler(notifications::kElicitationComplete,
        [&complete_received](const JsonRpcNotification& notif) {
            complete_received.set_value(
                notif.params ? *notif.params : JsonValue(JsonValue::object_tag));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string("2025-11-25");
    auto client = McpClient::Create(std::move(pair.client), opts);

    std::atomic<bool> handler_called{false};
    client->SetUrlElicitationHandler(
        [&handler_called](const ElicitRequestParams& params) {
            handler_called.store(true);
            EXPECT_EQ(params.mode, "url");
            EXPECT_TRUE(params.url.has_value());
            EXPECT_EQ(*params.url, "https://example.com/authorize");
            EXPECT_TRUE(params.elicitation_id.has_value());
            EXPECT_EQ(*params.elicitation_id, "elicit-1");
            ElicitResult result;
            result.action = "accept";
            return result;
        });

    JsonValue elicit_params(JsonValue::object_tag);
    elicit_params["message"] = JsonValue("Open the URL and approve access");
    elicit_params["mode"] = JsonValue("url");
    elicit_params["url"] = JsonValue("https://example.com/authorize");
    elicit_params["elicitationId"] = JsonValue("elicit-1");
    auto response = server_handler->SendRequest(
        methods::kElicit, std::move(elicit_params), RequestMeta{},
        std::chrono::seconds(5)).get();

    auto* action = response.Find("action");
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->GetString(), "accept");
    EXPECT_TRUE(handler_called.load());

    ASSERT_EQ(complete_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    JsonValue complete_params = complete_future.get();
    auto* elicitation_id = complete_params.Find("elicitationId");
    ASSERT_NE(elicitation_id, nullptr);
    EXPECT_EQ(elicitation_id->GetString(), "elicit-1");

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, UrlElicitationHandlerDeclineReturnsDecline) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string("2025-11-25")));

    std::promise<JsonValue> complete_received;
    auto complete_future = complete_received.get_future();
    server_handler->SetNotificationHandler(notifications::kElicitationComplete,
        [&complete_received](const JsonRpcNotification& notif) {
            complete_received.set_value(
                notif.params ? *notif.params : JsonValue(JsonValue::object_tag));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string("2025-11-25");
    auto client = McpClient::Create(std::move(pair.client), opts);

    client->SetUrlElicitationHandler([](const ElicitRequestParams&) {
        ElicitResult result;
        result.action = "decline";
        return result;
    });

    JsonValue elicit_params(JsonValue::object_tag);
    elicit_params["message"] = JsonValue("Open the URL and approve access");
    elicit_params["mode"] = JsonValue("url");
    elicit_params["url"] = JsonValue("https://example.com/authorize");
    elicit_params["elicitationId"] = JsonValue("elicit-decline");
    auto response = server_handler->SendRequest(
        methods::kElicit, std::move(elicit_params), RequestMeta{},
        std::chrono::seconds(5)).get();

    auto* action = response.Find("action");
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->GetString(), "decline");

    ASSERT_EQ(complete_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    JsonValue complete_params = complete_future.get();
    auto* elicitation_id = complete_params.Find("elicitationId");
    ASSERT_NE(elicitation_id, nullptr);
    EXPECT_EQ(elicitation_id->GetString(), "elicit-decline");

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, UrlElicitationHandlerCancelReturnsCancel) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string("2025-11-25")));

    std::promise<JsonValue> complete_received;
    auto complete_future = complete_received.get_future();
    server_handler->SetNotificationHandler(notifications::kElicitationComplete,
        [&complete_received](const JsonRpcNotification& notif) {
            complete_received.set_value(
                notif.params ? *notif.params : JsonValue(JsonValue::object_tag));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string("2025-11-25");
    auto client = McpClient::Create(std::move(pair.client), opts);

    client->SetUrlElicitationHandler([](const ElicitRequestParams&) {
        ElicitResult result;
        result.action = "cancel";
        return result;
    });

    JsonValue elicit_params(JsonValue::object_tag);
    elicit_params["message"] = JsonValue("Open the URL and approve access");
    elicit_params["mode"] = JsonValue("url");
    elicit_params["url"] = JsonValue("https://example.com/authorize");
    elicit_params["elicitationId"] = JsonValue("elicit-cancel");
    auto response = server_handler->SendRequest(
        methods::kElicit, std::move(elicit_params), RequestMeta{},
        std::chrono::seconds(5)).get();

    auto* action = response.Find("action");
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->GetString(), "cancel");

    ASSERT_EQ(complete_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    JsonValue complete_params = complete_future.get();
    auto* elicitation_id = complete_params.Find("elicitationId");
    ASSERT_NE(elicitation_id, nullptr);
    EXPECT_EQ(elicitation_id->GetString(), "elicit-cancel");

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, UrlElicitationWithoutHandlerDeclinesAndStillSendsComplete) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string("2025-11-25")));

    std::promise<JsonValue> complete_received;
    auto complete_future = complete_received.get_future();
    server_handler->SetNotificationHandler(notifications::kElicitationComplete,
        [&complete_received](const JsonRpcNotification& notif) {
            complete_received.set_value(
                notif.params ? *notif.params : JsonValue(JsonValue::object_tag));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string("2025-11-25");
    auto client = McpClient::Create(std::move(pair.client), opts);

    JsonValue elicit_params(JsonValue::object_tag);
    elicit_params["message"] = JsonValue("Open the URL and approve access");
    elicit_params["mode"] = JsonValue("url");
    elicit_params["url"] = JsonValue("https://example.com/authorize");
    elicit_params["elicitationId"] = JsonValue("elicit-2");
    auto response = server_handler->SendRequest(
        methods::kElicit, std::move(elicit_params), RequestMeta{},
        std::chrono::seconds(5)).get();

    auto* action = response.Find("action");
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->GetString(), "decline");

    ASSERT_EQ(complete_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    JsonValue complete_params = complete_future.get();
    auto* elicitation_id = complete_params.Find("elicitationId");
    ASSERT_NE(elicitation_id, nullptr);
    EXPECT_EQ(elicitation_id->GetString(), "elicit-2");

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, SessionExpiredTriggersReinitAndReplaysOnce) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    std::atomic<int> initialize_calls{0};
    std::atomic<int> tool_calls{0};
    server_handler->SetRequestHandler(methods::kInitialize,
        [&initialize_calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            initialize_calls.fetch_add(1);
            JsonValue result(JsonValue::object_tag);
            result["protocolVersion"] = JsonValue(std::string(kLegacyProtocolVersion));
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            result["serverInfo"] = SerializeImplementation(Implementation{"raw-server", "1.0"});
            p.set_value(std::move(result));
        });
    server_handler->SetRequestHandler(methods::kCallTool,
        [&tool_calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            if (tool_calls.fetch_add(1) == 0) {
                JsonValue err(JsonValue::object_tag);
                err["code"] = JsonValue(static_cast<int64_t>(McpErrorCode::SessionExpired));
                err["message"] = JsonValue("session expired");
                p.set_value(std::move(err));
                return;
            }
            JsonValue result(JsonValue::object_tag);
            JsonValue content(JsonValue::array_tag);
            result["content"] = std::move(content);
            result["isError"] = JsonValue(false);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Legacy;
    opts.initialization_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto tool_result = client->CallTool("echo");
    EXPECT_FALSE(tool_result.is_error);
    EXPECT_EQ(initialize_calls.load(), 2);
    EXPECT_EQ(tool_calls.load(), 2);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, SecondSessionExpiredPropagatesWithoutSecondReinit) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    std::atomic<int> initialize_calls{0};
    server_handler->SetRequestHandler(methods::kInitialize,
        [&initialize_calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            initialize_calls.fetch_add(1);
            JsonValue result(JsonValue::object_tag);
            result["protocolVersion"] = JsonValue(std::string(kLegacyProtocolVersion));
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            result["serverInfo"] = SerializeImplementation(Implementation{"raw-server", "1.0"});
            p.set_value(std::move(result));
        });
    server_handler->SetRequestHandler(methods::kCallTool,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue err(JsonValue::object_tag);
            err["code"] = JsonValue(static_cast<int64_t>(McpErrorCode::SessionExpired));
            err["message"] = JsonValue("session expired");
            p.set_value(std::move(err));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Legacy;
    opts.initialization_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), opts);

    bool threw = false;
    try {
        client->CallTool("echo");
    } catch (const McpError& e) {
        threw = true;
        EXPECT_EQ(e.Code(), McpErrorCode::SessionExpired);
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(initialize_calls.load(), 2);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, ReinitDisabledPropagatesSessionExpired) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    std::atomic<int> initialize_calls{0};
    server_handler->SetRequestHandler(methods::kInitialize,
        [&initialize_calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            initialize_calls.fetch_add(1);
            JsonValue result(JsonValue::object_tag);
            result["protocolVersion"] = JsonValue(std::string(kLegacyProtocolVersion));
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            result["serverInfo"] = SerializeImplementation(Implementation{"raw-server", "1.0"});
            p.set_value(std::move(result));
        });
    server_handler->SetRequestHandler(methods::kCallTool,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue err(JsonValue::object_tag);
            err["code"] = JsonValue(static_cast<int64_t>(McpErrorCode::SessionExpired));
            err["message"] = JsonValue("session expired");
            p.set_value(std::move(err));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Legacy;
    opts.initialization_timeout = std::chrono::seconds(5);
    opts.reinit_on_expired_session = false;
    auto client = McpClient::Create(std::move(pair.client), opts);

    bool threw = false;
    try {
        client->CallTool("echo");
    } catch (const McpError& e) {
        threw = true;
        EXPECT_EQ(e.Code(), McpErrorCode::SessionExpired);
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(initialize_calls.load(), 1);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, NonSessionExpiredErrorDoesNotReinit) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    std::atomic<int> initialize_calls{0};
    server_handler->SetRequestHandler(methods::kInitialize,
        [&initialize_calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            initialize_calls.fetch_add(1);
            JsonValue result(JsonValue::object_tag);
            result["protocolVersion"] = JsonValue(std::string(kLegacyProtocolVersion));
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            result["serverInfo"] = SerializeImplementation(Implementation{"raw-server", "1.0"});
            p.set_value(std::move(result));
        });
    server_handler->SetRequestHandler(methods::kCallTool,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue err(JsonValue::object_tag);
            err["code"] = JsonValue(static_cast<int64_t>(McpErrorCode::InternalError));
            err["message"] = JsonValue("boom");
            p.set_value(std::move(err));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Legacy;
    opts.initialization_timeout = std::chrono::seconds(5);
    auto client = McpClient::Create(std::move(pair.client), opts);

    bool threw = false;
    try {
        client->CallTool("echo");
    } catch (const McpError& e) {
        threw = true;
        EXPECT_EQ(e.Code(), McpErrorCode::InternalError);
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(initialize_calls.load(), 1);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, PollTaskViaTasksGet) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string("2025-11-25")));
    std::atomic<int> get_calls{0};
    std::atomic<int> result_calls{0};
    server_handler->SetRequestHandler(methods::kGetTask,
        [&get_calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            int n = get_calls.fetch_add(1);
            JsonValue result(JsonValue::object_tag);
            result["taskId"] = JsonValue("t-poll");
            result["status"] = JsonValue(n == 0 ? "working" : "completed");
            result["resultType"] = JsonValue("task");
            if (n > 0) {
                JsonValue payload(JsonValue::object_tag);
                JsonValue content(JsonValue::object_tag);
                content["type"] = JsonValue("text");
                content["text"] = JsonValue("done");
                JsonValue arr(JsonValue::array_tag);
                arr.PushBack(std::move(content));
                payload["content"] = JsonValue(std::move(arr));
                result["result"] = std::move(payload);
            }
            p.set_value(std::move(result));
        });
    server_handler->SetRequestHandler(methods::kGetTaskPayload,
        [&result_calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            result_calls.fetch_add(1);
            p.set_value(JsonValue(JsonValue::object_tag));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string("2025-11-25");
    opts.enable_tasks_extension = true;
    JsonValue task_ext(JsonValue::object_tag);
    task_ext[std::string(kTasksExtensionId)] = JsonValue(JsonValue::object_tag);
    opts.extensions = std::move(task_ext);
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto done = client->PollTaskToCompletion("t-poll",
        std::chrono::milliseconds(10), std::chrono::seconds(10));
    EXPECT_EQ(done.task_id, "t-poll");
    EXPECT_EQ(done.status, "completed");
    ASSERT_TRUE(done.result.has_value());
    EXPECT_GE(get_calls.load(), 2);
    EXPECT_EQ(result_calls.load(), 0);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, UpdateTaskCarriesInputResponses) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string("2025-11-25")));
    std::atomic<bool> saw_input_responses{false};
    server_handler->SetRequestHandler(methods::kUpdateTask,
        [&saw_input_responses](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (req.params && req.params->IsObject()) {
                auto* tid = req.params->Find("taskId");
                auto* ir = req.params->Find("inputResponses");
                saw_input_responses.store(tid && tid->IsString() &&
                    tid->GetString() == "t-input" && ir && ir->IsObject() &&
                    ir->Contains("q"));
            }
            p.set_value(JsonValue(JsonValue::object_tag));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string("2025-11-25");
    opts.enable_tasks_extension = true;
    auto client = McpClient::Create(std::move(pair.client), opts);

    JsonValue responses(JsonValue::object_tag);
    JsonValue answer(JsonValue::object_tag);
    answer["answer"] = JsonValue("yes");
    responses["q"] = std::move(answer);
    EXPECT_NO_THROW(client->UpdateTask("t-input", std::nullopt, std::move(responses)));
    EXPECT_TRUE(saw_input_responses.load());

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, UndeclaredExtensionReceivesTaskThrows) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    server_handler->SetRequestHandler(methods::kCallTool,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            JsonValue task(JsonValue::object_tag);
            task["taskId"] = JsonValue("t-undeclared");
            task["status"] = JsonValue("working");
            task["createdAt"] = JsonValue("2026-01-01T00:00:00Z");
            JsonValue result(JsonValue::object_tag);
            result["resultType"] = JsonValue("task");
            result["task"] = std::move(task);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    bool call_threw = false;
    try {
        client->CallTool("long-tool", JsonValue(JsonValue::object_tag));
    } catch (const McpError& e) {
        call_threw = true;
        EXPECT_EQ(e.Code(), McpErrorCode::HandlerError);
    }
    EXPECT_TRUE(call_threw);

    bool as_task_threw = false;
    try {
        client->CallToolAsTask("long-tool");
    } catch (const McpError& e) {
        as_task_threw = true;
        EXPECT_EQ(e.Code(), McpErrorCode::HandlerError);
    }
    EXPECT_TRUE(as_task_threw);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, ListSkillsRoundTrip) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> calls{0};
    server_handler->SetRequestHandler(methods::kListSkills,
        [&calls](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            calls.fetch_add(1);
            std::string cursor;
            if (req.params && req.params->IsObject()) {
                if (auto* c = req.params->Find("cursor"); c && c->IsString())
                    cursor = c->GetString();
            }
            std::string uri = cursor == "c1"
                ? "skill://other/SKILL.md" : "skill://code-review/SKILL.md";
            std::string name = cursor == "c1" ? "other" : "code-review";
            JsonValue skill(JsonValue::object_tag);
            skill["uri"] = JsonValue(uri);
            JsonValue fm(JsonValue::object_tag);
            fm["name"] = JsonValue(name);
            fm["description"] = JsonValue("Review code.");
            skill["frontmatter"] = std::move(fm);
            skill["resources"] = JsonValue("dynamic");
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            arr.PushBack(std::move(skill));
            result["skills"] = JsonValue(std::move(arr));
            result["ttlMs"] = JsonValue(int64_t(60000));
            result["cacheScope"] = JsonValue("private");
            result["resultType"] = JsonValue("complete");
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.enable_skills_extension = true;
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto r1 = client->ListSkills();
    ASSERT_EQ(r1.skills.size(), 1u);
    EXPECT_EQ(r1.skills[0].uri, "skill://code-review/SKILL.md");
    ASSERT_TRUE(r1.ttl_ms.has_value());
    EXPECT_EQ(*r1.ttl_ms, int64_t(60000));
    ASSERT_TRUE(r1.cache_scope.has_value());
    EXPECT_EQ(*r1.cache_scope, "private");
    EXPECT_EQ(calls.load(), 1);

    auto r2 = client->ListSkills();
    ASSERT_EQ(r2.skills.size(), 1u);
    EXPECT_EQ(r2.skills[0].uri, "skill://code-review/SKILL.md");
    EXPECT_EQ(calls.load(), 1);

    auto page = client->ListSkills("c1");
    ASSERT_EQ(page.skills.size(), 1u);
    EXPECT_EQ(page.skills[0].uri, "skill://other/SKILL.md");
    EXPECT_EQ(calls.load(), 2);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, SkillDirectFetchWithoutList) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> get_calls{0};
    std::atomic<int> dir_calls{0};
    std::atomic<int> res_calls{0};
    std::atomic<int> list_calls{0};
    server_handler->SetRequestHandler(methods::kGetSkill,
        [&get_calls](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            get_calls.fetch_add(1);
            auto uri = req.params->Find("uri")->GetString();
            JsonValue skill(JsonValue::object_tag);
            skill["uri"] = JsonValue(uri);
            JsonValue fm(JsonValue::object_tag);
            fm["name"] = JsonValue("code-review");
            skill["frontmatter"] = std::move(fm);
            skill["resources"] = JsonValue("dynamic");
            JsonValue result(JsonValue::object_tag);
            result["skill"] = std::move(skill);
            result["ttlMs"] = JsonValue(int64_t(60000));
            result["cacheScope"] = JsonValue("public");
            result["resultType"] = JsonValue("complete");
            p.set_value(std::move(result));
        });
    server_handler->SetRequestHandler(methods::kReadDirectory,
        [&dir_calls](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            dir_calls.fetch_add(1);
            auto uri = req.params->Find("uri")->GetString();
            EXPECT_EQ(uri, "skill://code-review/references");
            JsonValue child(JsonValue::object_tag);
            child["uri"] = JsonValue("skill://code-review/references/checklist.md");
            child["name"] = JsonValue("checklist.md");
            child["mimeType"] = JsonValue("text/markdown");
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            arr.PushBack(std::move(child));
            result["resources"] = JsonValue(std::move(arr));
            result["ttlMs"] = JsonValue(int64_t(60000));
            result["cacheScope"] = JsonValue("public");
            result["resultType"] = JsonValue("complete");
            p.set_value(std::move(result));
        });
    server_handler->SetRequestHandler(methods::kReadResource,
        [&res_calls](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            res_calls.fetch_add(1);
            auto uri = req.params->Find("uri")->GetString();
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            JsonValue c(JsonValue::object_tag);
            c["uri"] = JsonValue(uri);
            c["text"] = JsonValue("skill body");
            arr.PushBack(std::move(c));
            result["contents"] = JsonValue(std::move(arr));
            JsonValue hint(JsonValue::object_tag);
            hint["ttlMs"] = JsonValue(int64_t(60000));
            result["cacheHint"] = std::move(hint);
            p.set_value(std::move(result));
        });
    server_handler->SetRequestHandler(methods::kListSkills,
        [&list_calls](const JsonRpcRequest&, std::promise<JsonValue> p) {
            list_calls.fetch_add(1);
            p.set_value(JsonValue(JsonValue::object_tag));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.enable_skills_extension = true;
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto got = client->GetSkill("skill://code-review/SKILL.md");
    EXPECT_EQ(got.skill.uri, "skill://code-review/SKILL.md");
    EXPECT_EQ(get_calls.load(), 1);

    auto dir = client->ReadDirectory("skill://code-review/references");
    ASSERT_EQ(dir.resources.size(), 1u);
    EXPECT_EQ(dir.resources[0].uri, "skill://code-review/references/checklist.md");
    EXPECT_EQ(dir_calls.load(), 1);

    auto res = client->ReadResource("skill://code-review/SKILL.md");
    ASSERT_EQ(res.contents.size(), 1u);
    EXPECT_EQ(res_calls.load(), 1);

    EXPECT_EQ(list_calls.load(), 0);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, TtlExpiryRefetch) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> calls{0};
    server_handler->SetRequestHandler(methods::kGetSkill,
        [&calls](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            calls.fetch_add(1);
            auto uri = req.params->Find("uri")->GetString();
            JsonValue skill(JsonValue::object_tag);
            skill["uri"] = JsonValue(uri);
            skill["frontmatter"] = JsonValue(JsonValue::object_tag);
            skill["resources"] = JsonValue("dynamic");
            JsonValue result(JsonValue::object_tag);
            result["skill"] = std::move(skill);
            result["ttlMs"] = JsonValue(int64_t(50));
            result["cacheScope"] = JsonValue("private");
            result["resultType"] = JsonValue("complete");
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.enable_skills_extension = true;
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto r1 = client->GetSkill("skill://code-review/SKILL.md");
    EXPECT_EQ(r1.skill.uri, "skill://code-review/SKILL.md");
    EXPECT_EQ(calls.load(), 1);

    auto r2 = client->GetSkill("skill://code-review/SKILL.md");
    EXPECT_EQ(r2.skill.uri, "skill://code-review/SKILL.md");
    EXPECT_EQ(calls.load(), 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto r3 = client->GetSkill("skill://code-review/SKILL.md");
    EXPECT_EQ(r3.skill.uri, "skill://code-review/SKILL.md");
    EXPECT_EQ(calls.load(), 2);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, ElicitationApplyDefaultsFillsFiveFieldKinds) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLegacyProtocolVersion);
    ClientCapabilities caps;
    ElicitationCapability elicitation;
    elicitation.form = JsonValue(JsonValue::Object{{"applyDefaults", JsonValue(true)}});
    caps.elicitation = elicitation;
    opts.capabilities = caps;
    auto client = McpClient::Create(std::move(pair.client), opts);

    client->SetElicitationHandler([](const ElicitRequestParams&) {
        ElicitResult r;
        r.action = "accept";
        r.content = JsonValue(JsonValue::object_tag);
        return r;
    });

    JsonValue props(JsonValue::object_tag);
    JsonValue name_prop(JsonValue::object_tag);
    name_prop["type"] = JsonValue("string");
    name_prop["default"] = JsonValue("octocat");
    props["name"] = std::move(name_prop);
    JsonValue age_prop(JsonValue::object_tag);
    age_prop["type"] = JsonValue("integer");
    age_prop["default"] = JsonValue(30);
    props["age"] = std::move(age_prop);
    JsonValue score_prop(JsonValue::object_tag);
    score_prop["type"] = JsonValue("number");
    score_prop["default"] = JsonValue(9.5);
    props["score"] = std::move(score_prop);
    JsonValue color_prop(JsonValue::object_tag);
    color_prop["type"] = JsonValue("string");
    JsonValue colors(JsonValue::array_tag);
    colors.PushBack(JsonValue("red"));
    colors.PushBack(JsonValue("green"));
    color_prop["enum"] = std::move(colors);
    color_prop["default"] = JsonValue("green");
    props["color"] = std::move(color_prop);
    JsonValue active_prop(JsonValue::object_tag);
    active_prop["type"] = JsonValue("boolean");
    active_prop["default"] = JsonValue(true);
    props["active"] = std::move(active_prop);
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(props);

    ElicitRequestParams req;
    req.message = "defaults";
    req.requested_schema = schema;

    RequestMeta elicitation_meta;
    elicitation_meta.protocol_version = std::string(kLegacyProtocolVersion);

    auto result_json = server_handler->SendRequest(
        methods::kElicit, SerializeElicitRequestParams(req),
        elicitation_meta).get();
    auto result = DeserializeElicitResult(result_json);
    EXPECT_EQ(result.action, "accept");
    ASSERT_TRUE(result.content.has_value());
    ASSERT_TRUE(result.content->IsObject());
    EXPECT_EQ((*result.content)["name"].GetString(), "octocat");
    EXPECT_EQ((*result.content)["age"].GetInt(), 30);
    EXPECT_EQ((*result.content)["score"].GetDouble(), 9.5);
    EXPECT_EQ((*result.content)["color"].GetString(), "green");
    EXPECT_TRUE((*result.content)["active"].GetBool());

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, ElicitationApplyDefaultsDisabledLeavesContentEmpty) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLegacyProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    client->SetElicitationHandler([](const ElicitRequestParams&) {
        ElicitResult r;
        r.action = "accept";
        r.content = JsonValue(JsonValue::object_tag);
        return r;
    });

    JsonValue name_prop(JsonValue::object_tag);
    name_prop["type"] = JsonValue("string");
    name_prop["default"] = JsonValue("octocat");
    JsonValue props(JsonValue::object_tag);
    props["name"] = std::move(name_prop);
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(props);

    ElicitRequestParams req;
    req.message = "defaults";
    req.requested_schema = schema;

    RequestMeta elicitation_meta;
    elicitation_meta.protocol_version = std::string(kLegacyProtocolVersion);

    auto result_json = server_handler->SendRequest(
        methods::kElicit, SerializeElicitRequestParams(req),
        elicitation_meta).get();
    auto result = DeserializeElicitResult(result_json);
    EXPECT_EQ(result.action, "accept");
    ASSERT_TRUE(result.content.has_value());
    ASSERT_TRUE(result.content->IsObject());
    EXPECT_TRUE(result.content->Empty());

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, ElicitationApplyDefaultsKeepsExistingValues) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLegacyProtocolVersion);
    ClientCapabilities caps;
    ElicitationCapability elicitation;
    elicitation.form = JsonValue(JsonValue::Object{{"applyDefaults", JsonValue(true)}});
    caps.elicitation = elicitation;
    opts.capabilities = caps;
    auto client = McpClient::Create(std::move(pair.client), opts);

    client->SetElicitationHandler([](const ElicitRequestParams&) {
        ElicitResult r;
        r.action = "accept";
        JsonValue content(JsonValue::object_tag);
        content["name"] = JsonValue("Alice");
        r.content = std::move(content);
        return r;
    });

    JsonValue props(JsonValue::object_tag);
    JsonValue name_prop(JsonValue::object_tag);
    name_prop["type"] = JsonValue("string");
    name_prop["default"] = JsonValue("octocat");
    props["name"] = std::move(name_prop);
    JsonValue age_prop(JsonValue::object_tag);
    age_prop["type"] = JsonValue("integer");
    age_prop["default"] = JsonValue(30);
    props["age"] = std::move(age_prop);
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(props);

    ElicitRequestParams req;
    req.message = "defaults";
    req.requested_schema = schema;

    RequestMeta elicitation_meta;
    elicitation_meta.protocol_version = std::string(kLegacyProtocolVersion);

    auto result_json = server_handler->SendRequest(
        methods::kElicit, SerializeElicitRequestParams(req),
        elicitation_meta).get();
    auto result = DeserializeElicitResult(result_json);
    ASSERT_TRUE(result.content.has_value());
    EXPECT_EQ((*result.content)["name"].GetString(), "Alice");
    EXPECT_EQ((*result.content)["age"].GetInt(), 30);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, ElicitationApplyDefaultsIgnoresDecline) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLegacyProtocolVersion);
    ClientCapabilities caps;
    ElicitationCapability elicitation;
    elicitation.form = JsonValue(JsonValue::Object{{"applyDefaults", JsonValue(true)}});
    caps.elicitation = elicitation;
    opts.capabilities = caps;
    auto client = McpClient::Create(std::move(pair.client), opts);

    client->SetElicitationHandler([](const ElicitRequestParams&) {
        ElicitResult r;
        r.action = "decline";
        return r;
    });

    JsonValue name_prop(JsonValue::object_tag);
    name_prop["type"] = JsonValue("string");
    name_prop["default"] = JsonValue("octocat");
    JsonValue props(JsonValue::object_tag);
    props["name"] = std::move(name_prop);
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(props);

    ElicitRequestParams req;
    req.message = "defaults";
    req.requested_schema = schema;

    RequestMeta elicitation_meta;
    elicitation_meta.protocol_version = std::string(kLegacyProtocolVersion);

    auto result_json = server_handler->SendRequest(
        methods::kElicit, SerializeElicitRequestParams(req),
        elicitation_meta).get();
    auto result = DeserializeElicitResult(result_json);
    EXPECT_EQ(result.action, "decline");
    EXPECT_FALSE(result.content.has_value());

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, ElicitationApplyDefaultsRecursesNestedAndVariants) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLegacyProtocolVersion)));
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLegacyProtocolVersion);
    ClientCapabilities caps;
    ElicitationCapability elicitation;
    elicitation.form = JsonValue(JsonValue::Object{{"applyDefaults", JsonValue(true)}});
    caps.elicitation = elicitation;
    opts.capabilities = caps;
    auto client = McpClient::Create(std::move(pair.client), opts);

    client->SetElicitationHandler([](const ElicitRequestParams&) {
        ElicitResult r;
        r.action = "accept";
        JsonValue content(JsonValue::object_tag);
        content["address"] = JsonValue(JsonValue::object_tag);
        r.content = std::move(content);
        return r;
    });

    JsonValue city_prop(JsonValue::object_tag);
    city_prop["type"] = JsonValue("string");
    city_prop["default"] = JsonValue("Paris");
    JsonValue address_props(JsonValue::object_tag);
    address_props["city"] = std::move(city_prop);
    JsonValue address_prop(JsonValue::object_tag);
    address_prop["type"] = JsonValue("object");
    address_prop["properties"] = std::move(address_props);
    JsonValue props(JsonValue::object_tag);
    props["address"] = std::move(address_prop);
    JsonValue nick_prop(JsonValue::object_tag);
    nick_prop["type"] = JsonValue("string");
    nick_prop["default"] = JsonValue("bob");
    JsonValue nick_props(JsonValue::object_tag);
    nick_props["nick"] = std::move(nick_prop);
    JsonValue variant(JsonValue::object_tag);
    variant["type"] = JsonValue("object");
    variant["properties"] = std::move(nick_props);
    JsonValue any_of(JsonValue::array_tag);
    any_of.PushBack(std::move(variant));
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(props);
    schema["anyOf"] = std::move(any_of);

    ElicitRequestParams req;
    req.message = "defaults";
    req.requested_schema = schema;

    RequestMeta elicitation_meta;
    elicitation_meta.protocol_version = std::string(kLegacyProtocolVersion);

    auto result_json = server_handler->SendRequest(
        methods::kElicit, SerializeElicitRequestParams(req),
        elicitation_meta).get();
    auto result = DeserializeElicitResult(result_json);
    ASSERT_TRUE(result.content.has_value());
    ASSERT_TRUE(result.content->IsObject());
    auto* address = result.content->Find("address");
    ASSERT_NE(address, nullptr);
    ASSERT_TRUE(address->IsObject());
    EXPECT_EQ((*address)["city"].GetString(), "Paris");
    EXPECT_EQ((*result.content)["nick"].GetString(), "bob");

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, MrtrElicitationApplyDefaultsFillsInputResponses) {
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> round{0};
    std::atomic<bool> saw_default{false};
    server_handler->SetRequestHandler(methods::kCallTool,
        [&round, &saw_default](
            const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (round.load() == 0) {
                round.store(1);
                JsonValue name_prop(JsonValue::object_tag);
                name_prop["type"] = JsonValue("string");
                name_prop["default"] = JsonValue("octocat");
                JsonValue props(JsonValue::object_tag);
                props["name"] = std::move(name_prop);
                JsonValue schema(JsonValue::object_tag);
                schema["type"] = JsonValue("object");
                schema["properties"] = std::move(props);
                ElicitRequestParams params;
                params.message = "What is your name?";
                params.requested_schema = schema;
                InputRequiredResult ir;
                ir.input_requests["info"] = MakeInputRequestForElicitation(params);
                p.set_value(SerializeInputRequiredResult(ir));
                return;
            }
            round.store(2);
            auto* responses = req.params ? req.params->Find("inputResponses") : nullptr;
            auto* info = responses && responses->IsObject() ? responses->Find("info") : nullptr;
            auto* content = info && info->IsObject() ? info->Find("content") : nullptr;
            auto* name = content && content->IsObject() ? content->Find("name") : nullptr;
            saw_default.store(name && name->IsString() && name->GetString() == "octocat");
            JsonValue result(JsonValue::object_tag);
            result["structuredContent"] = JsonValue(JsonValue::object_tag);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.input_required_config = ClientOptions::InputRequiredConfig{};
    ClientCapabilities caps;
    ElicitationCapability elicitation;
    elicitation.form = JsonValue(JsonValue::Object{{"applyDefaults", JsonValue(true)}});
    caps.elicitation = elicitation;
    opts.capabilities = caps;
    auto client = McpClient::Create(std::move(pair.client), opts);

    client->SetElicitationHandler([](const ElicitRequestParams&) {
        ElicitResult r;
        r.action = "accept";
        r.content = JsonValue(JsonValue::object_tag);
        return r;
    });

    client->CallTool("echo", JsonValue(JsonValue::object_tag));
    EXPECT_TRUE(saw_default.load());
    EXPECT_EQ(round.load(), 2);

    client->Close();
    server_handler->Close();
}

TEST(McpClientTest, LegacyHandshakeMetaMatchesBodyVersion) {
    auto pair = InMemoryTransport::CreatePair();
    std::optional<JsonValue> captured_meta;
    std::string captured_body_version;
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    server_handler->SetRequestHandler(methods::kInitialize,
        [&](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (req.meta) captured_meta = *req.meta;
            else captured_meta = std::nullopt;
            if (req.params && req.params->IsObject()) {
                if (auto* v = req.params->Find("protocolVersion");
                    v && v->IsString())
                    captured_body_version = v->GetString();
            }
            JsonValue result(JsonValue::object_tag);
            result["protocolVersion"] = JsonValue(std::string(kLegacyProtocolVersion));
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            result["serverInfo"] =
                SerializeImplementation(Implementation{"legacy-server", "1.0"});
            p.set_value(std::move(result));
        });
    server_handler->Start();

    auto client_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client_handler->Start();

    auto result = VersionNegotiation::HandshakeInitialize(
        *client_handler, Implementation{"test-client", "1.0"}, std::nullopt,
        std::chrono::seconds(5));
    EXPECT_EQ(result.protocol_version, std::string(kLegacyProtocolVersion));
    EXPECT_EQ(captured_body_version, std::string(kLegacyProtocolVersion));

    std::string meta_version;
    if (captured_meta && captured_meta->IsObject()) {
        if (auto* pv = captured_meta->Find("io.modelcontextprotocol/protocolVersion");
            pv && pv->IsString())
            meta_version = pv->GetString();
    }
    EXPECT_NE(meta_version, std::string(kLatestProtocolVersion));
    if (!meta_version.empty()) {
        EXPECT_EQ(meta_version, captured_body_version);
    }

    client_handler->Close();
    server_handler->Close();
}

TEST(McpClientTest, ModernDiscoverMetaUnaffected) {
    auto pair = InMemoryTransport::CreatePair();
    std::optional<JsonValue> captured_meta;
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    server_handler->SetRequestHandler(methods::kDiscover,
        [&](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (req.meta) captured_meta = *req.meta;
            else captured_meta = std::nullopt;
            JsonValue result(JsonValue::object_tag);
            JsonValue supported(JsonValue::array_tag);
            supported.PushBack(JsonValue(std::string(kLatestProtocolVersion)));
            result["supportedVersions"] = std::move(supported);
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            result["serverInfo"] =
                SerializeImplementation(Implementation{"modern-server", "1.0"});
            p.set_value(std::move(result));
        });
    server_handler->Start();

    auto client_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client_handler->Start();

    ClientOptions opts;
    auto discovered = VersionNegotiation::ProbeDiscover(
        *client_handler, std::string(kLatestProtocolVersion),
        std::chrono::seconds(5), opts);
    ASSERT_TRUE(discovered.has_value());
    ASSERT_TRUE(captured_meta.has_value());
    ASSERT_TRUE(captured_meta->IsObject());
    auto* pv = captured_meta->Find("io.modelcontextprotocol/protocolVersion");
    ASSERT_NE(pv, nullptr);
    ASSERT_TRUE(pv->IsString());
    EXPECT_EQ(pv->GetString(), std::string(kLatestProtocolVersion));

    client_handler->Close();
    server_handler->Close();
}

TEST(McpClientTest, ClientOptionsElicitationHandlerDeclaredInDiscoverMeta) {
    auto pair = InMemoryTransport::CreatePair();
    std::optional<JsonValue> captured_meta;
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    server_handler->SetRequestHandler(methods::kDiscover,
        [&captured_meta](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            captured_meta = req.meta;
            JsonValue result(JsonValue::object_tag);
            JsonValue supported(JsonValue::array_tag);
            supported.PushBack(JsonValue(std::string(kLatestProtocolVersion)));
            result["supportedVersions"] = std::move(supported);
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            result["serverInfo"] =
                SerializeImplementation(Implementation{"modern-server", "1.0"});
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Auto;
    opts.discover_probe_timeout = std::chrono::seconds(5);
    opts.elicitation_handler = [](const ElicitRequestParams&) {
        return ElicitResult{};
    };
    auto client = McpClient::Create(std::move(pair.client), opts);

    EXPECT_TRUE(client->IsModernProtocol());
    ASSERT_TRUE(captured_meta.has_value());
    ASSERT_TRUE(captured_meta->IsObject());
    auto* cc = captured_meta->Find("io.modelcontextprotocol/clientCapabilities");
    ASSERT_NE(cc, nullptr);
    auto caps = DeserializeClientCapabilities(*cc);
    ASSERT_TRUE(caps.elicitation.has_value());
    ASSERT_TRUE(caps.elicitation->form.has_value());
    EXPECT_TRUE(caps.elicitation->form->IsObject());
    EXPECT_FALSE(caps.sampling.has_value());
    EXPECT_FALSE(caps.roots.has_value());

    client->Close();
    server_handler->Close();
}

struct PingMetaRecorder : public ITransport {
    explicit PingMetaRecorder(std::shared_ptr<ITransport> inner)
        : inner_(std::move(inner)) {}
    std::string_view SessionId() const override { return inner_->SessionId(); }
    MessageChannel& GetMessageChannel() override { return inner_->GetMessageChannel(); }
    void SendMessageAsync(JsonRpcMessage message) override {
        if (const auto* req = AsRequest(message)) {
            if (req->method == methods::kPing) {
                std::lock_guard<std::mutex> lk(mutex_);
                saw_ping_ = true;
                ping_meta_ = req->meta;
            }
        }
        inner_->SendMessageAsync(std::move(message));
    }
    void Close() override { inner_->Close(); }
    bool IsStateless() const override { return inner_->IsStateless(); }
    void Start() override { inner_->Start(); }
    bool SawPing() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return saw_ping_;
    }
    std::optional<JsonValue> PingMeta() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return ping_meta_;
    }
    std::shared_ptr<ITransport> inner_;
    mutable std::mutex mutex_;
    bool saw_ping_ = false;
    std::optional<JsonValue> ping_meta_;
};

TEST(McpClientTest, LegacyPingMetaNotModern) {
    {
        auto pair = InMemoryTransport::CreatePair();
        auto recorder = std::make_shared<PingMetaRecorder>(std::move(pair.client));
        ServerOptions sopts;
        sopts.server_info = Implementation{"ping-legacy-server", "1.0"};
        auto server = McpServer::Create(std::move(pair.server), sopts);
        ClientOptions opts;
        opts.connect_mode = ConnectMode::Legacy;
        opts.initialization_timeout = std::chrono::seconds(5);
        auto client = McpClient::Create(recorder, opts);
        ASSERT_FALSE(client->IsModernProtocol());
#if defined(__clang__) || defined(__GNUC__)
_Pragma("GCC diagnostic push")
_Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#endif
#if defined(_MSC_VER)
__pragma(warning(push))
__pragma(warning(disable : 4996))
#endif
        ASSERT_NO_THROW(client->Ping());
#if defined(__clang__) || defined(__GNUC__)
_Pragma("GCC diagnostic pop")
#endif
#if defined(_MSC_VER)
__pragma(warning(pop))
#endif
        ASSERT_TRUE(recorder->SawPing());
        std::string meta_version;
        if (auto meta = recorder->PingMeta(); meta && meta->IsObject()) {
            if (auto* pv = meta->Find("io.modelcontextprotocol/protocolVersion");
                pv && pv->IsString())
                meta_version = pv->GetString();
        }
        EXPECT_NE(meta_version, std::string(kLatestProtocolVersion));
        client->Close();
        server->Close();
    }
    {
        auto pair = InMemoryTransport::CreatePair();
        auto recorder = std::make_shared<PingMetaRecorder>(std::move(pair.client));
        ServerOptions sopts;
        sopts.server_info = Implementation{"ping-modern-server", "1.0"};
        auto server = McpServer::Create(std::move(pair.server), sopts);
        ClientOptions opts;
        opts.connect_mode = ConnectMode::Pin;
        auto client = McpClient::Create(recorder, opts);
        ASSERT_TRUE(client->IsModernProtocol());
#if defined(__clang__) || defined(__GNUC__)
_Pragma("GCC diagnostic push")
_Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#endif
#if defined(_MSC_VER)
__pragma(warning(push))
__pragma(warning(disable : 4996))
#endif
        EXPECT_THROW(client->Ping(), McpError);
#if defined(__clang__) || defined(__GNUC__)
_Pragma("GCC diagnostic pop")
#endif
#if defined(_MSC_VER)
__pragma(warning(pop))
#endif
        ASSERT_TRUE(recorder->SawPing());
        auto meta = recorder->PingMeta();
        ASSERT_TRUE(meta.has_value());
        ASSERT_TRUE(meta->IsObject());
        auto* pv = meta->Find("io.modelcontextprotocol/protocolVersion");
        ASSERT_NE(pv, nullptr);
        ASSERT_TRUE(pv->IsString());
        EXPECT_EQ(pv->GetString(), std::string(kLatestProtocolVersion));
        client->Close();
        server->Close();
    }
}

TEST(McpClientTest, SearchToolsFiltersByQueryCaseInsensitive) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"search-server", "1.0.0"};
    sopts.enable_progressive_discovery = true;
    auto server = McpServer::Create(std::move(pair.server), sopts);
    auto add = [&](const std::string& name, const std::string& desc) {
        ToolOptions topts;
        topts.Description(desc);
        server->RegisterTool(name, topts,
            std::function<CallToolResult(const Ctx&)>(
                [](const Ctx&) -> CallToolResult { return CallToolResult{}; }));
    };
    add("weather-now", "Get current weather");
    add("WEATHER-forecast", "forecast");
    add("calc-add", "add two numbers");
    add("journal", "daily WEATHER diary");

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Auto;
    opts.discover_probe_timeout = std::chrono::seconds(5);
    opts.enable_progressive_discovery = true;
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto r = client->SearchTools("weather");
    ASSERT_EQ(r.tools.size(), 3u);
    EXPECT_EQ(r.tools[0].name, "WEATHER-forecast");
    EXPECT_EQ(r.tools[1].name, "journal");
    EXPECT_EQ(r.tools[2].name, "weather-now");

    auto rf = client->SearchTools("FORECAST");
    ASSERT_EQ(rf.tools.size(), 1u);
    EXPECT_EQ(rf.tools[0].name, "WEATHER-forecast");

    auto all = client->SearchTools(std::nullopt);
    EXPECT_EQ(all.tools.size(), 4u);

    client->Close();
    server->Close();
}

TEST(McpClientTest, SearchToolsClampsOutOfRangePageSize) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"search-server", "1.0.0"};
    sopts.enable_progressive_discovery = true;
    auto server = McpServer::Create(std::move(pair.server), sopts);
    auto add = [&](const std::string& name) {
        ToolOptions topts;
        topts.Description("clamp tool");
        server->RegisterTool(name, topts,
            std::function<CallToolResult(const Ctx&)>(
                [](const Ctx&) -> CallToolResult { return CallToolResult{}; }));
    };
    add("clamp-a");
    add("clamp-b");
    add("clamp-c");

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Auto;
    opts.discover_probe_timeout = std::chrono::seconds(5);
    opts.enable_progressive_discovery = true;
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto zero = client->SearchTools(std::nullopt, 0);
    EXPECT_EQ(zero.tools.size(), 3u);
    auto neg = client->SearchTools(std::nullopt, -5);
    EXPECT_EQ(neg.tools.size(), 3u);
    auto huge = client->SearchTools(std::nullopt, 1000);
    EXPECT_EQ(huge.tools.size(), 3u);

    client->Close();
    server->Close();
}

TEST(McpClientTest, SearchToolsAggregatesPages) {
    auto pair = InMemoryTransport::CreatePair();
    ServerOptions sopts;
    sopts.server_info = Implementation{"search-server", "1.0.0"};
    sopts.enable_progressive_discovery = true;
    auto server = McpServer::Create(std::move(pair.server), sopts);
    auto add = [&](const std::string& name) {
        ToolOptions topts;
        topts.Description("paged tool");
        server->RegisterTool(name, topts,
            std::function<CallToolResult(const Ctx&)>(
                [](const Ctx&) -> CallToolResult { return CallToolResult{}; }));
    };
    add("pagetool-1");
    add("pagetool-2");
    add("pagetool-3");
    add("pagetool-4");
    add("pagetool-5");
    add("unrelated");

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Auto;
    opts.discover_probe_timeout = std::chrono::seconds(5);
    opts.enable_progressive_discovery = true;
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto r = client->SearchTools("pagetool", 2);
    ASSERT_EQ(r.tools.size(), 5u);
    EXPECT_EQ(r.tools[0].name, "pagetool-1");
    EXPECT_EQ(r.tools[1].name, "pagetool-2");
    EXPECT_EQ(r.tools[2].name, "pagetool-3");
    EXPECT_EQ(r.tools[3].name, "pagetool-4");
    EXPECT_EQ(r.tools[4].name, "pagetool-5");
    EXPECT_FALSE(r.next_cursor.has_value());

    client->Close();
    server->Close();
}

TEST(McpClientTest, Etag304HitReturnsCachedBody) {
    SetEtagCacheEnabled(true);
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> calls{0};
    std::atomic<bool> saw_conditional{false};
    server_handler->SetRequestHandler(methods::kReadResource,
        [&calls, &saw_conditional](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            calls.fetch_add(1);
            std::string inm;
            if (req.meta && req.meta->IsObject()) {
                if (auto* v = req.meta->Find("ifNoneMatch"); v && v->IsString())
                    inm = v->GetString();
            }
            if (!inm.empty()) saw_conditional.store(true);
            if (inm == "etag-v1") {
                JsonValue result(JsonValue::object_tag);
                result["notModified"] = JsonValue(true);
                result["etag"] = JsonValue("etag-v1");
                p.set_value(std::move(result));
                return;
            }
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            JsonValue c(JsonValue::object_tag);
            c["uri"] = JsonValue("uri-a");
            c["text"] = JsonValue("v1-body");
            arr.PushBack(std::move(c));
            result["contents"] = JsonValue(std::move(arr));
            result["etag"] = JsonValue("etag-v1");
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    CacheableRequestOptions ropts;
    ropts.cache_mode = "bypass";
    auto r1 = client->ReadResource("uri-a", ropts);
    ASSERT_EQ(r1.contents.size(), 1u);
    EXPECT_EQ(calls.load(), 1);
    auto r2 = client->ReadResource("uri-a", ropts);
    ASSERT_EQ(r2.contents.size(), 1u);
    EXPECT_EQ(calls.load(), 2);
    EXPECT_TRUE(saw_conditional.load());
    auto* t1 = std::get_if<TextResourceContents>(&r1.contents[0]);
    auto* t2 = std::get_if<TextResourceContents>(&r2.contents[0]);
    ASSERT_NE(t1, nullptr);
    ASSERT_NE(t2, nullptr);
    EXPECT_EQ(t2->text, "v1-body");
    EXPECT_EQ(t2->text, t1->text);

    client->Close();
    server_handler->Close();
    SetEtagCacheEnabled(false);
}

TEST(McpClientTest, EtagMissPassesThroughNewBody) {
    SetEtagCacheEnabled(true);
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> calls{0};
    std::atomic<bool> saw_conditional{false};
    server_handler->SetRequestHandler(methods::kReadResource,
        [&calls, &saw_conditional](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            calls.fetch_add(1);
            std::string inm;
            if (req.meta && req.meta->IsObject()) {
                if (auto* v = req.meta->Find("ifNoneMatch"); v && v->IsString())
                    inm = v->GetString();
            }
            if (!inm.empty()) saw_conditional.store(true);
            std::string body = "v1-body";
            std::string etag = "etag-v1";
            if (inm == "etag-v1") {
                body = "v2-body";
                etag = "etag-v2";
            } else if (inm == "etag-v2") {
                JsonValue result(JsonValue::object_tag);
                result["notModified"] = JsonValue(true);
                result["etag"] = JsonValue("etag-v2");
                p.set_value(std::move(result));
                return;
            }
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            JsonValue c(JsonValue::object_tag);
            c["uri"] = JsonValue("uri-a");
            c["text"] = JsonValue(body);
            arr.PushBack(std::move(c));
            result["contents"] = JsonValue(std::move(arr));
            result["etag"] = JsonValue(etag);
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    auto client = McpClient::Create(std::move(pair.client), opts);

    CacheableRequestOptions ropts;
    ropts.cache_mode = "bypass";
    auto r1 = client->ReadResource("uri-a", ropts);
    ASSERT_EQ(r1.contents.size(), 1u);
    auto r2 = client->ReadResource("uri-a", ropts);
    ASSERT_EQ(r2.contents.size(), 1u);
    EXPECT_EQ(calls.load(), 2);
    EXPECT_TRUE(saw_conditional.load());
    auto* t2 = std::get_if<TextResourceContents>(&r2.contents[0]);
    ASSERT_NE(t2, nullptr);
    EXPECT_EQ(t2->text, "v2-body");
    auto r3 = client->ReadResource("uri-a", ropts);
    ASSERT_EQ(r3.contents.size(), 1u);
    EXPECT_EQ(calls.load(), 3);
    auto* t3 = std::get_if<TextResourceContents>(&r3.contents[0]);
    ASSERT_NE(t3, nullptr);
    EXPECT_EQ(t3->text, "v2-body");

    client->Close();
    server_handler->Close();
    SetEtagCacheEnabled(false);
}

TEST(McpClientTest, EtagOversizeSkipsEtag) {
    SetEtagCacheEnabled(true);
    ListToolsResult big;
    Tool t;
    t.name = "big-tool";
    t.description = std::string(9u * 1024u * 1024u, 'x');
    t.input_schema = JsonValue(JsonValue::object_tag);
    big.tools.push_back(std::move(t));
    JsonValue j = SerializeListToolsResult(big);
    EXPECT_TRUE(j.Find("etag") == nullptr);
    SetEtagCacheEnabled(false);
}

TEST(McpClientTest, EtagDisabledWritesNoEtag) {
    SetEtagCacheEnabled(false);
    ListToolsResult small;
    Tool t;
    t.name = "small-tool";
    t.input_schema = JsonValue(JsonValue::object_tag);
    small.tools.push_back(std::move(t));
    JsonValue j = SerializeListToolsResult(small);
    EXPECT_TRUE(j.Find("etag") == nullptr);
    CacheableResult cr;
    cr.etag = "abc";
    JsonValue cj = SerializeCacheableResult(cr);
    ASSERT_NE(cj.Find("etag"), nullptr);
    EXPECT_EQ(cj.Find("etag")->GetString(), "abc");
    auto back = DeserializeCacheableResult(cj);
    ASSERT_TRUE(back.etag.has_value());
    EXPECT_EQ(*back.etag, "abc");
}

namespace {

class ObservableTransport : public TransportBase {
public:
    void SendMessageAsync(JsonRpcMessage) override {}

    void Close() override {
        SetDisconnected();
        channel_->Close();
    }

    void RaiseError(std::string_view message) { NotifyError(message); }

    void RaiseClose() { SetDisconnected(); }
};

} // anonymous namespace

TEST(McpClientTest, TransportErrorAndCloseAreObservable) {
    auto transport = std::make_shared<ObservableTransport>();
    std::atomic<int> errors{0};
    std::atomic<int> closes{0};
    std::string last_error;

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.on_transport_error = [&](std::string_view message) {
        last_error = std::string(message);
        errors.fetch_add(1);
    };
    opts.on_transport_close = [&]() { closes.fetch_add(1); };

    auto client = McpClient::Create(transport, opts);
    ASSERT_NE(client, nullptr);

    transport->RaiseError("stdio write failed");
    EXPECT_EQ(errors.load(), 1);
    EXPECT_EQ(last_error, "stdio write failed");

    transport->RaiseClose();
    EXPECT_EQ(closes.load(), 1);

    client->Close();
}

TEST(McpClientTest, ClientEtagOptionGatesConditionalRequests) {
    SetEtagCacheEnabled(true);
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    std::atomic<int> calls{0};
    std::atomic<bool> saw_conditional{false};
    server_handler->SetRequestHandler(methods::kListTools,
        [&calls, &saw_conditional](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            calls.fetch_add(1);
            if (req.meta && req.meta->IsObject()) {
                if (auto* v = req.meta->Find("ifNoneMatch"); v && v->IsString())
                    saw_conditional.store(true);
            }
            JsonValue result(JsonValue::object_tag);
            JsonValue arr(JsonValue::array_tag);
            JsonValue t(JsonValue::object_tag);
            t["name"] = JsonValue("tool-x");
            t["inputSchema"] = JsonValue(JsonValue::object_tag);
            arr.PushBack(std::move(t));
            result["tools"] = JsonValue(std::move(arr));
            result["etag"] = JsonValue("v1");
            p.set_value(std::move(result));
        });
    server_handler->Start();

    ClientOptions opts;
    opts.connect_mode = ConnectMode::Pin;
    opts.pin_protocol_version = std::string(kLatestProtocolVersion);
    opts.enable_etag_cache = false;
    auto client = McpClient::Create(std::move(pair.client), opts);

    auto r1 = client->ListTools();
    ASSERT_EQ(r1.tools.size(), 1u);
    auto r2 = client->ListTools();
    ASSERT_EQ(r2.tools.size(), 1u);
    EXPECT_EQ(calls.load(), 2);
    EXPECT_FALSE(saw_conditional.load());

    client->Close();
    server_handler->Close();
    SetEtagCacheEnabled(false);
}

namespace {

class CustomMethodCodec : public WireCodec {
public:
    explicit CustomMethodCodec(std::string_view version)
        : inner_(MakeWireCodec(version)) {}

    bool HasRequestMethod(std::string_view method) const override {
        return IsCustom(method) || inner_->HasRequestMethod(method);
    }

    bool HasNotificationMethod(std::string_view method) const override {
        return IsCustom(method) || inner_->HasNotificationMethod(method);
    }

    WireValidation ValidateRequest(
        std::string_view method, const JsonValue& raw) const override {
        return IsCustom(method)
            ? WireValidation::Ok
            : inner_->ValidateRequest(method, raw);
    }

    WireValidation ValidateResponse(
        std::string_view method, const JsonValue& raw) const override {
        return inner_->ValidateResponse(method, raw);
    }

    WireValidation ValidateNotification(
        std::string_view method, const JsonValue& raw) const override {
        return IsCustom(method)
            ? WireValidation::Ok
            : inner_->ValidateNotification(method, raw);
    }

    void StampOutgoingRequest(
        JsonValue& body, const RequestMeta& meta) const override {
        inner_->StampOutgoingRequest(body, meta);
    }

    JsonValue EncodeResult(
        std::string_view method, const JsonValue& result) const override {
        return inner_->EncodeResult(method, result);
    }

    int32_t EncodeErrorCode(int32_t code) const override {
        return inner_->EncodeErrorCode(code);
    }

    std::string_view Era() const override {
        return inner_->Era();
    }

private:
    static bool IsCustom(std::string_view method) {
        return method == "test/custom" || method == "test/hang" ||
               method == "test/notify";
    }

    std::unique_ptr<WireCodec> inner_;
};

RequestHandler LegacyInitializeHandler() {
    return [](const JsonRpcRequest&, std::promise<JsonValue> p) {
        JsonValue result(JsonValue::object_tag);
        result["protocolVersion"] = JsonValue(std::string(kLegacyProtocolVersion));
        result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
        result["serverInfo"] =
            SerializeImplementation(Implementation{"control-server", "1.0"});
        p.set_value(std::move(result));
    };
}

std::shared_ptr<McpSessionHandler> MakeControlServer(
    std::shared_ptr<ITransport> transport)
{
    auto handler = std::make_shared<McpSessionHandler>(
        std::move(transport),
        std::make_unique<CustomMethodCodec>(kLegacyProtocolVersion));
    handler->SetRequestHandler(methods::kInitialize, LegacyInitializeHandler());
    return handler;
}

std::unique_ptr<McpClient> MakeLegacyControlClient(
    std::shared_ptr<ITransport> transport)
{
    ClientOptions opts;
    opts.connect_mode = ConnectMode::Legacy;
    opts.initialization_timeout = std::chrono::seconds(5);
    return McpClient::Create(std::move(transport), opts);
}

} // namespace

TEST(McpClientTest, SendRequestRoundTrip) {
    MCP_RUN_WITH_TIMEOUT([&] {
        auto pair = InMemoryTransport::CreatePair();
        auto server_handler = MakeControlServer(std::move(pair.server));
        std::atomic<bool> handled{false};
        server_handler->SetRequestHandler("test/custom",
            [&handled](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                handled.store(true);
                JsonValue result(JsonValue::object_tag);
                if (req.params && req.params->IsObject()) {
                    if (auto* value = req.params->Find("value"))
                        result["echo"] = *value;
                }
                p.set_value(std::move(result));
            });
        server_handler->Start();

        auto client = MakeLegacyControlClient(std::move(pair.client));
        JsonValue params(JsonValue::object_tag);
        params["value"] = JsonValue("round-trip");
        auto future = client->SendRequest("test/custom", std::move(params));
        ASSERT_EQ(future.wait_for(std::chrono::seconds(2)),
                  std::future_status::ready);
        auto result = future.get();
        EXPECT_TRUE(handled.load());
        ASSERT_TRUE(result.IsObject());
        ASSERT_NE(result.Find("echo"), nullptr);
        EXPECT_EQ(result["echo"].GetString(), "round-trip");

        client->Close();
        server_handler->Close();
    });
}

TEST(McpClientTest, SendNotificationReachesServer) {
    MCP_RUN_WITH_TIMEOUT([&] {
        auto pair = InMemoryTransport::CreatePair();
        auto server_handler = MakeControlServer(std::move(pair.server));
        std::mutex mutex;
        std::condition_variable cv;
        bool received = false;
        JsonValue captured;
        server_handler->SetNotificationHandler("test/notify",
            [&](const JsonRpcNotification& notif) {
                std::lock_guard<std::mutex> lock(mutex);
                received = true;
                if (notif.params) captured = *notif.params;
                cv.notify_all();
            });
        server_handler->Start();

        auto client = MakeLegacyControlClient(std::move(pair.client));
        JsonValue params(JsonValue::object_tag);
        params["payload"] = JsonValue(42);
        client->SendNotification("test/notify", std::move(params));

        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(2),
                                [&received] { return received; }));
        ASSERT_TRUE(captured.IsObject());
        ASSERT_NE(captured.Find("payload"), nullptr);
        EXPECT_EQ(captured["payload"].GetInt(), 42);
        lock.unlock();

        client->Close();
        server_handler->Close();
    });
}

TEST(McpClientTest, CancelRequestSettlesLocallyAndNotifiesPeer) {
    MCP_RUN_WITH_TIMEOUT([&] {
        auto pair = InMemoryTransport::CreatePair();
        auto server_handler = MakeControlServer(std::move(pair.server));

        std::mutex pending_mutex;
        std::condition_variable pending_cv;
        std::optional<RequestId> pending_id;
        std::shared_ptr<std::promise<JsonValue>> pending_promise;
        server_handler->SetRequestHandler("test/hang",
            [&](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                std::lock_guard<std::mutex> lock(pending_mutex);
                pending_id = req.id;
                pending_promise =
                    std::make_shared<std::promise<JsonValue>>(std::move(p));
                pending_cv.notify_all();
            });

        std::mutex cancel_mutex;
        std::condition_variable cancel_cv;
        std::vector<RequestId> cancel_ids;
        std::vector<std::string> cancel_reasons;
        server_handler->SetOnNotificationCallback(
            [&](const JsonRpcNotification& notif) {
                if (notif.method != notifications::kCancelled) return;
                if (!notif.params || !notif.params->IsObject()) return;
                auto* id = notif.params->Find("requestId");
                auto* reason = notif.params->Find("reason");
                std::lock_guard<std::mutex> lock(cancel_mutex);
                if (id && id->IsInt()) {
                    cancel_ids.push_back(RequestId{id->GetInt()});
                } else if (id && id->IsString()) {
                    cancel_ids.push_back(RequestId{id->GetString()});
                }
                cancel_reasons.push_back(
                    reason && reason->IsString() ? reason->GetString()
                                                 : std::string());
                cancel_cv.notify_all();
            });
        server_handler->Start();

        auto client = MakeLegacyControlClient(std::move(pair.client));
        auto future =
            client->SendRequest("test/hang", JsonValue(JsonValue::object_tag));

        RequestId captured;
        {
            std::unique_lock<std::mutex> lock(pending_mutex);
            ASSERT_TRUE(pending_cv.wait_for(lock, std::chrono::seconds(2),
                [&pending_id] { return pending_id.has_value(); }));
            captured = *pending_id;
        }
        ASSERT_TRUE(std::holds_alternative<int64_t>(captured));

        client->CancelRequest(captured, "user aborted");
        {
            std::unique_lock<std::mutex> lock(cancel_mutex);
            ASSERT_TRUE(cancel_cv.wait_for(lock, std::chrono::seconds(2),
                [&cancel_ids] { return !cancel_ids.empty(); }));
            ASSERT_TRUE(std::holds_alternative<int64_t>(cancel_ids[0]));
            EXPECT_EQ(std::get<int64_t>(cancel_ids[0]),
                      std::get<int64_t>(captured));
            EXPECT_EQ(cancel_reasons[0], "user aborted");
        }

        ASSERT_EQ(future.wait_for(std::chrono::seconds(2)),
                  std::future_status::ready);
        auto result = future.get();
        ASSERT_TRUE(result.IsObject());
        ASSERT_NE(result.Find("code"), nullptr);
        EXPECT_EQ(result["code"].GetInt(),
                  static_cast<int64_t>(McpErrorCode::RequestCancelled));
        EXPECT_EQ(result["message"].GetString(),
                  std::string("request cancelled"));

        client->CancelRequest(
            RequestId{std::string("string-request-id")}, "typed");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        {
            std::lock_guard<std::mutex> lock(cancel_mutex);
            EXPECT_EQ(cancel_ids.size(), 1u);
        }

        {
            std::lock_guard<std::mutex> lock(pending_mutex);
            ASSERT_NE(pending_promise, nullptr);
            pending_promise->set_value(JsonValue(JsonValue::object_tag));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        client->Close();
        server_handler->Close();
    });
}

TEST(McpClientTest, SendRequestHandleExposesIdAndTypedOutcome) {
    MCP_RUN_WITH_TIMEOUT([&] {
        auto pair = InMemoryTransport::CreatePair();
        auto server_handler = MakeControlServer(std::move(pair.server));

        std::mutex pending_mutex;
        std::condition_variable pending_cv;
        std::optional<RequestId> pending_id;
        server_handler->SetRequestHandler("test/custom",
            [&](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                {
                    std::lock_guard<std::mutex> lock(pending_mutex);
                    pending_id = req.id;
                    pending_cv.notify_all();
                }
                JsonValue result(JsonValue::object_tag);
                result["echo"] = JsonValue("handle");
                p.set_value(std::move(result));
            });
        server_handler->Start();

        auto client = MakeLegacyControlClient(std::move(pair.client));
        RequestOptions options;
        options.timeout = std::chrono::seconds(2);
        auto handle = client->SendRequest(
            "test/custom", JsonValue(JsonValue::object_tag), options);

        ASSERT_TRUE(std::holds_alternative<int64_t>(handle.id));
        {
            std::unique_lock<std::mutex> lock(pending_mutex);
            ASSERT_TRUE(pending_cv.wait_for(lock, std::chrono::seconds(2),
                [&pending_id] { return pending_id.has_value(); }));
            EXPECT_EQ(handle.id, *pending_id);
        }

        auto outcome = handle.future.get();
        ASSERT_TRUE(std::holds_alternative<JsonValue>(outcome));
        EXPECT_EQ(std::get<JsonValue>(outcome)["echo"], JsonValue("handle"));

        client->Close();
        server_handler->Close();
    });
}

TEST(McpClientTest, RawAndTypedErrorModelsDiffer) {
    MCP_RUN_WITH_TIMEOUT([&] {
        auto pair = InMemoryTransport::CreatePair();
        auto server_handler = MakeControlServer(std::move(pair.server));
        server_handler->SetRequestHandler("test/custom",
            [](const JsonRpcRequest&, std::promise<JsonValue>) {
                throw McpError(McpErrorCode::InvalidParams, "bad input");
            });
        server_handler->Start();

        auto client = MakeLegacyControlClient(std::move(pair.client));

        auto raw_future = client->SendRequestRaw(
            "test/custom", JsonValue(JsonValue::object_tag));
        ASSERT_EQ(raw_future.wait_for(std::chrono::seconds(2)),
                  std::future_status::ready);
        auto raw = raw_future.get();
        ASSERT_TRUE(raw.Contains("code"));
        EXPECT_EQ(raw["code"].GetInt(),
                  static_cast<int64_t>(McpErrorCode::InvalidParams));
        EXPECT_EQ(raw["message"].GetString(), std::string("bad input"));

        auto handle = client->SendRequest(
            "test/custom", JsonValue(JsonValue::object_tag), RequestOptions{});
        auto outcome = handle.future.get();
        ASSERT_TRUE(std::holds_alternative<ErrorData>(outcome));
        const auto& error = std::get<ErrorData>(outcome);
        EXPECT_EQ(static_cast<int32_t>(error.code),
                  static_cast<int32_t>(McpErrorCode::InvalidParams));
        EXPECT_EQ(error.message, std::string("bad input"));

        client->Close();
        server_handler->Close();
    });
}

TEST(McpClientTest, CancelTokenCancelsPendingHandle) {
    MCP_RUN_WITH_TIMEOUT([&] {
        auto pair = InMemoryTransport::CreatePair();
        auto server_handler = MakeControlServer(std::move(pair.server));

        std::mutex pending_mutex;
        std::condition_variable pending_cv;
        std::optional<RequestId> pending_id;
        std::shared_ptr<std::promise<JsonValue>> held_promise;
        server_handler->SetRequestHandler("test/hang",
            [&](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                std::lock_guard<std::mutex> lock(pending_mutex);
                pending_id = req.id;
                held_promise =
                    std::make_shared<std::promise<JsonValue>>(std::move(p));
                pending_cv.notify_all();
            });
        server_handler->Start();

        auto client = MakeLegacyControlClient(std::move(pair.client));
        CancelToken token;
        RequestOptions options;
        options.timeout = std::chrono::seconds(5);
        options.cancel_token = token;
        auto handle = client->SendRequest(
            "test/hang", JsonValue(JsonValue::object_tag), options);

        {
            std::unique_lock<std::mutex> lock(pending_mutex);
            ASSERT_TRUE(pending_cv.wait_for(lock, std::chrono::seconds(2),
                [&pending_id] { return pending_id.has_value(); }));
            EXPECT_EQ(handle.id, *pending_id);
        }

        token.Cancel("token aborted");
        auto outcome = handle.future.get();
        ASSERT_TRUE(std::holds_alternative<ErrorData>(outcome));
        const auto& error = std::get<ErrorData>(outcome);
        EXPECT_EQ(static_cast<int32_t>(error.code),
                  static_cast<int32_t>(McpErrorCode::RequestCancelled));
        ASSERT_TRUE(error.data.has_value());
        auto* reason = error.data->Find("reason");
        ASSERT_NE(reason, nullptr);
        EXPECT_EQ(reason->GetString(), std::string("token aborted"));

        client->Close();
        server_handler->Close();
    });
}

TEST(McpClientTest, RequestOptionsTimeoutCanSkipTransportCancel) {
    MCP_RUN_WITH_TIMEOUT([&] {
        auto pair = InMemoryTransport::CreatePair();
        auto server_handler = MakeControlServer(std::move(pair.server));

        std::mutex cancel_mutex;
        std::vector<RequestId> cancel_ids;
        std::shared_ptr<std::promise<JsonValue>> held_promise;
        server_handler->SetRequestHandler("test/hang",
            [&](const JsonRpcRequest&, std::promise<JsonValue> p) {
                held_promise =
                    std::make_shared<std::promise<JsonValue>>(std::move(p));
            });
        server_handler->SetOnNotificationCallback(
            [&](const JsonRpcNotification& notif) {
                if (notif.method != notifications::kCancelled) return;
                if (!notif.params || !notif.params->IsObject()) return;
                auto* id = notif.params->Find("requestId");
                std::lock_guard<std::mutex> lock(cancel_mutex);
                if (id && id->IsInt()) {
                    cancel_ids.push_back(RequestId{id->GetInt()});
                }
            });
        server_handler->Start();

        auto client = MakeLegacyControlClient(std::move(pair.client));
        RequestOptions options;
        options.timeout = std::chrono::milliseconds(200);
        options.cancel_on_timeout = false;
        auto handle = client->SendRequest(
            "test/hang", JsonValue(JsonValue::object_tag), options);

        auto outcome = handle.future.get();
        ASSERT_TRUE(std::holds_alternative<ErrorData>(outcome));
        EXPECT_EQ(static_cast<int32_t>(std::get<ErrorData>(outcome).code),
                  static_cast<int32_t>(McpErrorCode::RequestTimeout));

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        {
            std::lock_guard<std::mutex> lock(cancel_mutex);
            EXPECT_TRUE(cancel_ids.empty());
        }

        client->Close();
        server_handler->Close();
    });
}

TEST(McpClientTest, SetLoggingLevelLegacySendsRequest) {
    MCP_RUN_WITH_TIMEOUT([&] {
        auto pair = InMemoryTransport::CreatePair();
        ServerOptions sopts;
        sopts.server_info = Implementation{"logging-server", "1.0"};
        auto server = McpServer::Create(std::move(pair.server), sopts);

        std::mutex mutex;
        std::condition_variable cv;
        std::vector<LoggingMessageNotificationParams> messages;

        ClientOptions opts;
        opts.connect_mode = ConnectMode::Legacy;
        opts.initialization_timeout = std::chrono::seconds(5);
        auto client = McpClient::Create(std::move(pair.client), opts);
        client->SetLoggingHandler(
            [&](const LoggingMessageNotificationParams& message) {
                std::lock_guard<std::mutex> lock(mutex);
                messages.push_back(message);
                cv.notify_all();
            });

        EXPECT_NO_THROW(client->SetLoggingLevel(LoggingLevel::Error));
        server->SendLoggingMessage(LoggingLevel::Info, "below-threshold");
        server->SendLoggingMessage(LoggingLevel::Critical, "at-threshold");

        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(2),
                                [&messages] { return !messages.empty(); }));
        ASSERT_EQ(messages.size(), 1u);
        EXPECT_EQ(messages[0].level, LoggingLevel::Critical);
        ASSERT_TRUE(messages[0].data.IsString());
        EXPECT_EQ(messages[0].data.GetString(), "at-threshold");
        lock.unlock();

        client->Close();
        server->Close();
    });
}

TEST(McpClientTest, SetLoggingLevelModernRejected) {
    MCP_RUN_WITH_TIMEOUT([&] {
        auto pair = InMemoryTransport::CreatePair();
        ClientOptions opts;
        opts.connect_mode = ConnectMode::Pin;
        opts.pin_protocol_version = std::string(kLatestProtocolVersion);
        auto client = McpClient::Create(std::move(pair.client), opts);

        bool rejected = false;
        try {
            client->SetLoggingLevel(LoggingLevel::Info);
        } catch (const McpError& e) {
            rejected = true;
            EXPECT_EQ(e.Code(), McpErrorCode::ProtocolViolation);
        }
        EXPECT_TRUE(rejected);

        client->Close();
    });
}
