// Phase1InteropTests — first-phase feature integration tests over real transports
//
// Covered features:
//   * McpServer::SendProgress (server-initiated notifications/progress)
//   * RequestOptions::on_progress (client progress callback)
//   * McpClient::SendRootsListChanged (gated on >= 2025-06-18)
//   * Streamable HTTP client GET SSE listen stream
//
// Transport matrix:
//   * InMemory   — Auto mode (modern era, token on top-level _meta)
//   * Streamable HTTP — Legacy mode (initialize handshake sends
//     notifications/initialized, which is what opens the client GET stream;
//     legacy era, token on params._meta.progressToken)
//
// The stdio transport has no subprocess loopback infrastructure in this repo
// (no test spawns a child server), so it is intentionally not covered here.

#include <mcp/Content.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/Methods.hpp>
#include <mcp/Transport.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>

#include <mcp/test/McpTest.hpp>
#include "../unit/TestServerUtil.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <variant>

using namespace mcp;
using Ctx = RequestContext<CallToolRequestParams>;

namespace {

// Run the test body with a hard timeout guard: a hung call fails the test
// instead of blocking forever (same convention as ClientServerRoundTrip).
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

constexpr int kMaxProgressSends = 250;
constexpr auto kProgressSendInterval = std::chrono::milliseconds(20);
constexpr auto kNotifyAckTimeout = std::chrono::seconds(5);

// The progress token travels on params._meta.progressToken in the legacy era
// and on the top-level _meta in the modern era; handle both.
std::optional<ProgressToken> ExtractProgressToken(const Ctx& ctx) {
    if (ctx.Params().meta && ctx.Params().meta->progress_token)
        return *ctx.Params().meta->progress_token;
    if (ctx.GetRequest().meta) {
        if (auto* pt = ctx.GetRequest().meta->Find("progressToken"))
            return DeserializeProgressToken(*pt);
    }
    return std::nullopt;
}

// Tool that echoes the request's progress token back via SendProgress in a
// loop until the client acknowledges receipt (or the budget is exhausted), so
// the test has no timing dependency on stream setup order.
void RegisterProgressTool(McpServer& server, std::shared_ptr<std::atomic<bool>> ack) {
    server.RegisterTool("progress_echo",
        ToolOptions{}.Description("Emit progress until the client acknowledges"),
        std::function<CallToolResult(const Ctx&)>(
            [ack](const Ctx& ctx) -> CallToolResult {
                auto token = ExtractProgressToken(ctx);
                if (token) {
                    for (int i = 1; i <= kMaxProgressSends && !ack->load(); ++i) {
                        ctx.Server().SendProgress(*token, static_cast<double>(i),
                            10.0, "step " + std::to_string(i));
                        std::this_thread::sleep_for(kProgressSendInterval);
                    }
                }
                CallToolResult r;
                r.content.push_back(
                    TextContent{"text", token ? "ok" : "no-token"});
                return r;
            }));
}

} // namespace

// ============================================================
// InMemory (Auto mode, modern era)
// ============================================================
struct Phase1InMemoryFixture : mcp::test::TestCase {
    std::unique_ptr<McpServer> server;
    std::unique_ptr<McpClient> client;
    std::thread server_thread;
    std::shared_ptr<std::atomic<bool>> progress_ack;

