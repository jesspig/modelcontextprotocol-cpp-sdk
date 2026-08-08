// HttpServerTests — unit tests for HttpServer, EventStore, and StreamableHttp transports

#include <mcp/http/HttpServer.hpp>
#include <mcp/http/EventStore.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>

#include <hv/requests.h>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

// Avoid `using namespace mcp;` — HttpRequest/HttpResponse clash between hv and mcp
using MCP_Request = mcp::HttpRequest;
using MCP_Response = mcp::HttpResponse;

static const uint16_t kBasePort = 18765;

namespace {

// Port availability probe: a fresh bind succeeds only when the port is free.
bool PortIsFree(uint16_t port) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rc = bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    closesocket(s);
    return rc == 0;
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rc = ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::close(s);
    return rc == 0;
#endif
}

// Pick a free port near the preferred one so parallel test runs don't collide.
uint16_t PickFreePort(uint16_t preferred) {
    for (uint16_t p = preferred; p < preferred + 100; ++p) {
        if (PortIsFree(p)) return p;
    }
    return preferred;
}

// Poll until the server answers any request (ready) or the deadline passes.
bool WaitUntilReady(uint16_t port) {
    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/ready";
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        auto r = requests::get(url.c_str());
        if (r) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

} // namespace

// ============================================================
// HttpServer
// ============================================================
TEST(HttpServerTest, GetPing) {
    auto port = PickFreePort(kBasePort);
    mcp::HttpServer server(port);
    server.SetHandler("GET", "/ping", [](const MCP_Request&, MCP_Response& resp) {
        resp.body = "pong";
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto r = requests::get(("http://127.0.0.1:" + std::to_string(port) + "/ping").c_str());
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->status_code, 200);
    EXPECT_EQ(r->body, "pong");
    server.Stop();
}

TEST(HttpServerTest, PostEcho) {
    auto port = PickFreePort(kBasePort);
    mcp::HttpServer server(port);
    server.SetHandler("POST", "/echo", [](const MCP_Request& req, MCP_Response& resp) {
        resp.body = req.body;
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto r = requests::post(("http://127.0.0.1:" + std::to_string(port) + "/echo").c_str(), "hello");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->status_code, 200);
    EXPECT_EQ(r->body, "hello");
    server.Stop();
}

TEST(HttpServerTest, NotFound) {
    auto port = PickFreePort(kBasePort);
    mcp::HttpServer server(port);
    server.SetHandler("GET", "/ping", [](const MCP_Request&, MCP_Response& resp) {
        resp.body = "pong";
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto r = requests::get(("http://127.0.0.1:" + std::to_string(port) + "/nonexistent").c_str());
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->status_code, 404);
    server.Stop();
}

TEST(HttpServerTest, MultipleHandlers) {
    auto port = PickFreePort(kBasePort);
    mcp::HttpServer server(port);
    server.SetHandler("GET", "/a", [](const MCP_Request&, MCP_Response& resp) { resp.body = "A"; });
    server.SetHandler("GET", "/b", [](const MCP_Request&, MCP_Response& resp) { resp.body = "B"; });
    server.SetHandler("POST", "/a", [](const MCP_Request&, MCP_Response& resp) { resp.body = "A-post"; });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto base = "http://127.0.0.1:" + std::to_string(port);
    auto r1 = requests::get((base + "/a").c_str());
    auto r2 = requests::get((base + "/b").c_str());
    auto r3 = requests::post((base + "/a").c_str(), "");
    ASSERT_NE(r1, nullptr);
    ASSERT_NE(r2, nullptr);
    ASSERT_NE(r3, nullptr);
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
