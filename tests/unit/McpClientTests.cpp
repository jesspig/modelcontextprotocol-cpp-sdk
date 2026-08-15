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
                ir.input_requests.sampling = InputRequestSampling{std::move(params)};
                ir.request_state = "st-1";
                p.set_value(SerializeInputRequiredResult(ir));
                return;
            }
            round.store(2);
            auto* ir_resp = req.params ? req.params->Find("inputResponses") : nullptr;
            ASSERT_NE(ir_resp, nullptr);
            auto* sampling = ir_resp->Find("sampling");
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
                ir.input_requests.roots = InputRequestRoots{};
                ir.request_state = "st-roots";
                p.set_value(SerializeInputRequiredResult(ir));
                return;
            }
            round.store(2);
            auto* ir_resp = req.params ? req.params->Find("inputResponses") : nullptr;
            ASSERT_NE(ir_resp, nullptr);
            auto* roots = ir_resp->Find("roots");
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
                    ir.input_requests.elicit = InputRequestElicit{"more"};
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
        r.values = JsonValue("ok");
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
    EXPECT_LT(gaps[3], 150);

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
            ir.input_requests.elicit = InputRequestElicit{"more input"};
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
        r.values = JsonValue("ok");
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
