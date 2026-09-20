// McpClientTests — unit tests for McpClient creation, options, tool conversion,
// and real protocol negotiation against an in-memory server

#include <mcp/client/McpClient.hpp>
#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/McpTypes.hpp>

#include <mcp/test/McpTest.hpp>

#include <atomic>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using namespace mcp;

using Ctx = RequestContext<CallToolRequestParams>;

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

// Auto mode against a server whose -32022 lists an overlapping supported
// version: the probe is retried once with the shared version and succeeds.
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

// Auto mode against a server whose -32022 lists only legacy versions: no
// overlap exists, so negotiation falls back to initialize.
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

// Auto mode against a TS-style server whose discover result carries no
// top-level serverInfo: the _meta["io.modelcontextprotocol/serverInfo"]
// envelope must be picked up and the probe must still succeed.
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

// Auto mode against a server exposing serverInfo neither at the top level
// nor in _meta: the probe still succeeds with an empty implementation.
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

// Auto mode with a successful discover that lists only a non-latest
// client-supported version: modern era is kept but the negotiated version
// is the shared one, not unconditionally the latest.
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

// Auto mode with a successful discover declaring an empty supportedVersions
// list: the lists share no version, so the probe fails and negotiation
// falls back to the initialize handshake.
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

// ── Pagination: without a cursor all pages are merged automatically ──
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

    // Without a cursor both pages are merged.
    auto all = client->ListTools();
    ASSERT_EQ(all.tools.size(), 3u);
    EXPECT_EQ(all.tools[0].name, "tool-1");
    EXPECT_EQ(all.tools[2].name, "tool-3");

    // With an explicit cursor a single page is returned.
    auto page = client->ListTools("c1");
    ASSERT_EQ(page.tools.size(), 1u);
    EXPECT_EQ(page.tools[0].name, "tool-3");

    client->Close();
    server_handler->Close();
}

// ── MRTR total budget caps the per-round timeout ──
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
    // The round timeout must have been capped to ~1s by the total budget.
    EXPECT_LT(elapsed.count(), 10);

    client->Close();
    server_handler->Close();
}

// ── MRTR: input_required 内嵌 sampling/createMessage 由 SamplingHandler 填充 ──
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

// ── MRTR: input_required 内嵌 roots/list 由 RootsHandler 填充 ──
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

// ── MRTR: inputRequests 按 method 分派，inputResponses 键回显服务端原键 ──
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

// ── MRTR: 未知 input request method 抛类型化错误（不静默忽略）──
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
        EXPECT_EQ(e.Code(), McpErrorCode::MethodNotFound);
        EXPECT_NE(std::string(e.what()).find("unsupported input request method"),
            std::string::npos);
    }
    EXPECT_EQ(calls.load(), 1);

    client->Close();
    server_handler->Close();
}

// ── MRTR: state-only 轮次（仅 requestState）重试前退避 >= 50ms ──
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

// ── MRTR: state-only 退避指数增长，带 input_requests 的轮次后重置 ──
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

// ── MRTR: 默认 max_rounds=10，第 11 轮后停止 ──
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

// ── SEP-2549: ttlMs cache hint caches list results; listChanged invalidates ──
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

    // Second call is served from the cache.
    auto r2 = client->ListTools();
    ASSERT_EQ(r2.tools.size(), 1u);
    EXPECT_EQ(calls.load(), 1);

    // A listChanged notification (handled by the built-in handler) invalidates
    // the cache; the next call reaches the server again.
    server_handler->SendNotification(notifications::kToolListChanged,
        JsonValue(JsonValue::object_tag));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto r3 = client->ListTools();
    ASSERT_EQ(r3.tools.size(), 1u);
    EXPECT_EQ(calls.load(), 2);

    client->Close();
    server_handler->Close();
}

// ── SEP-2106: server rejects tool output that violates outputSchema ──
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
                // structured_content missing the required "name" property
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

// ── SEP-2549: list pages with different cursors are cached separately ──
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

// ── SEP-2549: resources/read cache keys include the uri ──
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

// ── SEP-2549: ttl hints above 24h are clamped, not rejected ──
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

// ── SEP-2549: private entries hit within the same connection and stay
// separate from public entries ──
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

// ── SEP-2549: private cache entries are dropped when the connection closes ──
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

// ── SEP-2549: resources/updated invalidates only the affected uri ──
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

// ── SubscribeAsync: waits for the acknowledged first frame ──
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

// ── SubscribeAsync: a server that answers the request but never sends the
// acknowledged frame makes SubscribeAsync time out with an error ──
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
    server_handler->SetRequestHandler(methods::kSubscribe,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            p.set_value(JsonValue(JsonValue::object_tag));
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

// ── SendRootsListChanged: allowed at the 2025-06-18 boundary and later ──
TEST(McpClientTest, SendRootsListChangedAtOrAfterMinVersion) {
    // Boundary: pinned exactly at 2025-06-18, the notification goes through.
    auto pair = InMemoryTransport::CreatePair();
    auto server_handler = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
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

    // Later legacy era (2025-11-25) is after 2025-06-18, so it is allowed too.
    auto pair2 = InMemoryTransport::CreatePair();
    auto server_handler2 = std::make_shared<McpSessionHandler>(
        std::move(pair2.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
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

// ── SendRootsListChanged: rejected with McpError below 2025-06-18 ──
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

// ── progress: CallTool on_progress 收到服务端按请求 token 回显的通知 ──
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

// ── progress: 未注册 on_progress 时收到的通知被静默忽略 ──
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

// ── progress: 请求结束后同 token 的通知不再触发回调 ──
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

// ── ListToolsAll aggregates a three-page cursor chain in order ──
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

// ── ListToolsAll throws at the 64-page cap when the cursor never ends ──
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

// ── ListToolsAll on a single page equals the plain listing ──
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

// ── ListResourcesAll / ListResourceTemplatesAll / ListPromptsAll aggregate ──
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

// ── CallToolAsTask: task 句柄立即返回，GetTask/PollTaskToCompletion 跟进 ──
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

// ── CallToolAsTask: 对端未任务化而同步返回 CallToolResult 时自动降级 ──
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

// ── U9 接线: ClientOptions::max_total_timeout 传入 handler 后封顶 progress 续命 ──
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

// ── U7: URL elicitation——url handler 被调用后自动发送 elicitation/complete ──
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
            ASSERT_TRUE(params.url.has_value());
            EXPECT_EQ(*params.url, "https://example.com/authorize");
            ASSERT_TRUE(params.elicitation_id.has_value());
            EXPECT_EQ(*params.elicitation_id, "elicit-1");
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

// ── U7: 未设置 url handler 时按 decline 应答且仍发送 complete ──
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

// ── U8: SessionExpired 触发恰一次重新 initialize 并重放原请求 ──
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

// ── U8: 重放再次 SessionExpired 时原样透传，不再第二次恢复 ──
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

// ── U8: reinit_on_expired_session=false 时 SessionExpired 直接透传 ──
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

// ── U8: 非 SessionExpired 错误不触发重新初始化 ──
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
