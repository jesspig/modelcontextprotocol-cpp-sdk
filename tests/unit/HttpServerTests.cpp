#include <mcp/Methods.hpp>
#include <mcp/http/HttpServer.hpp>
#include <mcp/http/EventStore.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/protocol/WireCodec.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>

#include <http/McpParamHeaders.hpp>
#include <transport/detail/net/HttpClient.hpp>

#include <mcp/test/McpTest.hpp>
#include "TestServerUtil.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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

TEST(HttpServerTest, BindHostLoopbackAcceptsLoopback) {
    auto port = PickFreePort(kTestBasePort);
    mcp::HttpServerOptions opts;
    opts.bind_host = "127.0.0.1";
    mcp::HttpServer server(port, opts);
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

TEST(HttpServerTest, BindHostInvalidThrows) {
    auto port = PickFreePort(kTestBasePort);
    mcp::HttpServerOptions opts;
    opts.bind_host = "999.999.999.999";
    mcp::HttpServer server(port, opts);
    server.SetHandler("GET", "/ping", [](const MCP_Request&, MCP_Response& resp) {
        resp.body = "pong";
    });
    EXPECT_THROW(server.Start(), std::exception);
}

TEST(HttpServerTest, BindHostDefaultRegression) {
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
    server.Stop();
}

TEST(HttpServerTest, HostHeaderMissingRejectedNoCrash) {
    auto port = PickFreePort(kTestBasePort + 980);
    mcp::HttpServer server(port);
    server.SetHandler("GET", "/ping", [](const MCP_Request&, MCP_Response& resp) {
        resp.body = "pong";
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string first_line;
#ifdef _WIN32
    WSADATA wsa;
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &wsa), 0);
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_NE(sock, INVALID_SOCKET);
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(sock, 0);
#endif
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int connected = connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    ASSERT_EQ(connected, 0);

    std::string req = "GET /ping HTTP/1.1\r\n\r\n";
    int sent = send(sock, req.c_str(), static_cast<int>(req.size()), 0);
    ASSERT_GT(sent, 0);

    char buf[1024];
    int n = recv(sock, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = '\0';
        first_line = std::string(buf, static_cast<size_t>(n));
    }
#ifdef _WIN32
    closesocket(sock);
    WSACleanup();
#else
    close(sock);
#endif
    if (!first_line.empty()) {
        EXPECT_NE(first_line.find("403"), std::string::npos);
    }
    server.Stop();
}

TEST(HttpServerTest, BindHostIpv6LoopbackAccepts) {
#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
        SOCKET probe = socket(AF_INET6, SOCK_STREAM, 0);
        if (probe == INVALID_SOCKET) { WSACleanup(); return; }
        closesocket(probe);
        WSACleanup();
    }
#else
    {
        int probe = socket(AF_INET6, SOCK_STREAM, 0);
        if (probe < 0) return;
        close(probe);
    }
#endif
    auto port = PickFreePort(kTestBasePort);
    mcp::HttpServerOptions opts;
    opts.bind_host = "::1";
    mcp::HttpServer server(port, opts);
    server.SetHandler("GET", "/ping", [](const MCP_Request&, MCP_Response& resp) {
        resp.body = "pong";
    });
    server.Start();
    bool ipv6_ready = false;
    for (int i = 0; i < 50 && !ipv6_ready; ++i) {
#ifdef _WIN32
        SOCKET probe = socket(AF_INET6, SOCK_STREAM, 0);
        if (probe == INVALID_SOCKET) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        struct sockaddr_in6 pa {};
        pa.sin6_family = AF_INET6; pa.sin6_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET6, "::1", &pa.sin6_addr);
        if (connect(probe, reinterpret_cast<struct sockaddr*>(&pa), sizeof(pa)) == 0) ipv6_ready = true;
        closesocket(probe);
#else
        int probe = socket(AF_INET6, SOCK_STREAM, 0);
        if (probe < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        struct sockaddr_in6 pa {};
        pa.sin6_family = AF_INET6; pa.sin6_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET6, "::1", &pa.sin6_addr);
        if (connect(probe, reinterpret_cast<struct sockaddr*>(&pa), sizeof(pa)) == 0) ipv6_ready = true;
        close(probe);
#endif
        if (!ipv6_ready) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(ipv6_ready);

    std::string first_line;
#ifdef _WIN32
    WSADATA wsa; ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &wsa), 0);
    SOCKET sock = socket(AF_INET6, SOCK_STREAM, 0);
    ASSERT_NE(sock, INVALID_SOCKET);
