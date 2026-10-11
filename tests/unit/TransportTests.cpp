#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/transport/SseClientTransport.hpp>
#include <mcp/Transport.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/test/McpTest.hpp>

#include "TestFakes.hpp"
#include "TestServerUtil.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace mcp;

namespace mcp {
namespace sse_client_impl {

std::chrono::milliseconds ReconnectDelayForAttempt(int attempt, std::optional<uint64_t> retry_ms);

} // namespace sse_client_impl
} // namespace mcp

namespace {

#ifdef _WIN32
using SseRetryStubSocket = SOCKET;
constexpr SseRetryStubSocket kSseRetryStubInvalidSocket = INVALID_SOCKET;
#else
using SseRetryStubSocket = int;
constexpr SseRetryStubSocket kSseRetryStubInvalidSocket = -1;
#endif

void SseRetryStubCloseSocket(SseRetryStubSocket s) {
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

bool SseRetryStubWaitReadable(SseRetryStubSocket s, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
    int rc = select(0, &rfds, nullptr, nullptr, &tv);
#else
    int rc = select(s + 1, &rfds, nullptr, nullptr, &tv);
#endif
    return rc > 0;
}

std::string SseRetryStubRecvHead(SseRetryStubSocket s) {
    std::string head;
    char buf[1024];
    while (head.find("\r\n\r\n") == std::string::npos) {
#ifdef _WIN32
        int n = recv(s, buf, sizeof(buf), 0);
#else
        ssize_t n = recv(s, buf, sizeof(buf), 0);
#endif
        if (n <= 0)
            break;
        head.append(buf, static_cast<std::size_t>(n));
        if (head.size() > std::size_t{65536})
            break;
    }
    return head;
}

void SseRetryStubSendAll(SseRetryStubSocket s, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#ifdef _WIN32
        int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
        ssize_t n = send(s, data.data() + sent, data.size() - sent, 0);
#endif
        if (n <= 0)
            break;
        sent += static_cast<std::size_t>(n);
    }
}

std::string SseRetryStubHeaderValue(const std::string& head, const char* name) {
    std::string want(name);
    for (char& c : want)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::size_t pos = head.find("\r\n");
    if (pos == std::string::npos)
        return {};
    pos += 2;
    while (pos < head.size()) {
        std::size_t end = head.find("\r\n", pos);
        std::string line = (end == std::string::npos) ? head.substr(pos) : head.substr(pos, end - pos);
        if (line.empty())
            break;
        std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            for (char& c : key)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (key == want) {
                std::string value = line.substr(colon + 1);
                std::size_t first = value.find_first_not_of(" \t");
                if (first == std::string::npos)
                    return {};
                value.erase(0, first);
                std::size_t last = value.find_last_not_of(" \t");
                if (last != std::string::npos)
                    value.erase(last + 1);
                return value;
            }
        }
        if (end == std::string::npos)
            break;
        pos = end + 2;
    }
    return {};
}

struct SseRetryStubState {
    std::mutex mutex;
    std::string first_last_event_id;
    bool first_seen = false;
    std::string second_last_event_id;
    bool second_seen = false;
    std::chrono::steady_clock::time_point first_close{};
    std::chrono::steady_clock::time_point second_arrival{};
    std::promise<void> second_arrived;
    std::promise<void> listening;
    std::atomic<bool> stop{false};
    std::atomic<bool> listening_ok{false};
    std::atomic<int> connections{0};
};

