// HttpServerTests — unit tests for HttpServer, EventStore, and StreamableHttp transports

#include <mcp/Methods.hpp>
#include <mcp/http/HttpServer.hpp>
#include <mcp/http/EventStore.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/protocol/WireCodec.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>

#include <transport/detail/net/HttpClient.hpp>

#include <gtest/gtest.h>
#include "TestServerUtil.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

// Avoid `using namespace mcp;` — HttpRequest/HttpResponse clash between hv and mcp
using MCP_Request = mcp::HttpRequest;
using MCP_Response = mcp::HttpResponse;

namespace {

using NetResp = mcp::detail::net::HttpResponseInfo;

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