#else
    int sock = socket(AF_INET6, SOCK_STREAM, 0);
    ASSERT_GE(sock, 0);
#endif
    struct sockaddr_in6 a6{};
    a6.sin6_family = AF_INET6;
    a6.sin6_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET6, "::1", &a6.sin6_addr);
    ASSERT_EQ(connect(sock, reinterpret_cast<struct sockaddr*>(&a6), sizeof(a6)), 0);
    std::string req = "GET /ping HTTP/1.1\r\nHost: [::1]\r\n\r\n";
    ASSERT_GT(send(sock, req.c_str(), static_cast<int>(req.size()), 0), 0);
    char buf[1024];
    int n = recv(sock, buf, sizeof(buf) - 1, 0);
    if (n > 0) { buf[n] = '\0'; first_line = std::string(buf, static_cast<size_t>(n)); }
#ifdef _WIN32
    closesocket(sock); WSACleanup();
#else
    close(sock);
#endif
    if (!first_line.empty()) EXPECT_NE(first_line.find("200"), std::string::npos);
    server.Stop();
}

TEST(HttpServerTest, BindHostInvalidIpv6Throws) {
    auto port = PickFreePort(kTestBasePort);
    mcp::HttpServerOptions opts;
    opts.bind_host = "gggg::1";
    mcp::HttpServer server(port, opts);
    server.SetHandler("GET", "/ping", [](const MCP_Request&, MCP_Response& resp) {
        resp.body = "pong";
    });
    EXPECT_THROW(server.Start(), std::exception);
}

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

TEST(StreamableHttpTest, McpHeadersValidation) {
    std::string error;
    auto body = mcp::JsonValue::Parse(R"({"jsonrpc":"2.0","method":"tools/list","id":1})");

    EXPECT_TRUE(mcp::StreamableHttpServerTransport::ValidateMcpHeaders("tools/list", "", body, error));
    EXPECT_TRUE(error.empty());

    EXPECT_FALSE(mcp::StreamableHttpServerTransport::ValidateMcpHeaders("tools/call", "", body, error));
    EXPECT_FALSE(error.empty());
}

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
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo","_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})",
        hdrs);
    ASSERT_NE(r, std::nullopt);
    EXPECT_EQ(r->status_code, 200);
    EXPECT_EQ(r->headers["content-type"], "text/event-stream");
    EXPECT_EQ(r->headers["mcp-param-foo"], "bar");

    handler->Close();
    transport->Close();
}

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
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo","_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})",
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
    req.body = R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})";
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
        R"({"jsonrpc":"2.0","id":7,"method":"unknown/method","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})",
        {});
    ASSERT_NE(r, std::nullopt);
    EXPECT_EQ(r->status_code, 404);
    EXPECT_EQ(r->headers["content-type"], "application/json");
    EXPECT_NE(r->body.find("-32601"), std::string::npos);

    handler->Close();
    transport->Close();
}

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

TEST(StreamableHttpTest, ClientOmitsMcpParamHeaders) {
    auto port = PickFreePort(kTestBasePort + 1250);
    mcp::HttpServer mock(port);
    std::mutex m;
    std::condition_variable cv;
    bool post_seen = false;
    std::unordered_map<std::string, std::string> seen_headers;
    mock.SetHandler("POST", "/mcp", [&](const MCP_Request& req, MCP_Response& resp) {
        {
            std::lock_guard<std::mutex> lock(m);
            seen_headers = req.headers;
            post_seen = true;
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
    mcp::StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);

    mcp::JsonValue params(mcp::JsonValue::object_tag);
    params["name"] = mcp::JsonValue("echo");
    params["text"] = mcp::JsonValue("hello");
    params["count"] = mcp::JsonValue(int64_t(3));
    params["ratio"] = mcp::JsonValue(0.5);
    params["token"] = mcp::JsonValue("secret-token");

    mcp::JsonRpcMessage req(mcp::JsonRpcRequest{});
    auto& rr = std::get<mcp::JsonRpcRequest>(req);
    rr.id = int64_t(1);
    rr.method = "tools/call";
    rr.params = std::move(params);
    transport->SendMessageAsync(std::move(req));

    std::unordered_map<std::string, std::string> received;
    {
        std::unique_lock<std::mutex> lock(m);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                                [&] { return post_seen; }));
        received = seen_headers;
    }

    EXPECT_EQ(received["mcp-method"], "tools/call");
    EXPECT_EQ(received["mcp-name"], "echo");
    for (const auto& entry : received)
        EXPECT_EQ(entry.first.find("mcp-param-"), std::string::npos);

    transport->Close();
    mock.Stop();
}

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

