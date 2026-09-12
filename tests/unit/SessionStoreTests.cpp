#include <mcp/http/SessionStore.hpp>
#include <mcp/http/EventStore.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/test/McpTest.hpp>
#include <transport/detail/net/HttpClient.hpp>
#include "TestServerUtil.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <thread>

using namespace mcp;

namespace {

constexpr auto kHttpTimeout = std::chrono::milliseconds(2000);

// MessageChannel::AsyncReceive blocks until a message arrives; poll Empty()
// first so the subsequent receive returns immediately (single consumer).
bool WaitForChannelMessage(MessageChannel& channel,
                           std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (channel.Empty() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return !channel.Empty();
}

// Never throws: a failed request surfaces as status_code 0 for assertions.
mcp::detail::net::HttpResponseInfo TryRequest(
    mcp::detail::net::HttpClient& http,
    const mcp::detail::net::HttpRequestSpec& req) {
    try {
        return http.Request(req);
    } catch (const std::exception&) {
        return {};
    }
}

// Server-side POST handling parks on a promise until the message loop replies,
// so each request must be sent from its own thread while the main thread
// feeds the reply through the message channel.
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

// ── 回环：实例 A 创建会话（initialize 响应下发 Mcp-Session-Id），
//    实例 B（同 store、无本地会话）接管该 session 继续服务；
//    DELETE 后同 id 再请求得到 404。──
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
