#include <mcp/http/SessionStore.hpp>
#include <mcp/http/EventStore.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <mcp/JsonRpc.hpp>
#include <gtest/gtest.h>
#include <transport/detail/net/HttpClient.hpp>
#include "TestServerUtil.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace mcp;

namespace {

constexpr auto kHttpTimeout = std::chrono::milliseconds(2000);

bool WaitForChannelMessage(MessageChannel& channel,
                           std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (channel.Empty() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return !channel.Empty();
}

mcp::detail::net::HttpResponseInfo TryRequest(
    mcp::detail::net::HttpClient& http,
    const mcp::detail::net::HttpRequestSpec& req) {
    try {
        return http.Request(req);
    } catch (const std::exception&) {
        return {};
    }
}

struct HttpThread {
    std::optional<mcp::detail::net::HttpResponseInfo> response;
    std::thread thread;
    void Run(mcp::detail::net::HttpClient& http,
             const mcp::detail::net::HttpRequestSpec& req) {
        thread = std::thread([this, &http, &req] {
            try {
                response = http.Request(req);
            } catch (const std::exception&) {
                response = std::nullopt;
            }
        });
    }
    void Wait() {
        if (thread.joinable()) thread.join();
    }
    ~HttpThread() {
        Wait();
    }
};

void ReplyNextRequestWithResult(StreamableHttpServerTransport& server,
                                MessageChannel& channel,
                                const char* result_json) {
    JsonRpcMessage msg;
    channel.AsyncReceive([&](std::error_code ec, JsonRpcMessage received) {
        if (!ec) msg = std::move(received);
    });
    auto* request = AsRequest(msg);
    if (!request) return;
    JsonRpcResponse response;
    response.id = request->id;
    response.result = JsonValue::Parse(result_json);
    server.SendMessageAsync(std::move(response));
}

} // namespace

TEST(SessionStoreTest, InMemorySaveLoadRoundtrip) {
    InMemorySessionStore store;
    SessionRecord record;
    record.protocol_version = "2025-06-18";
    record.created_at_ms = 1726100000000LL;
    store.Save("sess-1", record);

    auto loaded = store.Load("sess-1");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->protocol_version, "2025-06-18");
    EXPECT_EQ(loaded->created_at_ms, 1726100000000LL);
}

TEST(SessionStoreTest, InMemoryLoadUnknownReturnsNullopt) {
    InMemorySessionStore store;
    EXPECT_FALSE(store.Load("missing").has_value());
}

TEST(SessionStoreTest, InMemoryRemoveMakesLoadMiss) {
    InMemorySessionStore store;
    SessionRecord record;
    record.protocol_version = "2025-03-26";
    record.created_at_ms = 1;
    store.Save("sess-2", record);
    EXPECT_TRUE(store.Load("sess-2").has_value());
    store.Remove("sess-2");
    EXPECT_FALSE(store.Load("sess-2").has_value());
    store.Remove("sess-2");
    EXPECT_FALSE(store.Load("sess-2").has_value());
}

TEST(SessionStoreTest, InMemoryOverwriteExisting) {
    InMemorySessionStore store;
    SessionRecord first;
    first.protocol_version = "2025-03-26";
    first.created_at_ms = 1;
    store.Save("sess-3", first);
    SessionRecord second;
    second.protocol_version = "2025-06-18";
    second.created_at_ms = 2;
    store.Save("sess-3", second);

    auto loaded = store.Load("sess-3");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->protocol_version, "2025-06-18");
    EXPECT_EQ(loaded->created_at_ms, 2);
}