namespace {

std::string HeaderLookup(const std::unordered_map<std::string, std::string>& headers,
                         const std::string& name) {
    for (const auto& [key, value] : headers) {
        if (key.size() != name.size()) continue;
        bool equal = true;
        for (size_t i = 0; i < key.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(key[i])) !=
                std::tolower(static_cast<unsigned char>(name[i]))) {
                equal = false;
                break;
            }
        }
        if (equal) return value;
    }
    return {};
}

bool WaitForCondition(const std::function<bool()>& predicate, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

} // namespace

TEST(StreamableHttpTest, McpParamAnnotationParsesNestedPropertyPaths) {
    auto schema = mcp::JsonValue::Parse(R"({
        "type": "object",
        "properties": {
            "region": {"type": "string", "x-mcp-header": "Region"},
            "tenant": {
                "type": "object",
                "properties": {"id": {"type": "integer", "x-mcp-header": "Tenant-Id"}}
            },
            "query": {"type": "string"}
        }
    })");
    auto parsed = mcp::http_detail::ParseToolParamAnnotations(schema);
    ASSERT_TRUE(parsed.IsValid());
    ASSERT_EQ(parsed.annotations.size(), 2u);

    auto arguments = mcp::JsonValue::Parse(R"({"region":"us-west1","tenant":{"id":42}})");
    for (const auto& annotation : parsed.annotations) {
        auto value = mcp::http_detail::ValueAtPath(arguments, annotation.property_path);
        ASSERT_TRUE(value.has_value());
        auto encoded = mcp::http_detail::EncodeHeaderValue(*value);
        ASSERT_TRUE(encoded.has_value());
        if (annotation.header_name == "Region") EXPECT_EQ(*encoded, "us-west1");
        else EXPECT_EQ(*encoded, "42");
    }
}

TEST(StreamableHttpTest, McpParamAnnotationRejectsInvalidSchemas) {
    const char* kInvalidSchemas[] = {
        R"({"properties":{"a":{"type":"string","x-mcp-header":""}}})",
        R"({"properties":{"a":{"type":"string","x-mcp-header":"Bad Name"}}})",
        R"({"properties":{"a":{"type":"number","x-mcp-header":"Num"}}})",
        R"({"properties":{"a":{"type":"string","x-mcp-header":"Dup"},"b":{"type":"string","x-mcp-header":"dup"}}})",
        R"({"properties":{"a":{"type":"array","items":{"type":"string","x-mcp-header":"X"}}}})",
        R"({"properties":{"a":{"oneOf":[{"type":"string","x-mcp-header":"Y"}]}}})",
    };
    for (const char* schema_json : kInvalidSchemas) {
        auto parsed = mcp::http_detail::ParseToolParamAnnotations(
            mcp::JsonValue::Parse(schema_json));
        EXPECT_FALSE(parsed.IsValid());
    }
}

TEST(StreamableHttpTest, McpParamValueEncodingMatchesSpecExamples) {
    struct Case { const char* json; const char* expected; };
    const Case kCases[] = {
        {"\"us-west1\"", "us-west1"},
        {"\"Hello, \\u4e16\\u754c\"", "=?base64?SGVsbG8sIOS4lueVjA==?="},
        {"\" padded \"", "=?base64?IHBhZGRlZCA=?="},
        {"\"line1\\nline2\"", "=?base64?bGluZTEKbGluZTI=?="},
        {"\"=?base64?literal?=\"", "=?base64?PT9iYXNlNjQ/bGl0ZXJhbD89?="},
        {"true", "true"},
        {"42", "42"},
    };
    for (const auto& test_case : kCases) {
        auto value = mcp::JsonValue::Parse(test_case.json);
        auto encoded = mcp::http_detail::EncodeHeaderValue(value);
        ASSERT_TRUE(encoded.has_value());
        EXPECT_EQ(*encoded, test_case.expected);
    }
}