void SseRetryStubServe(uint16_t port, SseRetryStubState* state) {
    SseRetryStubSocket listen_s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_s == kSseRetryStubInvalidSocket) {
        state->listening.set_value();
        return;
    }
    int reuse = 1;
    setsockopt(listen_s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = bind(listen_s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
              listen(listen_s, 8) == 0;
    state->listening_ok.store(ok);
    state->listening.set_value();
    if (!ok) {
        SseRetryStubCloseSocket(listen_s);
        return;
    }
    while (!state->stop.load()) {
        if (!SseRetryStubWaitReadable(listen_s, 100))
            continue;
        sockaddr_in peer{};
#ifdef _WIN32
        int peer_len = sizeof(peer);
#else
        socklen_t peer_len = sizeof(peer);
#endif
        SseRetryStubSocket conn = accept(listen_s, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (conn == kSseRetryStubInvalidSocket)
            continue;
        int index = state->connections.fetch_add(1);
        std::string head = SseRetryStubRecvHead(conn);
        std::string last_event_id = SseRetryStubHeaderValue(head, "last-event-id");
        if (index == 0) {
            SseRetryStubSendAll(conn,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n"
                "id: event-1\nretry: 500\ndata: \n\nid: event-2\nretry: 500\nid: eve");
            std::lock_guard<std::mutex> lock(state->mutex);
            state->first_seen = true;
            state->first_last_event_id = last_event_id;
            state->first_close = std::chrono::steady_clock::now();
        } else {
            SseRetryStubSendAll(conn,
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n"
                "\r\n");
            std::lock_guard<std::mutex> lock(state->mutex);
            if (index == 1) {
                state->second_seen = true;
                state->second_last_event_id = last_event_id;
                state->second_arrival = std::chrono::steady_clock::now();
                state->second_arrived.set_value();
            }
        }
        SseRetryStubCloseSocket(conn);
    }
    SseRetryStubCloseSocket(listen_s);
}

} // namespace

TEST(TransportTest, InMemoryCreate) {
    auto pair = InMemoryTransport::CreatePair();

    EXPECT_TRUE(pair.client->SessionId().empty());
    EXPECT_TRUE(pair.server->SessionId().empty());
    EXPECT_FALSE(pair.client->IsStateless());
    EXPECT_FALSE(pair.server->IsStateless());

    JsonRpcRequest req;
    req.id = int64_t(1);
    req.method = "ping";
    pair.client->SendMessageAsync(JsonRpcMessage{req});
    pair.server->SendMessageAsync(JsonRpcMessage{req});

    pair.client->Close();
    pair.server->Close();
}

TEST(TransportTest, InMemoryPairMove) {
    auto pair = InMemoryTransport::CreatePair();
    auto server = std::move(pair.server);
    auto client = std::move(pair.client);
    EXPECT_NE(server, nullptr);
    EXPECT_NE(client, nullptr);
    server->Close();
    client->Close();
}

TEST(TransportTest, TransportBaseStateMachine) {
    auto pair = InMemoryTransport::CreatePair();
    auto* tb = dynamic_cast<TransportBase*>(pair.client.get());
    ASSERT_NE(tb, nullptr);
    EXPECT_EQ(tb->GetState(), TransportState::Initial);

    tb->SetConnected();
    EXPECT_EQ(tb->GetState(), TransportState::Connected);

    tb->SetDisconnected();
    EXPECT_EQ(tb->GetState(), TransportState::Disconnected);
}

TEST(TransportTest, TransportBaseErrorPropagation) {
    auto pair = InMemoryTransport::CreatePair();
    auto* tb = dynamic_cast<TransportBase*>(pair.client.get());
    ASSERT_NE(tb, nullptr);

    bool close_called = false;
    tb->SetOnClose([&close_called]() { close_called = true; });

    tb->SetConnected();
    tb->SetDisconnected();

    EXPECT_TRUE(close_called);
    EXPECT_EQ(tb->GetState(), TransportState::Disconnected);
}

TEST(TransportTest, InMemoryMessageSendNoCrash) {
    auto pair = InMemoryTransport::CreatePair();

    JsonRpcRequest req;
    req.id = int64_t(42);
    req.method = "tools/list";

    pair.client->SendMessageAsync(JsonRpcMessage{req});

    std::error_code ec;
    JsonRpcMessage received;
    pair.server->GetMessageChannel().AsyncReceive(
        [&](std::error_code recv_ec, JsonRpcMessage msg) {
            ec = recv_ec;
            received = std::move(msg);
        });

    ASSERT_FALSE(ec);
    const auto& req2 = std::get<JsonRpcRequest>(received);
    EXPECT_EQ(req2.method, "tools/list");
    EXPECT_EQ(req2.id, RequestId{int64_t(42)});

    pair.client->Close();
    pair.server->Close();
}

TEST(TransportTest, CloseThenSendDoesNotThrow) {
    auto pair = InMemoryTransport::CreatePair();
    pair.client->Close();
    pair.server->Close();

    JsonRpcRequest req;
    req.id = int64_t(1);
    req.method = "ping";
    EXPECT_NO_THROW(pair.client->SendMessageAsync(JsonRpcMessage{req}));
    EXPECT_NO_THROW(pair.server->SendMessageAsync(JsonRpcMessage{req}));
}

TEST(TransportTest, FakeTransportRecordsSends) {
    FakeTransport fake;
    fake.SetConnectResult(true);
    EXPECT_TRUE(fake.ConnectResult());
    EXPECT_FALSE(fake.Closed());
    EXPECT_TRUE(fake.Sent().empty());

    JsonRpcRequest initialize;
    initialize.id = int64_t(1);
    initialize.method = "initialize";
    fake.SendMessageAsync(JsonRpcMessage{initialize});

    JsonRpcRequest list_tools;
    list_tools.id = int64_t(2);
    list_tools.method = "tools/list";
    JsonRpcMessage last_sent{list_tools};
    fake.SendMessageAsync(last_sent);

    EXPECT_CALL_COUNT(fake, 2);
    EXPECT_EQ(fake.LastSent(), SerializeMessage(last_sent));

    auto first = DeserializeMessage(fake.Sent()[0].payload);
    ASSERT_TRUE(IsRequest(first));
    EXPECT_EQ(AsRequest(first)->id, RequestId{int64_t(1)});
    EXPECT_EQ(AsRequest(first)->method, "initialize");

    JsonRpcNotification initialized;
    initialized.method = "notifications/initialized";
    ASSERT_TRUE(fake.PushIncoming(JsonRpcMessage{initialized}));

    JsonRpcMessage received;
    fake.GetMessageChannel().AsyncReceive(
        [&received](std::error_code, JsonRpcMessage message) {
            received = std::move(message);
        });
    ASSERT_TRUE(IsNotification(received));
    EXPECT_EQ(AsNotification(received)->method, "notifications/initialized");

    fake.Close();
    EXPECT_TRUE(fake.Closed());
    EXPECT_FALSE(fake.GetMessageChannel().IsOpen());
}

TEST(SseClientRetryTest, RetryWindowClamped) {
    struct RetryWindowCase {
        int attempt;
        std::optional<uint64_t> retry_ms;
        int64_t expected_ms;
    };
    const RetryWindowCase cases[] = {
        {1, std::nullopt, 1000},
        {2, std::nullopt, 1000},
        {3, std::nullopt, 1000},
        {1, std::optional<uint64_t>{500}, 500},
        {2, std::optional<uint64_t>{500}, 500},
        {1, std::optional<uint64_t>{0}, 450},
        {1, std::optional<uint64_t>{1}, 450},
        {1, std::optional<uint64_t>{449}, 450},
        {1, std::optional<uint64_t>{450}, 450},
        {1, std::optional<uint64_t>{1000}, 1000},
        {1, std::optional<uint64_t>{1001}, 1000},
        {1, std::optional<uint64_t>{30000}, 1000},
        {1, std::optional<uint64_t>{UINT64_MAX}, 1000},
        {2, std::optional<uint64_t>{100}, 450},
    };
    for (const auto& c : cases) {
        EXPECT_EQ(mcp::sse_client_impl::ReconnectDelayForAttempt(c.attempt, c.retry_ms).count(),
                  c.expected_ms);
    }
    const std::optional<uint64_t> probes[] = {
        std::nullopt,
        std::optional<uint64_t>{0},
        std::optional<uint64_t>{1},
        std::optional<uint64_t>{449},
        std::optional<uint64_t>{450},
        std::optional<uint64_t>{500},
        std::optional<uint64_t>{999},
        std::optional<uint64_t>{1000},
        std::optional<uint64_t>{1001},
        std::optional<uint64_t>{5000},
        std::optional<uint64_t>{UINT64_MAX},
    };
    for (int attempt = 1; attempt <= 4; ++attempt) {
        for (const auto& retry : probes) {
            auto wait = mcp::sse_client_impl::ReconnectDelayForAttempt(attempt, retry);
            EXPECT_GE(wait.count(), 450);
            EXPECT_LE(wait.count(), 1000);
        }
    }
}

TEST(SseClientRetryTest, ReconnectCarriesLastEventId) {
#ifdef _WIN32
    WSADATA wsa_data{};
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
    uint16_t port = PickFreePort(static_cast<uint16_t>(kTestBasePort + 250));
    SseRetryStubState state;
    std::thread server([port, &state] { SseRetryStubServe(port, &state); });
    ASSERT_EQ(state.listening.get_future().wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    ASSERT_TRUE(state.listening_ok.load());

    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/sse";
    auto session = SseClientTransport(url).Connect();

    bool reconnected = state.second_arrived.get_future().wait_for(std::chrono::seconds(8)) ==
                       std::future_status::ready;
    session->Close();
    state.stop.store(true);
    server.join();

    ASSERT_TRUE(reconnected);
    ASSERT_TRUE(state.first_seen);
    ASSERT_TRUE(state.second_seen);
    EXPECT_TRUE(state.first_last_event_id.empty());
    EXPECT_EQ(state.second_last_event_id, "event-2");
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          state.second_arrival - state.first_close)
                          .count();
    EXPECT_GE(elapsed_ms, 400);
}