TEST(SessionStoreTest, ServerAdoptsSessionCreatedByOtherInstance) {
    auto session_store = std::make_shared<InMemorySessionStore>();
    auto event_store = std::make_shared<EventStore>();

    auto port_a = PickFreePort(kTestBasePort + 1700);
    StreamableHttpServerOptions opts_a;
    opts_a.port = port_a;
    opts_a.stateless = false;
    opts_a.event_store = event_store;
    opts_a.session_store = session_store;
    StreamableHttpServerTransport server_a(opts_a);
    server_a.Start();
    ASSERT_TRUE(WaitUntilReady(port_a));

    mcp::detail::net::HttpClient http;
    mcp::detail::net::HttpRequestSpec init_req;
    init_req.method = "POST";
    init_req.url = "http://127.0.0.1:" + std::to_string(port_a) + "/mcp";
    init_req.body =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
        R"("protocolVersion":"2025-06-18","capabilities":{},)"
        R"("clientInfo":{"name":"test","version":"0"}}})";
    init_req.headers["content-type"] = "application/json";
    init_req.timeout = kHttpTimeout;
    HttpThread init_call;
    init_call.Run(http, init_req);

    ASSERT_TRUE(WaitForChannelMessage(server_a.GetMessageChannel(),
                                      std::chrono::milliseconds(5000)));
    ReplyNextRequestWithResult(
        server_a, server_a.GetMessageChannel(),
        R"({"protocolVersion":"2025-06-18","capabilities":{},)"
        R"("serverInfo":{"name":"instance-a","version":"0"}})");
    init_call.Wait();

    ASSERT_TRUE(init_call.response.has_value());
    ASSERT_EQ(init_call.response->status_code, 200);
    auto sid_it = init_call.response->headers.find("mcp-session-id");
    ASSERT_TRUE(sid_it != init_call.response->headers.end());
    ASSERT_FALSE(sid_it->second.empty());
    std::string session_id = sid_it->second;

    auto port_b = PickFreePort(static_cast<uint16_t>(port_a + 50));
    StreamableHttpServerOptions opts_b;
    opts_b.port = port_b;
    opts_b.stateless = false;
    opts_b.event_store = event_store;
    opts_b.session_store = session_store;
    StreamableHttpServerTransport server_b(opts_b);
    server_b.Start();
    ASSERT_TRUE(WaitUntilReady(port_b));

    mcp::detail::net::HttpRequestSpec call_req;
    call_req.method = "POST";
    call_req.url = "http://127.0.0.1:" + std::to_string(port_b) + "/mcp";
    call_req.body = R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})";
    call_req.headers["content-type"] = "application/json";
    call_req.headers["mcp-session-id"] = session_id;
    call_req.timeout = kHttpTimeout;
    HttpThread adopt_call;
    adopt_call.Run(http, call_req);

    ASSERT_TRUE(WaitForChannelMessage(server_b.GetMessageChannel(),
                                      std::chrono::milliseconds(5000)));
    ReplyNextRequestWithResult(
        server_b, server_b.GetMessageChannel(), R"({"tools":[]})");
    adopt_call.Wait();

    ASSERT_TRUE(adopt_call.response.has_value());
    ASSERT_EQ(adopt_call.response->status_code, 200);
    EXPECT_NE(adopt_call.response->body.find("tools"), std::string::npos);

    mcp::detail::net::HttpRequestSpec del_req;
    del_req.method = "DELETE";
    del_req.url = "http://127.0.0.1:" + std::to_string(port_b) + "/mcp";
    del_req.headers["mcp-session-id"] = session_id;
    del_req.timeout = kHttpTimeout;
    auto del_resp = TryRequest(http, del_req);
    ASSERT_EQ(del_resp.status_code, 200);
    EXPECT_FALSE(session_store->Load(session_id).has_value());

    auto gone_resp = TryRequest(http, call_req);
    EXPECT_EQ(gone_resp.status_code, 404);

    server_b.Close();
    server_a.Close();
}