    void SetUp() override {
        progress_ack = std::make_shared<std::atomic<bool>>(false);
        auto pair = InMemoryTransport::CreatePair();

        ServerOptions sopts;
        sopts.server_info = Implementation{"TestServer", "1.0.0"};
        server = McpServer::Create(pair.server, sopts);
        RegisterProgressTool(*server, progress_ack);

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

TEST_F(Phase1InMemoryFixture, ProgressRoundTrip) {
    RunWithTimeout([this]() {
        auto got = std::make_shared<std::promise<ProgressNotificationParams>>();
        auto fut = got->get_future();

        RequestOptions opts;
        opts.on_progress = [got, this](const ProgressNotificationParams& p) {
            if (!progress_ack->exchange(true)) got->set_value(p);
        };

        auto result = client->CallTool("progress_echo", std::nullopt, opts);
        EXPECT_FALSE(result.is_error);

        ASSERT_EQ(fut.wait_for(kNotifyAckTimeout), std::future_status::ready);
        auto p = fut.get();
        // InMemory preserves ordering: the first notification is step 1.
        EXPECT_EQ(p.progress, 1.0);
        ASSERT_TRUE(p.total.has_value());
        EXPECT_EQ(*p.total, 10.0);
        ASSERT_TRUE(p.message.has_value());
        EXPECT_EQ(*p.message, "step 1");
        // Client-generated tokens are monotonic int64 values.
        EXPECT_TRUE(std::holds_alternative<int64_t>(p.progress_token));
        EXPECT_GE(std::get<int64_t>(p.progress_token), int64_t(1));
    });
}

TEST_F(Phase1InMemoryFixture, RootsListChangedRoundTrip) {
    RunWithTimeout([this]() {
        ASSERT_GE(std::string(client->GetNegotiatedProtocolVersion()),
                  std::string("2025-06-18"));

        auto got = std::make_shared<std::promise<std::string>>();
        auto fut = got->get_future();
        server->GetSessionHandler().SetNotificationHandler(
            notifications::kRootsListChanged,
            [got](const JsonRpcNotification& n) { got->set_value(n.method); });

        client->SendRootsListChanged();

        ASSERT_EQ(fut.wait_for(kNotifyAckTimeout), std::future_status::ready);
        EXPECT_EQ(fut.get(), std::string(notifications::kRootsListChanged));
    });
}

// ============================================================
// Streamable HTTP (Legacy mode: initialize opens the GET stream)
// ============================================================
struct Phase1HttpFixture : mcp::test::TestCase {
    uint16_t port = 0;
    std::shared_ptr<StreamableHttpServerTransport> server_transport;
    std::unique_ptr<McpServer> server;
    std::shared_ptr<ITransport> client_transport;
    std::unique_ptr<McpClient> client;
    std::thread server_thread;
    std::shared_ptr<std::atomic<bool>> progress_ack;

    void SetUp() override {
        progress_ack = std::make_shared<std::atomic<bool>>(false);
        port = PickFreePort(static_cast<uint16_t>(kTestBasePort + 1500));

        // stateless defaults to true, enable_legacy_sse to true: server-side
        // notifications are broadcast to every open SSE stream.
        StreamableHttpServerOptions topts;
        topts.port = port;
        topts.endpoint = "/mcp";
        server_transport = std::make_shared<StreamableHttpServerTransport>(topts);

        ServerOptions sopts;
        sopts.server_info = Implementation{"TestServer", "1.0.0"};
        server = McpServer::Create(server_transport, sopts);
        RegisterProgressTool(*server, progress_ack);

        server_thread = std::thread([this]() { server->Run(); });
        ASSERT_TRUE(WaitUntilReady(port));

        HttpClientTransportOptions copts;
        copts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
        StreamableHttpClientTransport http_client(copts);
        client_transport = http_client.Connect();

        ClientOptions cops;
        cops.client_info = Implementation{"TestClient", "1.0.0"};
        // Auto mode settles via server/discover, which never sends
        // notifications/initialized — the GET listen stream would stay closed.
        // Legacy mode runs the initialize handshake, which does.
        cops.connect_mode = ConnectMode::Legacy;
        client = McpClient::Create(client_transport, cops);
    }

    void TearDown() override {
        if (client) client->Close();
        if (client_transport) client_transport->Close();
        if (server) server->Close();
        if (server_transport) server_transport->Close();
        if (server_thread.joinable()) server_thread.join();
    }
};

TEST_F(Phase1HttpFixture, ProgressRoundTrip) {
    RunWithTimeout([this]() {
        auto got = std::make_shared<std::promise<ProgressNotificationParams>>();
        auto fut = got->get_future();

        RequestOptions opts;
        opts.on_progress = [got, this](const ProgressNotificationParams& p) {
            if (!progress_ack->exchange(true)) got->set_value(p);
        };

        auto result = client->CallTool("progress_echo", std::nullopt, opts);
        EXPECT_FALSE(result.is_error);

        ASSERT_EQ(fut.wait_for(kNotifyAckTimeout), std::future_status::ready);
        auto p = fut.get();
        // The GET stream may open mid-loop, so the first delivery can be any
        // step; only the token routing and field round-trip are asserted.
        EXPECT_GE(p.progress, 1.0);
        EXPECT_LE(p.progress, 10.0);
        ASSERT_TRUE(p.total.has_value());
        EXPECT_EQ(*p.total, 10.0);
        ASSERT_TRUE(p.message.has_value());
        EXPECT_TRUE(p.message->rfind("step ", 0) == 0);
        EXPECT_TRUE(std::holds_alternative<int64_t>(p.progress_token));
        EXPECT_GE(std::get<int64_t>(p.progress_token), int64_t(1));
    });
}

TEST_F(Phase1HttpFixture, GetSseDeliversToolListChanged) {
    RunWithTimeout([this]() {
        auto got = std::make_shared<std::promise<std::string>>();
        auto fut = got->get_future();
        client->SetNotificationHandler(notifications::kToolListChanged,
            [got](const JsonRpcNotification& n) { got->set_value(n.method); });

        // The listen stream connects asynchronously after the initialize
        // handshake; broadcast repeatedly until the client confirms receipt
        // (earlier broadcasts before the stream is up are simply dropped).
        auto deadline = std::chrono::steady_clock::now() + kNotifyAckTimeout;
        bool delivered = false;
        while (!delivered && std::chrono::steady_clock::now() < deadline) {
            server->SendToolListChanged();
            if (fut.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready)
                delivered = true;
        }
        ASSERT_TRUE(delivered);
        EXPECT_EQ(fut.get(), std::string(notifications::kToolListChanged));
    });
}