TEST(StreamableHttpTest, McpNameHeaderDecodesBase64Sentinel) {
    auto body = mcp::JsonValue::Parse(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"Hello, \u4e16\u754c"}})");
    std::string error;
    EXPECT_TRUE(mcp::StreamableHttpServerTransport::ValidateMcpHeaders(
        "tools/call", "=?base64?SGVsbG8sIOS4lueVjA==?=", body, error));
    EXPECT_TRUE(error.empty());

    EXPECT_FALSE(mcp::StreamableHttpServerTransport::ValidateMcpHeaders(
        "tools/call", "=?base64?b3RoZXI=?=", body, error));
}

TEST(StreamableHttpTest, UnauthorizedChallengeCarriesScopes) {
    auto port = PickFreePort(kTestBasePort + 1100);

    mcp::StreamableHttpServerOptions opts;
    opts.port = port;
    opts.endpoint = "/mcp";
    opts.stateless = true;
    opts.enable_legacy_sse = false;
    mcp::StreamableHttpServerOptions::BearerAuthConfig bearer;
    bearer.verify = [](const std::string&) { return mcp::AuthResult{}; };
    bearer.resource_metadata_url =
        "https://example.com/.well-known/oauth-protected-resource";
    bearer.scopes_supported = {"files:read", "files:write"};
    bearer.serve_metadata_endpoint = false;
    opts.bearer_auth = bearer;

    auto transport = std::make_shared<mcp::StreamableHttpServerTransport>(opts);
    transport->Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::unordered_map<std::string, std::string> hdrs;
    hdrs["Content-Type"] = "application/json";
    auto r = HttpPost("http://127.0.0.1:" + std::to_string(port) + "/mcp",
                      R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", hdrs);
    ASSERT_NE(r, std::nullopt);
    EXPECT_EQ(r->status_code, 401);
    const auto challenge = HeaderLookup(r->headers, "www-authenticate");
    EXPECT_NE(challenge.find("resource_metadata="), std::string::npos);
    EXPECT_NE(challenge.find("scope=\"files:read files:write\""), std::string::npos);

    transport->Close();
}

TEST(StreamableHttpTest, ServerValidatesMcpParamHeadersAgainstBody) {
    auto port = PickFreePort(kTestBasePort + 1120);

    mcp::StreamableHttpServerOptions opts;
    opts.port = port;
    opts.endpoint = "/mcp";
    opts.stateless = true;
    opts.enable_legacy_sse = false;
    opts.resolve_param_annotations =
        [](const std::string& method, const std::string& name) {
            std::vector<mcp::McpParamAnnotationInfo> resolved;
            if (method == "tools/call" && name == "execute_sql") {
                resolved.push_back({{"region"}, "Region"});
            }
            return resolved;
        };

    auto transport = std::make_shared<mcp::StreamableHttpServerTransport>(opts);
    auto handler = std::make_shared<mcp::McpSessionHandler>(
        transport, mcp::MakeWireCodec(std::string(mcp::kLatestProtocolVersion)));
    handler->SetRequestHandler(mcp::methods::kCallTool,
        [](const mcp::JsonRpcRequest&, std::promise<mcp::JsonValue> p) {
            p.set_value(mcp::JsonValue(mcp::JsonValue::object_tag));
        });
    handler->Start();
    transport->Start();
    ASSERT_TRUE(WaitUntilReady(port));

    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    const char* kBody =
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"execute_sql","arguments":{"region":"us-west1"}}})";

    auto base_headers = [&] {
        std::unordered_map<std::string, std::string> hdrs;
        hdrs["Content-Type"] = "application/json";
        hdrs["Mcp-Method"] = "tools/call";
        hdrs["Mcp-Name"] = "execute_sql";
        return hdrs;
    };

    {
        auto hdrs = base_headers();
        hdrs["Mcp-Param-Region"] = "us-west1";
        auto r = HttpPost(url, kBody, hdrs);
        ASSERT_NE(r, std::nullopt);
        EXPECT_NE(r->status_code, 400);
    }

    {
        auto hdrs = base_headers();
        hdrs["Mcp-Param-Region"] = "us-east1";
        auto r = HttpPost(url, kBody, hdrs);
        ASSERT_NE(r, std::nullopt);
        EXPECT_EQ(r->status_code, 400);
        EXPECT_NE(r->body.find("-32020"), std::string::npos);
    }

    {
        auto hdrs = base_headers();
        auto r = HttpPost(url, kBody, hdrs);
        ASSERT_NE(r, std::nullopt);
        EXPECT_EQ(r->status_code, 400);
    }

    handler->Close();
    transport->Close();
}

