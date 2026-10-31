#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/transport/SseClientTransport.hpp>
#include <mcp/transport/StdioClientTransport.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <mcp/transport/detail/PlatformIO.hpp>
#include <mcp/McpError.hpp>
#include <mcp/Transport.hpp>
#include <mcp/JsonRpc.hpp>
#include <gtest/gtest.h>

#include "TestFakes.hpp"
#include "TestServerUtil.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef MCP_HAVE_OPENSSL
#include <openssl/asn1.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include "TlsTestCertificate.hpp"
#include <transport/detail/net/TlsSocket.hpp>
#endif

using namespace mcp;

namespace mcp { namespace detail {

#ifdef _WIN32
std::unique_ptr<PipeHandle> CreatePipeFromHandle(HANDLE handle, bool overlapped);
#else
std::unique_ptr<PipeHandle> CreatePipeFromFd(int fd);
#endif

}} // namespace mcp::detail

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

TEST(TransportTest, StdioFrameBoundDefaultsToLimit) {
    StdioClientTransportOptions opts;
    opts.command = "cat";
    ASSERT_TRUE(opts.max_frame_bytes.has_value());
    EXPECT_EQ(*opts.max_frame_bytes, mcp::detail::kMaxStdioFrameBytes);
}

namespace {

struct TestPipePair {
    std::unique_ptr<mcp::detail::PipeHandle> pipe;
#ifdef _WIN32
    HANDLE peer = INVALID_HANDLE_VALUE;
#else
    int peer = -1;
#endif

    ~TestPipePair() { ClosePeer(); }

    void ClosePeer() {
#ifdef _WIN32
        if (peer != INVALID_HANDLE_VALUE) {
            CloseHandle(peer);
            peer = INVALID_HANDLE_VALUE;
        }
#else
        if (peer >= 0) {
            ::close(peer);
            peer = -1;
        }
#endif
    }

    size_t ReadPeer(char* buffer, size_t size) {
#ifdef _WIN32
        DWORD available = 0;
        if (!PeekNamedPipe(peer, nullptr, 0, nullptr, &available, nullptr)) return 0;
        if (available == 0) return 0;
        DWORD got = 0;
        if (!ReadFile(peer, buffer, static_cast<DWORD>(size), &got, nullptr)) return 0;
        return static_cast<size_t>(got);
#else
        ssize_t n = ::read(peer, buffer, size);
        return n > 0 ? static_cast<size_t>(n) : 0;
#endif
    }
};

void CreateTestPipePair(TestPipePair& pair, bool writing_end) {
#ifdef _WIN32
    static std::atomic<unsigned long long> counter{0};
    std::wstring name = std::wstring(L"\\\\.\\pipe\\mcp-pipe-test-") +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(counter.fetch_add(1));
    DWORD open_mode = writing_end ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND;
    open_mode |= FILE_FLAG_OVERLAPPED;
    HANDLE server = CreateNamedPipeW(name.c_str(), open_mode,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr);
    if (server == INVALID_HANDLE_VALUE) return;
    HANDLE client = CreateFileW(name.c_str(), writing_end ? GENERIC_READ : GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (client == INVALID_HANDLE_VALUE) {
        CloseHandle(server);
        return;
    }
    if (!ConnectNamedPipe(server, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
        CloseHandle(client);
        CloseHandle(server);
        return;
    }
    pair.pipe = mcp::detail::CreatePipeFromHandle(server, true);
    pair.peer = client;
#else
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) return;
    int flags = ::fcntl(fds[writing_end ? 0 : 1], F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(fds[writing_end ? 0 : 1], F_SETFL, flags | O_NONBLOCK);
    pair.pipe = mcp::detail::CreatePipeFromFd(writing_end ? fds[1] : fds[0]);
    pair.peer = writing_end ? fds[0] : fds[1];
#endif
}

std::unique_ptr<mcp::detail::PipeHandle> CreateBrokenPipe() {
#ifdef _WIN32
    HANDLE bogus = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(0x1234));
    return mcp::detail::CreatePipeFromHandle(bogus, false);
#else
    constexpr int kNeverOpenedFd = 4096;
    if (::fcntl(kNeverOpenedFd, F_GETFD, 0) != -1 || errno != EBADF) return nullptr;
    return mcp::detail::CreatePipeFromFd(kNeverOpenedFd);
#endif
}

class PartialWritePipe : public mcp::detail::PipeHandle {
public:
    explicit PartialWritePipe(std::size_t chunk) : chunk_(chunk) {}

    std::size_t Read(char*, std::size_t) override { return 0; }
    std::size_t Write(const char* data, std::size_t size) override {
        std::size_t written = std::min(size, chunk_);
        buffer_.append(data, written);
        return written;
    }
    void Close() override { closed_ = true; }
    bool IsEof() const override { return closed_; }

    const std::string& Buffer() const { return buffer_; }

private:
    std::size_t chunk_;
    std::string buffer_;
    bool closed_ = false;
};

class StalledWritePipe : public mcp::detail::PipeHandle {
public:
    std::size_t Read(char*, std::size_t) override { return 0; }
    std::size_t Write(const char*, std::size_t) override { return 0; }
    void Close() override {}
    bool IsEof() const override { return false; }
};

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t pos = text.find('\n', start);
        if (pos == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return lines;
}

} // namespace

TEST(TransportTest, PipeReadSeparatesTimeoutEofAndHardError) {
    TestPipePair pair;
    CreateTestPipePair(pair, false);
    ASSERT_NE(pair.pipe, nullptr);

    char buffer[64];
    EXPECT_EQ(pair.pipe->Read(buffer, sizeof(buffer)), std::size_t{0});
    EXPECT_FALSE(pair.pipe->IsEof());

    pair.ClosePeer();
    std::size_t total = 0;
    for (int attempt = 0; attempt < 50 && !pair.pipe->IsEof(); ++attempt) {
        total += pair.pipe->Read(buffer, sizeof(buffer));
    }
    EXPECT_EQ(total, std::size_t{0});
    EXPECT_TRUE(pair.pipe->IsEof());

    auto broken = CreateBrokenPipe();
    ASSERT_NE(broken, nullptr);
    EXPECT_THROW(broken->Read(buffer, sizeof(buffer)), McpError);
    EXPECT_TRUE(broken->IsEof());
}

TEST(TransportTest, ConcurrentPipeWritesDeliverWholeFrames) {
    TestPipePair pair;
    CreateTestPipePair(pair, true);
    ASSERT_NE(pair.pipe, nullptr);

    const std::string first = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"first\"}\n";
    const std::string second = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"second\"}\n";

    std::thread writer_a([&]() {
        pair.pipe->Write(first.data(), first.size());
    });
    std::thread writer_b([&]() {
        pair.pipe->Write(second.data(), second.size());
    });
    writer_a.join();
    writer_b.join();

    std::string received;
    char buffer[512];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.size() < first.size() + second.size() &&
           std::chrono::steady_clock::now() < deadline) {
        std::size_t n = pair.ReadPeer(buffer, sizeof(buffer));
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        received.append(buffer, n);
    }