namespace {

int64_t SessionTestNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

TEST(SessionStoreTest, IdleExpiryReturnsNulloptAfter30Min) {
    InMemorySessionStore store;
    auto now = SessionTestNowMs();
    SessionRecord record;
    record.protocol_version = "2025-06-18";
    record.created_at_ms = now - 31LL * 60 * 1000;
    record.last_seen_ms = now - 31LL * 60 * 1000;
    ASSERT_TRUE(store.TrySave("sess-idle", record));
    EXPECT_FALSE(store.Load("sess-idle").has_value());
    EXPECT_EQ(store.Size(), size_t{0});
}

TEST(SessionStoreTest, LoadRefreshesSlidingWindow) {
    InMemorySessionStore store;
    auto now = SessionTestNowMs();
    SessionRecord record;
    record.protocol_version = "2025-06-18";
    record.created_at_ms = now - 29LL * 60 * 1000;
    record.last_seen_ms = now - 29LL * 60 * 1000;
    ASSERT_TRUE(store.TrySave("sess-slide", record));
    auto loaded = store.Load("sess-slide");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_GE(loaded->last_seen_ms, record.last_seen_ms);
    auto reloaded = store.Load("sess-slide");
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_GE(reloaded->last_seen_ms, loaded->last_seen_ms);
}

TEST(SessionStoreTest, CapacityRejectsBeyond10000) {
    InMemorySessionStore store;
    SessionRecord record;
    record.protocol_version = "2025-06-18";
    for (size_t i = 0; i < mcp::detail::kMaxHttpSessions; ++i)
        ASSERT_TRUE(store.TrySave("sess-" + std::to_string(i), record));
    EXPECT_EQ(store.Size(), mcp::detail::kMaxHttpSessions);
    EXPECT_FALSE(store.TrySave("sess-overflow", record));
    EXPECT_EQ(store.Size(), mcp::detail::kMaxHttpSessions);
    SessionRecord update;
    update.protocol_version = "2026-07-28";
    EXPECT_TRUE(store.TrySave("sess-0", update));
    EXPECT_EQ(store.Size(), mcp::detail::kMaxHttpSessions);
    EXPECT_THROW(store.Save("sess-overflow", record), std::runtime_error);
}

TEST(SessionStoreTest, ExpiredSessionsReclaimedOnNextSave) {
    InMemorySessionStore store;
    auto now = SessionTestNowMs();
    SessionRecord stale;
    stale.protocol_version = "2025-06-18";
    stale.created_at_ms = now - 31LL * 60 * 1000;
    stale.last_seen_ms = now - 31LL * 60 * 1000;
    ASSERT_TRUE(store.TrySave("stale-1", stale));
    EXPECT_EQ(store.SweepExpired(), size_t{1});
    EXPECT_EQ(store.Size(), size_t{0});

    ASSERT_TRUE(store.TrySave("stale-2", stale));
    SessionRecord fresh;
    fresh.protocol_version = "2025-06-18";
    fresh.last_seen_ms = now;
    ASSERT_TRUE(store.TrySave("fresh-1", fresh));
    EXPECT_EQ(store.Size(), size_t{1});
    EXPECT_FALSE(store.Load("stale-2").has_value());
    EXPECT_TRUE(store.Load("fresh-1").has_value());
}

TEST(SessionStoreTest, OversizedBodyReturns413) {
    auto port = PickFreePort(kTestBasePort + 1850);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    StreamableHttpServerTransport server(opts);
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

    std::string req = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(mcp::detail::kMaxHttpBodyBytes + 1) + "\r\n\r\n";
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
    ASSERT_FALSE(first_line.empty());
    EXPECT_NE(first_line.find("413"), std::string::npos);

    server.Close();
}

TEST(SessionStoreTest, MetadataEndpointAnonymous) {
    auto port = PickFreePort(kTestBasePort + 1860);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    opts.enable_legacy_sse = false;
    StreamableHttpServerOptions::BearerAuthConfig bearer;
    bearer.verify = [](const std::string&) { return AuthResult{}; };
    bearer.resource_metadata_url = "http://127.0.0.1:" + std::to_string(port) +
        "/.well-known/oauth-protected-resource";
    bearer.scopes_supported = {"mcp:read"};
    bearer.serve_metadata_endpoint = true;
    opts.bearer_auth = bearer;
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    mcp::detail::net::HttpClient http;
    mcp::detail::net::HttpRequestSpec meta_req;
    meta_req.method = "GET";
    meta_req.url = "http://127.0.0.1:" + std::to_string(port) +
        "/.well-known/oauth-protected-resource";
    meta_req.timeout = kHttpTimeout;
    auto meta_resp = TryRequest(http, meta_req);
    EXPECT_EQ(meta_resp.status_code, 200);

    mcp::detail::net::HttpRequestSpec mcp_req;
    mcp_req.method = "POST";
    mcp_req.url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    mcp_req.body = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
    mcp_req.timeout = kHttpTimeout;
    auto mcp_resp = TryRequest(http, mcp_req);
    EXPECT_EQ(mcp_resp.status_code, 401);

    server.Close();
}