TEST(StreamableHttpTest, ClientMirrorsAnnotatedToolArguments) {
    auto port = PickFreePort(kTestBasePort + 1140);
    mcp::HttpServer mock(port);

    std::mutex m;
    std::unordered_map<std::string, std::string> call_headers;
    std::atomic<bool> saw_call{false};

    mock.SetHandler("POST", "/mcp", [&](const MCP_Request& req, MCP_Response& resp) {
        auto body = mcp::JsonValue::Parse(req.body);
        std::string method;
        if (auto* mth = body.Find("method"); mth && mth->IsString()) method = mth->GetString();
        resp.headers["content-type"] = "application/json";

        if (method == "tools/list") {
            resp.body = R"({"jsonrpc":"2.0","id":1,"result":{"tools":[{"name":"execute_sql","inputSchema":{"type":"object","properties":{"region":{"type":"string","x-mcp-header":"Region"},"query":{"type":"string"}}}}]}})";
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m);
            call_headers = req.headers;
        }
        saw_call.store(true);
        resp.body = R"({"jsonrpc":"2.0","id":2,"result":{"content":[]}})";
    });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    mcp::HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    opts.transport_mode = mcp::HttpTransportMode::StreamableHttp;
    opts.enable_listen_stream = false;
    mcp::StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);

    std::atomic<bool> got_list_response{false};
    std::thread reader([&] {
        transport->GetMessageChannel().AsyncReceive(
            [&](std::error_code ec, mcp::JsonRpcMessage) {
                if (!ec) got_list_response.store(true);
            });
    });

    mcp::JsonRpcRequest list_request;
    list_request.id = int64_t(1);
    list_request.method = "tools/list";
    transport->SendMessageAsync(mcp::JsonRpcMessage(std::move(list_request)));

    EXPECT_TRUE(WaitForCondition([&] { return got_list_response.load(); }, 5000));

    mcp::JsonRpcRequest call_request;
    call_request.id = int64_t(2);
    call_request.method = "tools/call";
    {
        mcp::JsonValue params(mcp::JsonValue::object_tag);
        params["name"] = mcp::JsonValue("execute_sql");
        mcp::JsonValue arguments(mcp::JsonValue::object_tag);
        arguments["region"] = mcp::JsonValue("us-west1");
        arguments["query"] = mcp::JsonValue("SELECT 1");
        params["arguments"] = std::move(arguments);
        call_request.params = std::move(params);
    }
    transport->SendMessageAsync(mcp::JsonRpcMessage(std::move(call_request)));

    EXPECT_TRUE(WaitForCondition([&] { return saw_call.load(); }, 5000));
    std::unordered_map<std::string, std::string> headers;
    {
        std::lock_guard<std::mutex> lock(m);
        headers = call_headers;
    }
    EXPECT_EQ(HeaderLookup(headers, "Mcp-Method"), "tools/call");
    EXPECT_EQ(HeaderLookup(headers, "Mcp-Name"), "execute_sql");
    EXPECT_EQ(HeaderLookup(headers, "Mcp-Param-Region"), "us-west1");
    EXPECT_TRUE(HeaderLookup(headers, "Mcp-Param-Query").empty());

    transport->Close();
    if (reader.joinable()) reader.join();
    mock.Stop();
}
