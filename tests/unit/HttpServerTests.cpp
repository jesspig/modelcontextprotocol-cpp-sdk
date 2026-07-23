#include <mcp/http/HttpServer.hpp>
#include <mcp/http/EventStore.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>

#include <hv/requests.h>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

// ponytail: no using namespace mcp; — HttpRequest/HttpResponse clash hv vs mcp
using MCP_Request = mcp::HttpRequest;
using MCP_Response = mcp::HttpResponse;

static const uint16_t kBasePort = 18765;

// ============================================================
// HttpServer
// ============================================================
TEST(HttpServerTest, GetPing) {
    mcp::HttpServer server(kBasePort);
    server.SetHandler("GET", "/ping", [](const MCP_Request&, MCP_Response& resp) {
        resp.body = "pong";
    });
    server.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto r = requests::get("http://127.0.0.1:18765/ping");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->status_code, 200);
    EXPECT_EQ(r->body, "pong");
    server.Stop();
}

TEST(HttpServerTest, PostEcho) {
    mcp::HttpServer server(kBasePort);
    server.SetHandler("POST", "/echo", [](const MCP_Request& req, MCP_Response& resp) {
        resp.body = req.body;
    });
    server.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto r = requests::post("http://127.0.0.1:18765/echo", "hello");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->status_code, 200);
    EXPECT_EQ(r->body, "hello");
    server.Stop();
}

TEST(HttpServerTest, NotFound) {
    mcp::HttpServer server(kBasePort);
    server.SetHandler("GET", "/ping", [](const MCP_Request&, MCP_Response& resp) {
        resp.body = "pong";
    });
    server.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto r = requests::get("http://127.0.0.1:18765/nonexistent");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->status_code, 404);
    server.Stop();
}

TEST(HttpServerTest, MultipleHandlers) {
    mcp::HttpServer server(kBasePort);
    server.SetHandler("GET", "/a", [](const MCP_Request&, MCP_Response& resp) { resp.body = "A"; });
    server.SetHandler("GET", "/b", [](const MCP_Request&, MCP_Response& resp) { resp.body = "B"; });
    server.SetHandler("POST", "/a", [](const MCP_Request&, MCP_Response& resp) { resp.body = "A-post"; });
    server.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(requests::get("http://127.0.0.1:18765/a")->body, "A");
    EXPECT_EQ(requests::get("http://127.0.0.1:18765/b")->body, "B");
    EXPECT_EQ(requests::post("http://127.0.0.1:18765/a", "")->body, "A-post");
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
    EXPECT_EQ(events[0], "event2");

    EXPECT_TRUE(store.HasEvents("sess1"));
    store.Clear("sess1");
    EXPECT_FALSE(store.HasEvents("sess1"));
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
