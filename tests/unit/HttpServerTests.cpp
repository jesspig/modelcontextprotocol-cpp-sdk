// HttpServerTests — unit tests for HttpServer, EventStore, and StreamableHttp transports

#include <mcp/Methods.hpp>
#include <mcp/http/HttpServer.hpp>
#include <mcp/http/EventStore.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/protocol/WireCodec.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>

#include <transport/detail/net/HttpClient.hpp>

#include <mcp/test/McpTest.hpp>
#include "TestServerUtil.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Avoid `using namespace mcp;` — HttpRequest/HttpResponse clash between hv and mcp
using MCP_Request = mcp::HttpRequest;
using MCP_Response = mcp::HttpResponse;

namespace {

using NetResp = mcp::detail::net::HttpResponseInfo;

std::vector<std::promise<mcp::JsonValue>> g_held_promises;

std::optional<NetResp> HttpGet(
    const std::string& url,
    const std::unordered_map<std::string, std::string>& hdrs = {})
{
    try {
        mcp::detail::net::HttpClient client;
        mcp::detail::net::HttpRequestSpec req;
        req.method = "GET";
        req.url = url;
        req.headers = hdrs;
        return client.Request(req);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<NetResp> HttpPost(
    const std::string& url, const std::string& body,
    const std::unordered_map<std::string, std::string>& hdrs = {})
{
    try {
        mcp::detail::net::HttpClient client;
        mcp::detail::net::HttpRequestSpec req;
        req.method = "POST";
        req.url = url;
        req.body = body;
        req.headers = hdrs;
        return client.Request(req);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<NetResp> HttpDelete(const std::string& url) {
    try {
        mcp::detail::net::HttpClient client;
        mcp::detail::net::HttpRequestSpec req;
        req.method = "DELETE";
        req.url = url;
        return client.Request(req);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

// ============================================================
// HttpServer
// ============================================================
TEST(HttpServerTest, GetPing) {
    auto port = PickFreePort(kTestBasePort);
    mcp::HttpServer server(port);
    server.SetHandler("GET", "/ping", [](const MCP_Request&, MCP_Response& resp) {
        resp.body = "pong";
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto r = HttpGet("http://127.0.0.1:" + std::to_string(port) + "/ping");
    ASSERT_NE(r, std::nullopt);
    EXPECT_EQ(r->status_code, 200);
    EXPECT_EQ(r->body, "pong");
    server.Stop();
}

TEST(HttpServerTest, PostEcho) {
    auto port = PickFreePort(kTestBasePort);
    mcp::HttpServer server(port);
    server.SetHandler("POST", "/echo", [](const MCP_Request& req, MCP_Response& resp) {
        resp.body = req.body;
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto r = HttpPost("http://127.0.0.1:" + std::to_string(port) + "/echo", "hello");
    ASSERT_NE(r, std::nullopt);
    EXPECT_EQ(r->status_code, 200);
    EXPECT_EQ(r->body, "hello");
    server.Stop();
}

TEST(HttpServerTest, NotFound) {
    auto port = PickFreePort(kTestBasePort);
    mcp::HttpServer server(port);
    server.SetHandler("GET", "/ping", [](const MCP_Request&, MCP_Response& resp) {
        resp.body = "pong";
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto r = HttpGet("http://127.0.0.1:" + std::to_string(port) + "/nonexistent");
    ASSERT_NE(r, std::nullopt);
    EXPECT_EQ(r->status_code, 404);
    server.Stop();
}

TEST(HttpServerTest, MultipleHandlers) {
    auto port = PickFreePort(kTestBasePort);
    mcp::HttpServer server(port);
    server.SetHandler("GET", "/a", [](const MCP_Request&, MCP_Response& resp) { resp.body = "A"; });
    server.SetHandler("GET", "/b", [](const MCP_Request&, MCP_Response& resp) { resp.body = "B"; });
    server.SetHandler("POST", "/a", [](const MCP_Request&, MCP_Response& resp) { resp.body = "A-post"; });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto base = "http://127.0.0.1:" + std::to_string(port);
    auto r1 = HttpGet(base + "/a");
    auto r2 = HttpGet(base + "/b");
    auto r3 = HttpPost(base + "/a", "");
    ASSERT_NE(r1, std::nullopt);
    ASSERT_NE(r2, std::nullopt);
    ASSERT_NE(r3, std::nullopt);
    EXPECT_EQ(r1->body, "A");
    EXPECT_EQ(r2->body, "B");
    EXPECT_EQ(r3->body, "A-post");
    server.Stop();
}

// ============================================================
// EventStore
// ============================================================
TEST(EventStoreTest, AppendAndRetrieve) {
    mcp::EventStore store;
    auto id1 = store.Append("sess1", "event1");
    store.Append("sess1", "event2");
    store.Append("sess2", "event3");

    auto events = store.GetEventsSince("sess1", id1);
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].second, "event2");

    EXPECT_FALSE(store.GetEventsSince("sess1", 0).empty());
    store.Clear("sess1");
    EXPECT_TRUE(store.GetEventsSince("sess1", 0).empty());
}

TEST(EventStoreTest, MaxCapacity) {
    mcp::EventStore store;
    for (size_t i = 0; i < mcp::EventStore::kMaxEventsPerSession + 10; ++i)
        store.Append("sess1", "data");

    auto events = store.GetEventsSince("sess1", 0);
    EXPECT_LE(events.size(), mcp::EventStore::kMaxEventsPerSession);
}

// ============================================================
// StreamableHttpServerTransport + StreamableHttpClientTransport
// ============================================================
TEST(StreamableHttpTest, McpHeadersValidation) {
    std::string error;
    auto body = mcp::JsonValue::Parse(R"({"jsonrpc":"2.0","method":"tools/list","id":1})");

    EXPECT_TRUE(mcp::StreamableHttpServerTransport::ValidateMcpHeaders("tools/list", "", body, error));
    EXPECT_TRUE(error.empty());

    EXPECT_FALSE(mcp::StreamableHttpServerTransport::ValidateMcpHeaders("tools/call", "", body, error));
    EXPECT_FALSE(error.empty());
}

// SEP-2243: a result meta "x-mcp-header" annotation is mirrored into
// Mcp-Param-* response headers in stateless mode.
TEST(StreamableHttpTest, StatelessResponseMirrorsMcpParamHeaders) {
    auto port = PickFreePort(kTestBasePort + 500);

    mcp::StreamableHttpServerOptions opts;
    opts.port = port;
    opts.endpoint = "/mcp";
    opts.stateless = true;
    opts.enable_legacy_sse = false;
    auto transport = std::make_shared<mcp::StreamableHttpServerTransport>(opts);

    auto handler = std::make_shared<mcp::McpSessionHandler>(
        transport, mcp::MakeWireCodec(std::string(mcp::kLatestProtocolVersion)));
    handler->SetRequestHandler(mcp::methods::kCallTool,
        [](const mcp::JsonRpcRequest&, std::promise<mcp::JsonValue> p) {
            mcp::JsonValue result(mcp::JsonValue::object_tag);
            mcp::JsonValue meta(mcp::JsonValue::object_tag);
            mcp::JsonValue xhc(mcp::JsonValue::object_tag);
            xhc["foo"] = mcp::JsonValue("bar");
            meta["x-mcp-header"] = std::move(xhc);
            result["_meta"] = std::move(meta);
            p.set_value(std::move(result));
        });
    handler->Start();
    transport->Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::unordered_map<std::string, std::string> hdrs;
    hdrs["Content-Type"] = "application/json";
    hdrs["Mcp-Method"] = "tools/call";
    auto r = HttpPost(
        "http://127.0.0.1:" + std::to_string(port) + "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}})",
        hdrs);
    ASSERT_NE(r, std::nullopt);
    EXPECT_EQ(r->status_code, 200);
    EXPECT_EQ(r->headers["content-type"], "text/event-stream");
    EXPECT_EQ(r->headers["mcp-param-foo"], "bar");

    handler->Close();
    transport->Close();
}

// ── DNS rebinding protection: foreign Host headers are rejected ──
TEST(HttpServerTest, HostValidationRejectsForeignHost) {
    auto port = PickFreePort(kTestBasePort + 900);
    mcp::HttpServer server(port);
    server.SetHandler("GET", "/ping", [](const MCP_Request&, MCP_Response& resp) {
        resp.body = "pong";
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto base = "http://127.0.0.1:" + std::to_string(port);
    auto ok = HttpGet(base + "/ping");
    ASSERT_NE(ok, std::nullopt);
    EXPECT_EQ(ok->status_code, 200);

    std::unordered_map<std::string, std::string> hdrs;
    hdrs["Host"] = "evil.example.com";
    auto rejected = HttpGet(base + "/ping", hdrs);
    ASSERT_NE(rejected, std::nullopt);
    EXPECT_EQ(rejected->status_code, 403);
    server.Stop();
}

TEST(HttpServerTest, HostValidationAllowsConfiguredHosts) {
    auto port = PickFreePort(kTestBasePort + 950);
    mcp::HttpServerOptions opts;
    opts.allowed_hosts = {"my-host:1234"};
    mcp::HttpServer server(port, opts);
    server.SetHandler("GET", "/ping", [](const MCP_Request&, MCP_Response& resp) {
        resp.body = "pong";
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto base = "http://127.0.0.1:" + std::to_string(port);
    std::unordered_map<std::string, std::string> hdrs;
    hdrs["Host"] = "my-host:1234";
    auto ok = HttpGet(base + "/ping", hdrs);
    ASSERT_NE(ok, std::nullopt);
    EXPECT_EQ(ok->status_code, 200);

    auto rejected = HttpGet(base + "/ping");
    ASSERT_NE(rejected, std::nullopt);
    EXPECT_EQ(rejected->status_code, 403);
    server.Stop();
}

// ── SSE keepalive: comment frames are broadcast at the configured interval ──
TEST(StreamableHttpTest, SseKeepAliveFrames) {
    auto port = PickFreePort(kTestBasePort + 700);
    mcp::StreamableHttpServerOptions opts;
    opts.port = port;
    opts.endpoint = "/mcp";
    opts.enable_legacy_sse = true;
    opts.sse_keep_alive_ms = 100;
    auto transport = std::make_shared<mcp::StreamableHttpServerTransport>(opts);
    auto handler = std::make_shared<mcp::McpSessionHandler>(
        transport, mcp::MakeWireCodec(std::string(mcp::kLatestProtocolVersion)));
    handler->Start();
    transport->Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::mutex m;
    std::condition_variable cv;
    std::string received;
    std::atomic<bool> got_ping{false};

    mcp::detail::net::HttpClient client;
    std::thread reader([&] {
        mcp::detail::net::HttpRequestSpec req;
        req.method = "GET";
        req.url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
        req.headers["Accept"] = "text/event-stream";
        try {
            client.Request(req, [&](std::string_view chunk) {
                std::lock_guard<std::mutex> lock(m);
                received.append(chunk.data(), chunk.size());
                if (received.find(": ping") != std::string::npos)
                    got_ping.store(true);
                cv.notify_all();
            });
        } catch (...) {
        }
    });

    {
        std::unique_lock<std::mutex> lock(m);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                                [&] { return got_ping.load(); }));
    }

    client.Close();
    reader.join();
    handler->Close();
    transport->Close();
}

// ── DELETE terminates a sessionful transport (405 in stateless mode) ──
TEST(StreamableHttpTest, DeleteTerminatesSession) {
    auto port = PickFreePort(kTestBasePort + 1000);
    mcp::StreamableHttpServerOptions opts;
    opts.port = port;
    opts.endpoint = "/mcp";
    opts.stateless = false;
    opts.enable_legacy_sse = false;
    auto transport = std::make_shared<mcp::StreamableHttpServerTransport>(opts);
    auto handler = std::make_shared<mcp::McpSessionHandler>(
        transport, mcp::MakeWireCodec(std::string(mcp::kLatestProtocolVersion)));
    handler->Start();
    transport->Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto r = HttpDelete("http://127.0.0.1:" + std::to_string(port) + "/mcp");
    ASSERT_NE(r, std::nullopt);
    EXPECT_EQ(r->status_code, 200);

    handler->Close();
    transport->Close();
}

// ── Stateful mode: a request response is delivered synchronously on the
// POST SSE stream (200 + text/event-stream), not via 202 + GET stream. ──
TEST(StreamableHttpTest, StatefulRequestResponseViaSseStream) {
    auto port = PickFreePort(kTestBasePort + 600);
    mcp::StreamableHttpServerOptions opts;
    opts.port = port;
    opts.endpoint = "/mcp";
    opts.stateless = false;
    opts.enable_legacy_sse = false;
    auto transport = std::make_shared<mcp::StreamableHttpServerTransport>(opts);
    auto handler = std::make_shared<mcp::McpSessionHandler>(
        transport, mcp::MakeWireCodec(std::string(mcp::kLatestProtocolVersion)));
    handler->SetRequestHandler(mcp::methods::kCallTool,
        [](const mcp::JsonRpcRequest&, std::promise<mcp::JsonValue> p) {
            mcp::JsonValue result(mcp::JsonValue::object_tag);
            result["text"] = mcp::JsonValue("echo-ok");
            p.set_value(std::move(result));
        });
    handler->Start();
    transport->Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::unordered_map<std::string, std::string> hdrs;
    hdrs["Content-Type"] = "application/json";
    hdrs["Mcp-Method"] = "tools/call";
    auto r = HttpPost(
        "http://127.0.0.1:" + std::to_string(port) + "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}})",
        hdrs);
    ASSERT_NE(r, std::nullopt);
    EXPECT_EQ(r->status_code, 200);
    EXPECT_EQ(r->headers["content-type"], "text/event-stream");
    EXPECT_NE(r->body.find("event: message"), std::string::npos);
    EXPECT_NE(r->body.find("\"id\":1"), std::string::npos);
    EXPECT_NE(r->body.find("echo-ok"), std::string::npos);

    handler->Close();
    transport->Close();
}

// ── Stateful mode: a notification is acknowledged with 202 + {} ──
TEST(StreamableHttpTest, StatefulNotificationReturns202) {
    auto port = PickFreePort(kTestBasePort + 650);
    mcp::StreamableHttpServerOptions opts;
    opts.port = port;
    opts.endpoint = "/mcp";
    opts.stateless = false;
    opts.enable_legacy_sse = false;
    auto transport = std::make_shared<mcp::StreamableHttpServerTransport>(opts);
    auto handler = std::make_shared<mcp::McpSessionHandler>(
        transport, mcp::MakeWireCodec(std::string(mcp::kLatestProtocolVersion)));
    handler->Start();
    transport->Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto r = HttpPost(
        "http://127.0.0.1:" + std::to_string(port) + "/mcp",
        R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":1,"reason":"bye"}})",
        {});
    ASSERT_NE(r, std::nullopt);
    EXPECT_EQ(r->status_code, 202);
    EXPECT_EQ(r->headers["content-type"], "application/json");
    EXPECT_EQ(r->body, "{}");

    handler->Close();
    transport->Close();
}

// ── Stateful mode: a request whose response never arrives times out with
// 504 (kStatelessTimeout). The client timeout must exceed the server's. ──
TEST(StreamableHttpTest, StatefulRequestTimeoutReturns504) {
    auto port = PickFreePort(kTestBasePort + 720);
    mcp::StreamableHttpServerOptions opts;
    opts.port = port;
    opts.endpoint = "/mcp";
    opts.stateless = false;
    opts.enable_legacy_sse = false;
    auto transport = std::make_shared<mcp::StreamableHttpServerTransport>(opts);
    auto handler = std::make_shared<mcp::McpSessionHandler>(
        transport, mcp::MakeWireCodec(std::string(mcp::kLatestProtocolVersion)));
    handler->SetRequestHandler(mcp::methods::kCallTool,
        [](const mcp::JsonRpcRequest&, std::promise<mcp::JsonValue> p) {
            g_held_promises.push_back(std::move(p));
        });
    handler->Start();
    transport->Start();
    ASSERT_TRUE(WaitUntilReady(port));

    mcp::detail::net::HttpClient client;
    mcp::detail::net::HttpRequestSpec req;
    req.method = "POST";
    req.url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    req.body = R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}})";
    req.headers["Content-Type"] = "application/json";
    req.headers["Mcp-Method"] = "tools/call";
    req.timeout = std::chrono::milliseconds(60000);
    auto resp = client.Request(req);
    EXPECT_EQ(resp.status_code, 504);
    EXPECT_EQ(resp.headers["content-type"], "application/json");
    EXPECT_NE(resp.body.find("-32000"), std::string::npos);

    handler->Close();
    transport->Close();
}

// ── Protocol errors map to conventional HTTP status codes: -32601 → 404 ──
TEST(StreamableHttpTest, UnknownMethodMapsTo404) {
    auto port = PickFreePort(kTestBasePort + 750);
    mcp::StreamableHttpServerOptions opts;
    opts.port = port;
    opts.endpoint = "/mcp";
    opts.stateless = false;
    opts.enable_legacy_sse = false;
    auto transport = std::make_shared<mcp::StreamableHttpServerTransport>(opts);
    auto handler = std::make_shared<mcp::McpSessionHandler>(
        transport, mcp::MakeWireCodec(std::string(mcp::kLatestProtocolVersion)));
    handler->Start();
    transport->Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto r = HttpPost(
        "http://127.0.0.1:" + std::to_string(port) + "/mcp",
        R"({"jsonrpc":"2.0","id":7,"method":"unknown/method","params":{},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}})",
        {});
    ASSERT_NE(r, std::nullopt);
    EXPECT_EQ(r->status_code, 404);
    EXPECT_EQ(r->headers["content-type"], "application/json");
    EXPECT_NE(r->body.find("-32601"), std::string::npos);

    handler->Close();
    transport->Close();
}

// ── Client: a 200 + text/event-stream POST response delivers its events ──
TEST(StreamableHttpTest, ClientReceivesSseStreamResponse) {
    auto port = PickFreePort(kTestBasePort + 800);
    mcp::HttpServer mock(port);
    mock.SetHandler("POST", "/mcp", [](const MCP_Request&, MCP_Response& resp) {
        resp.status_code = 200;
        resp.headers["content-type"] = "text/event-stream";
        resp.body = "event: message\ndata: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"resultType\":\"complete\",\"text\":\"pong\"}}\n\n";
        resp.sse_close_after_write = true;
    });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    mcp::HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    mcp::StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);
    auto& channel = transport->GetMessageChannel();

    mcp::JsonRpcMessage req(mcp::JsonRpcRequest{});
    auto& rr = std::get<mcp::JsonRpcRequest>(req);
    rr.id = int64_t(1);
    rr.method = "tools/call";
    transport->SendMessageAsync(std::move(req));

    mcp::JsonRpcMessage received;
    std::atomic<bool> got{false};
    channel.AsyncReceive([&](std::error_code ec, mcp::JsonRpcMessage msg) {
        if (!ec) {
            received = std::move(msg);
            got.store(true);
        }
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!got.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(got.load());
    ASSERT_TRUE(mcp::IsResponse(received));
    const auto& resp = std::get<mcp::JsonRpcResponse>(received);
    EXPECT_EQ(resp.id, mcp::RequestId(int64_t(1)));

    transport->Close();
    mock.Stop();
}

// ── Client: 202 acknowledges a notification; the connection stays up ──
TEST(StreamableHttpTest, ClientIgnores202ForNotification) {
    auto port = PickFreePort(kTestBasePort + 850);
    mcp::HttpServer mock(port);
    mock.SetHandler("POST", "/mcp", [](const MCP_Request&, MCP_Response& resp) {
        resp.status_code = 202;
        resp.status_text = "Accepted";
        resp.headers["content-type"] = "application/json";
        resp.body = "{}";
    });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    mcp::HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    mcp::StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);

    mcp::JsonRpcNotification notif;
    notif.method = "notifications/cancelled";
    transport->SendMessageAsync(std::move(notif));

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_TRUE(transport->GetMessageChannel().IsOpen());

    transport->Close();
    mock.Stop();
}

// ── Client: a known session id is carried on every POST from the start ──
TEST(StreamableHttpTest, ClientSendsKnownSessionIdOnFirstRequest) {
    auto port = PickFreePort(kTestBasePort + 1100);
    mcp::HttpServer mock(port);
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> session_headers;
    mock.SetHandler("POST", "/mcp", [&](const MCP_Request& req, MCP_Response& resp) {
        {
            std::lock_guard<std::mutex> lock(m);
            auto it = req.headers.find("mcp-session-id");
            session_headers.push_back(it == req.headers.end() ? "" : it->second);
            cv.notify_all();
        }
        resp.status_code = 200;
        resp.headers["content-type"] = "application/json";
        resp.body = R"({"jsonrpc":"2.0","id":1,"result":{"resultType":"complete"}})";
    });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    mcp::HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    opts.known_session_id = "sess-known";
    mcp::StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);

    mcp::JsonRpcMessage req(mcp::JsonRpcRequest{});
    auto& rr = std::get<mcp::JsonRpcRequest>(req);
    rr.id = int64_t(1);
    rr.method = "tools/call";
    transport->SendMessageAsync(std::move(req));

    {
        std::unique_lock<std::mutex> lock(m);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                                [&] { return !session_headers.empty(); }));
    }
    EXPECT_EQ(session_headers[0], "sess-known");

    transport->Close();
    mock.Stop();
}

// ── Client: a Mcp-Session-Id response header is captured and carried on
// subsequent POSTs (stateful server interop) ──
TEST(StreamableHttpTest, ClientCarriesCapturedSessionId) {
    auto port = PickFreePort(kTestBasePort + 1150);
    mcp::HttpServer mock(port);
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> session_headers;
    mock.SetHandler("POST", "/mcp", [&](const MCP_Request& req, MCP_Response& resp) {
        {
            std::lock_guard<std::mutex> lock(m);
            auto it = req.headers.find("mcp-session-id");
            session_headers.push_back(it == req.headers.end() ? "" : it->second);
            cv.notify_all();
        }
        std::string id = "1";
        try {
            auto body = mcp::JsonValue::Parse(req.body);
            if (auto* v = body.Find("id"); v && v->IsInt()) {
                id = std::to_string(v->GetInt());
            }
        } catch (...) {
        }
        resp.status_code = 200;
        resp.headers["content-type"] = "application/json";
        resp.headers["mcp-session-id"] = "sess-123";
        resp.body = "{\"jsonrpc\":\"2.0\",\"id\":" + id +
            ",\"result\":{\"resultType\":\"complete\"}}";
    });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    mcp::HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    mcp::StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);

    for (int64_t i = 1; i <= 2; ++i) {
        mcp::JsonRpcMessage req(mcp::JsonRpcRequest{});
        auto& rr = std::get<mcp::JsonRpcRequest>(req);
        rr.id = i;
        rr.method = "tools/call";
        transport->SendMessageAsync(std::move(req));
    }

    {
        std::unique_lock<std::mutex> lock(m);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                                [&] { return session_headers.size() >= 2; }));
    }
    EXPECT_EQ(session_headers[0], "");
    EXPECT_EQ(session_headers[1], "sess-123");

    transport->Close();
    mock.Stop();
}

// ── Client: Close() sends a DELETE carrying the session id when the server
// issued one (stateful session termination); stateless servers never see it ──
TEST(StreamableHttpTest, ClientSendsDeleteOnCloseWithSessionId) {
    auto port = PickFreePort(kTestBasePort + 1200);
    mcp::HttpServer mock(port);
    std::promise<void> post_seen;
    auto post_future = post_seen.get_future();
    std::mutex m;
    std::condition_variable cv;
    std::string delete_session;
    std::atomic<int> delete_calls{0};
    mock.SetHandler("POST", "/mcp", [&](const MCP_Request&, MCP_Response& resp) {
        post_seen.set_value();
        resp.status_code = 200;
        resp.headers["content-type"] = "application/json";
        resp.headers["mcp-session-id"] = "sess-456";
        resp.body = R"({"jsonrpc":"2.0","id":1,"result":{"resultType":"complete"}})";
    });
    mock.SetHandler("DELETE", "/mcp", [&](const MCP_Request& req, MCP_Response& resp) {
        resp.status_code = 200;
        {
            std::lock_guard<std::mutex> lock(m);
            auto it = req.headers.find("mcp-session-id");
            delete_session = it == req.headers.end() ? "" : it->second;
            delete_calls.fetch_add(1);
            cv.notify_all();
        }
    });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    mcp::HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    mcp::StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);

    mcp::JsonRpcMessage req(mcp::JsonRpcRequest{});
    auto& rr = std::get<mcp::JsonRpcRequest>(req);
    rr.id = int64_t(1);
    rr.method = "tools/call";
    transport->SendMessageAsync(std::move(req));
    ASSERT_EQ(post_future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    transport->Close();

    {
        std::unique_lock<std::mutex> lock(m);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                                [&] { return delete_calls.load() > 0; }));
    }
    EXPECT_EQ(delete_session, "sess-456");

    mock.Stop();
}