    auto lines = SplitLines(received);
    ASSERT_EQ(lines.size(), std::size_t{2});
    EXPECT_TRUE((lines[0] == first.substr(0, first.size() - 1) &&
                 lines[1] == second.substr(0, second.size() - 1)) ||
                (lines[0] == second.substr(0, second.size() - 1) &&
                 lines[1] == first.substr(0, first.size() - 1)));
    EXPECT_EQ(received.size(), first.size() + second.size());
}

TEST(TransportTest, WritePathLoopsUntilWholeMessageIsWritten) {
    PartialWritePipe pipe(7);
    const std::string payload(1000, 'x');
    EXPECT_TRUE(mcp::detail::WriteWholeMessage(pipe, payload.data(), payload.size()));
    EXPECT_EQ(pipe.Buffer().size(), payload.size());
    EXPECT_EQ(pipe.Buffer(), payload);

    StalledWritePipe stalled;
    EXPECT_FALSE(mcp::detail::WriteWholeMessage(stalled, payload.data(), payload.size()));
}

TEST(TransportTest, StdioClientDisconnectsAndReportsWriteFailure) {
    StdioClientTransportOptions options;
#ifdef _WIN32
    options.command = "cmd.exe";
    options.arguments = {"/c", "exit", "0"};
#else
    options.command = "sh";
    options.arguments = {"-c", "exit 0"};
#endif
    StdioClientTransport transport(options);
    auto session = transport.Connect();
    ASSERT_NE(session, nullptr);
    auto* base = dynamic_cast<TransportBase*>(session.get());
    ASSERT_NE(base, nullptr);

    std::string error;
    base->SetOnError([&error](std::string_view message) { error = std::string(message); });

    JsonRpcRequest request;
    request.id = int64_t(1);
    request.method = "ping";

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (error.empty() && std::chrono::steady_clock::now() < deadline) {
        session->SendMessageAsync(JsonRpcMessage{request});
        if (error.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_FALSE(error.empty());
    EXPECT_EQ(static_cast<int>(base->GetState()), static_cast<int>(TransportState::Disconnected));
}

namespace {

std::filesystem::path StdioHelperPath() {
    static const std::filesystem::path path = MCP_STDIO_TEST_HELPER_PATH;
    return path;
}

std::string PathToUtf8(const std::filesystem::path& path) {
    auto encoded = path.u8string();
    return std::string(encoded.begin(), encoded.end());
}

std::filesystem::path MakeStdioTempPath(const std::string& name) {
    static std::atomic<unsigned long long> counter{0};
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() /
        ("mcp_stdio_test_" + std::to_string(stamp) + "_" +
         std::to_string(counter.fetch_add(1)) + "_" + name);
    std::filesystem::remove(path);
    return path;
}

struct StdioTempFile {
    std::filesystem::path path = MakeStdioTempPath("artifact");
    ~StdioTempFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

std::string ReadStdioFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
    return content;
}

std::string WaitForStdioFile(const std::filesystem::path& path,
                             const std::string& needle, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    std::string content;
    while (std::chrono::steady_clock::now() < deadline) {
        content = ReadStdioFile(path);
        if (content.find(needle) != std::string::npos) return content;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return ReadStdioFile(path);
}

bool StdioProcessAlive(int64_t pid) {
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                 FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) return false;
    DWORD result = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return result == WAIT_TIMEOUT;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

bool WaitForStdioProcessGone(int64_t pid, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!StdioProcessAlive(pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return !StdioProcessAlive(pid);
}

StdioClientTransportOptions StdioHelperOptions(
    const std::vector<std::string>& arguments) {
    StdioClientTransportOptions options;
    options.command = PathToUtf8(StdioHelperPath());
    options.arguments = arguments;
    return options;
}

} // namespace

TEST(TransportTest, StdioStderrCaptured) {
    StdioTempFile stderr_file;
    auto options = StdioHelperOptions({"--stderr-line", "stdio-stderr-line"});
    options.stderr_path = PathToUtf8(stderr_file.path);
    StdioClientTransport transport(options);
    auto session = transport.Connect();
    ASSERT_NE(session, nullptr);

    const std::string content = WaitForStdioFile(
        stderr_file.path, "stdio-stderr-line", 5000);
    EXPECT_NE(content.find("stdio-stderr-line"), std::string::npos) << content;
    session->Close();
}

TEST(TransportTest, StdioStderrCallbackReceivesOutput) {
    std::mutex mutex;
    std::string captured;
    auto options = StdioHelperOptions({"--stderr-line", "stdio-callback-line"});
    options.on_stderr = [&mutex, &captured](std::string_view chunk) {
        std::lock_guard<std::mutex> lock(mutex);
        captured.append(chunk);
    };
    StdioClientTransport transport(options);
    auto session = transport.Connect();
    ASSERT_NE(session, nullptr);

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    bool received = false;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            received = captured.find("stdio-callback-line") != std::string::npos;
        }
        if (received) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(received);
    session->Close();
}

TEST(TransportTest, StdioPidAvailable) {
    auto options = StdioHelperOptions({"--sleep", "3000"});
    StdioClientTransport transport(options);
    EXPECT_FALSE(transport.Pid().has_value());

    auto session = transport.Connect();
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(transport.Pid().has_value());
    EXPECT_GT(*transport.Pid(), 0);
    session->Close();
}

TEST(TransportTest, StdioProcessTreeTerminated) {
    StdioTempFile pid_file;
    auto options = StdioHelperOptions(
        {"--spawn-grandchild", PathToUtf8(pid_file.path), "30000"});
    options.terminate_process_tree = true;
    StdioClientTransport transport(options);
    auto session = transport.Connect();
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(transport.Pid().has_value());

    const std::string pid_text = WaitForStdioFile(pid_file.path, "\n", 10000);
    ASSERT_FALSE(pid_text.empty()) << "grandchild pid was not reported";
    const int64_t grandchild_pid = std::stoll(pid_text);
    ASSERT_GT(grandchild_pid, 0);
    EXPECT_TRUE(StdioProcessAlive(grandchild_pid));

    session->Close();
    EXPECT_FALSE(StdioProcessAlive(grandchild_pid));
    EXPECT_TRUE(WaitForStdioProcessGone(grandchild_pid, 5000));
}

TEST(TransportTest, StdioConcurrentConnects) {
    constexpr int kConnectThreads = 4;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kConnectThreads);
    for (int index = 0; index < kConnectThreads; ++index) {
        threads.emplace_back([&failures]() {
            auto options = StdioHelperOptions({"--sleep", "3000"});
            try {
                StdioClientTransport transport(options);
                auto session = transport.Connect();
                if (!transport.Pid().has_value()) ++failures;
                session->Close();
            } catch (...) {
                ++failures;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(failures.load(), 0);
}

#ifndef _WIN32
TEST(TransportTest, StdioShutdownTimeoutHonored) {
    auto options = StdioHelperOptions({"--ignore-sigterm", "30000"});
    options.shutdown_timeout = std::chrono::milliseconds(300);
    StdioClientTransport transport(options);
    auto session = transport.Connect();
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(transport.Pid().has_value());
    const int64_t pid = *transport.Pid();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const auto start = std::chrono::steady_clock::now();
    session->Close();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(elapsed, 1500);
    EXPECT_TRUE(WaitForStdioProcessGone(pid, 5000));
}

TEST(TransportTest, StdioProcessReapedWithoutZombie) {
    auto options = StdioHelperOptions({"--sleep", "30000"});
    StdioClientTransport transport(options);
    auto session = transport.Connect();
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(transport.Pid().has_value());
    const pid_t pid = static_cast<pid_t>(*transport.Pid());

    session->Close();
    int status = 0;
    errno = 0;
    EXPECT_EQ(waitpid(pid, &status, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
}
#endif

#ifdef MCP_HAVE_OPENSSL

namespace {

X509* LoadTestCertificate() {
    BIO* bio = BIO_new_mem_buf(kTestCertificatePem.data(),
        static_cast<int>(kTestCertificatePem.size()));
    if (bio == nullptr) return nullptr;
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return cert;
}

EVP_PKEY* LoadTestPrivateKey() {
    BIO* bio = BIO_new_mem_buf(kTestPrivateKeyPem.data(),
        static_cast<int>(kTestPrivateKeyPem.size()));
    if (bio == nullptr) return nullptr;
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return key;
}

struct TlsStubServer {
    uint16_t port = 0;
    std::promise<void> listening;
    std::atomic<bool> listening_ok{false};
    std::atomic<bool> stop{false};
};

void TlsStubServe(TlsStubServer* state, const std::string& payload) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    EVP_PKEY* key = LoadTestPrivateKey();
    X509* cert = LoadTestCertificate();
    if (ctx == nullptr || key == nullptr || cert == nullptr) {
        if (cert != nullptr) X509_free(cert);
        if (key != nullptr) EVP_PKEY_free(key);
        if (ctx != nullptr) SSL_CTX_free(ctx);
        state->listening_ok.store(false);
        state->listening.set_value();
        return;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_use_certificate(ctx, cert);
    SSL_CTX_use_PrivateKey(ctx, key);

    SseRetryStubSocket listen_s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_s == kSseRetryStubInvalidSocket) {
        state->listening_ok.store(false);
        state->listening.set_value();
        SSL_CTX_free(ctx);
        X509_free(cert);
        EVP_PKEY_free(key);
        return;
    }
    int reuse = 1;
    setsockopt(listen_s, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(state->port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = bind(listen_s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
              listen(listen_s, 4) == 0;
    state->listening_ok.store(ok);
    state->listening.set_value();

    if (ok) {
        while (!state->stop.load()) {
            if (!SseRetryStubWaitReadable(listen_s, 100)) continue;
            sockaddr_in peer{};
#ifdef _WIN32
            int peer_len = sizeof(peer);
#else
            socklen_t peer_len = sizeof(peer);
#endif
            SseRetryStubSocket conn = accept(listen_s,
                reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (conn == kSseRetryStubInvalidSocket) continue;
            SSL* ssl = SSL_new(ctx);
            if (ssl != nullptr) {
                SSL_set_fd(ssl, static_cast<int>(conn));
                if (SSL_accept(ssl) == 1)
                    SSL_write(ssl, payload.data(), static_cast<int>(payload.size()));
                SSL_free(ssl);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            SseRetryStubCloseSocket(conn);
            break;
        }
    }

    SseRetryStubCloseSocket(listen_s);
    SSL_CTX_free(ctx);
    X509_free(cert);
    EVP_PKEY_free(key);
}

} // namespace

TEST(TlsSocketTest, HandshakeAndReadFirstFrame) {
    uint16_t port = PickFreePort(static_cast<uint16_t>(kTestBasePort + 260));
    TlsStubServer state;
    state.port = port;
    std::thread server([&state]() { TlsStubServe(&state, "hello-frame\n"); });

    bool listening = state.listening.get_future().wait_for(std::chrono::seconds(10)) ==
                     std::future_status::ready;
    ASSERT_TRUE(listening);
    ASSERT_TRUE(state.listening_ok.load());

    std::string received;
    std::string failure;
    try {
        detail::net::TlsSocket client(false);
        client.Connect("127.0.0.1", port, std::chrono::seconds(15));
        char buffer[64];
        std::size_t n = client.Read(buffer, sizeof(buffer), std::chrono::seconds(15));
        received.assign(buffer, n);
        client.Close();
    } catch (const std::exception& e) {
        failure = e.what();
    }
    state.stop.store(true);
    server.join();

    ASSERT_TRUE(failure.empty()) << failure;
    EXPECT_EQ(received, "hello-frame\n");
}

#endif