#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#endif

#include <transport/detail/net/TcpSocket.hpp>
#include <transport/detail/net/HttpClient.hpp>
#include <mcp/detail/HttpFraming.hpp>
#include <mcp/detail/SseEventParser.hpp>
#include <mcp/McpError.hpp>
#include "TestServerUtil.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <csignal>
#endif

#ifndef _WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace mcp;
namespace net = mcp::detail::net;

namespace {

#ifdef _WIN32
void EnsureWinsockOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0)
            throw std::runtime_error("TcpTestServer: WSAStartup failed");
    });
}
#endif

void CloseFd(int fd) {
#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(fd);
#endif
}

std::string RecvLine(int fd) {
    std::string line;
    for (;;) {
        char c = 0;
#ifdef _WIN32
        int n = ::recv(static_cast<SOCKET>(fd), &c, 1, 0);
#else
        ssize_t n = ::recv(fd, &c, 1, 0);
#endif
        if (n <= 0) break;
        line.push_back(c);
        if (c == '\n') break;
    }
    return line;
}

std::string RecvN(int fd, std::size_t n) {
    std::string buf(n, '\0');
    std::size_t total = 0;
    while (total < n) {
#ifdef _WIN32
        int r = ::recv(static_cast<SOCKET>(fd), &buf[total], static_cast<int>(n - total), 0);
#else
        ssize_t r = ::recv(fd, &buf[total], n - total, 0);
#endif
        if (r <= 0) break;
        total += static_cast<std::size_t>(r);
    }
    buf.resize(total);
    return buf;
}

#ifdef __APPLE__
namespace {
struct TestSigpipeGuard {
    TestSigpipeGuard() { std::signal(SIGPIPE, SIG_IGN); }
};
const TestSigpipeGuard kTestSigpipeGuard;
}
#endif

void SendRaw(int fd, std::string_view data) {
    while (!data.empty()) {
#ifdef _WIN32
        int s = ::send(static_cast<SOCKET>(fd), data.data(), static_cast<int>(data.size()), 0);
#elif defined(__APPLE__)
        ssize_t s = ::send(fd, data.data(), data.size(), 0);
#else
        ssize_t s = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
#endif
        if (s <= 0) return;
        data.remove_prefix(static_cast<std::size_t>(s));
    }
}

class TcpTestServer {
public:
    explicit TcpTestServer(std::function<void(int)> handler)
        : handler_(std::move(handler)) {
#ifdef _WIN32
        EnsureWinsockOnce();
        SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == INVALID_SOCKET)
            throw std::runtime_error("TcpTestServer: socket failed");
#else
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            throw std::runtime_error("TcpTestServer: socket failed");
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
#ifdef _WIN32
        int len = sizeof(addr);
#else
        socklen_t len = sizeof(addr);
#endif
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            CloseFd(static_cast<int>(fd));
            throw std::runtime_error("TcpTestServer: bind failed");
        }
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            CloseFd(static_cast<int>(fd));
            throw std::runtime_error("TcpTestServer: getsockname failed");
        }
        if (::listen(fd, 16) != 0) {
            CloseFd(static_cast<int>(fd));
            throw std::runtime_error("TcpTestServer: listen failed");
        }
        listen_fd_ = static_cast<int>(fd);
        port_ = ntohs(addr.sin_port);
        accept_thread_ = std::thread([this] { AcceptLoop(); });
    }

    ~TcpTestServer() {
        stop_.store(true);
#ifdef _WIN32
        ::shutdown(static_cast<SOCKET>(listen_fd_), SD_BOTH);
#else
        ::shutdown(listen_fd_, SHUT_RDWR);
#endif
        CloseFd(listen_fd_);
        std::vector<int> conns;
        {
            std::lock_guard<std::mutex> lk(conn_mutex_);
            conns = conn_fds_;
        }
        for (int fd : conns) {
#ifdef _WIN32
            ::shutdown(static_cast<SOCKET>(fd), SD_BOTH);
#else
            ::shutdown(fd, SHUT_RDWR);
#endif
            CloseFd(fd);
        }
        if (accept_thread_.joinable()) accept_thread_.join();
        for (std::thread& t : conn_threads_) {
            if (t.joinable()) t.join();
        }
    }

    TcpTestServer(const TcpTestServer&) = delete;
    TcpTestServer& operator=(const TcpTestServer&) = delete;

    int Port() const { return port_; }

    std::atomic<int> accept_count{0};

private:
    void AcceptLoop() {
        for (;;) {
#ifdef _WIN32
            WSAPOLLFD pfd;
            pfd.fd = static_cast<SOCKET>(listen_fd_);
            pfd.events = POLLIN;
            pfd.revents = 0;
            int prc;
            do {
                prc = ::WSAPoll(&pfd, 1, 100);
            } while (prc == SOCKET_ERROR && WSAGetLastError() == WSAEINTR);
            if (prc <= 0) {
                if (stop_.load()) break;
                continue;
            }
            SOCKET c = ::accept(static_cast<SOCKET>(listen_fd_), nullptr, nullptr);
            if (c == INVALID_SOCKET) {
                if (stop_.load()) break;
                continue;
            }
            int client_fd = static_cast<int>(c);
#else
            struct pollfd pfd;
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int prc;
            do {
                prc = ::poll(&pfd, 1, 100);
            } while (prc < 0 && errno == EINTR);
            if (prc <= 0) {
                if (stop_.load()) break;
                continue;
            }
            int client_fd = ::accept(listen_fd_, nullptr, nullptr);
            if (client_fd < 0) {
                if (stop_.load()) break;
                continue;
            }
#endif
            accept_count.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(conn_mutex_);
                conn_fds_.push_back(client_fd);
            }
            conn_threads_.emplace_back([this, client_fd] {
                try {
                    handler_(client_fd);
                } catch (...) {
                }
                CloseFd(client_fd);
            });
        }
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread accept_thread_;
    std::vector<std::thread> conn_threads_;
    std::mutex conn_mutex_;
    std::vector<int> conn_fds_;
    std::function<void(int)> handler_;
};

} // anonymous namespace

TEST(TcpSocketTest, ConnectEchoRoundTrip) {
    TcpTestServer server([](int fd) {
        std::string msg = RecvN(fd, 4);
        SendRaw(fd, msg);
    });
    net::TcpSocket client;
    client.Connect("127.0.0.1", server.Port(), std::chrono::seconds(5));
    client.Write("ping", 4);
    char buf[4] = {};
    std::size_t n = client.Read(buf, 4, std::chrono::seconds(5));
    EXPECT_EQ(n, 4);
    EXPECT_EQ(std::string(buf, n), "ping");
}

TEST(TcpSocketTest, ReadTimeoutReturnsZero) {
    TcpTestServer server([](int) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });
    net::TcpSocket client;
    client.Connect("127.0.0.1", server.Port(), std::chrono::seconds(5));
    char buf[4] = {};
    std::size_t n = client.Read(buf, 4, std::chrono::milliseconds(50));
    EXPECT_EQ(n, 0);
    EXPECT_FALSE(client.IsEof());
}

TEST(TcpSocketTest, ReadDetectsEof) {
    TcpTestServer server([](int) {});
    net::TcpSocket client;
    client.Connect("127.0.0.1", server.Port(), std::chrono::seconds(5));
    char buf[4] = {};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!client.IsEof() && std::chrono::steady_clock::now() < deadline)
        client.Read(buf, 4);
    EXPECT_TRUE(client.IsEof());
}

TEST(TcpSocketTest, CloseInterruptsBlockingRead) {
    TcpTestServer server([](int) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
    });
    net::TcpSocket client;
    client.Connect("127.0.0.1", server.Port(), std::chrono::seconds(5));
    char buf[4] = {};
    auto start = std::chrono::steady_clock::now();
    std::promise<void> read_started;
    std::thread reader([&] {
        read_started.set_value();
        try {
            std::size_t n = client.Read(buf, 4, std::chrono::seconds(5));
            EXPECT_EQ(n, std::size_t{0});
            EXPECT_TRUE(client.IsEof());
        } catch (const mcp::McpError&) {
        }
    });
    ASSERT_EQ(read_started.get_future().wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    client.Close();
    reader.join();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
}

TEST(TcpSocketTest, ConnectRefusedThrows) {
    uint16_t port = PickFreePort(20000);
    net::TcpSocket client;
    EXPECT_THROW(client.Connect("127.0.0.1", port, std::chrono::seconds(2)), mcp::McpError);
}

TEST(HttpClientTest, GetParsesStatusAndBody) {
    TcpTestServer server([](int fd) {
        RecvLine(fd);
        for (;;) {
            std::string h = RecvLine(fd);
            if (h == "\r\n" || h == "\n" || h.empty()) break;
        }
        SendRaw(fd, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\n\r\nhello");
    });
    net::HttpClient client;
    net::HttpRequestSpec req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(server.Port()) + "/";
    auto resp = client.Request(req);
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_EQ(resp.headers.at("content-type"), "text/plain");
    EXPECT_EQ(resp.body, "hello");
}

TEST(HttpClientTest, PostSendsBodyAndHeaders) {
    bool request_line_ok = false;
    bool cl_ok = false;
    bool custom_ok = false;
    std::string posted_body;
    TcpTestServer server([&](int fd) {
        request_line_ok = RecvLine(fd).rfind("POST /x HTTP/1.1", 0) == 0;
        for (;;) {
            std::string h = RecvLine(fd);
            if (h == "\r\n" || h == "\n" || h.empty()) break;
            if (h.find("Content-Length: 4") != std::string::npos) cl_ok = true;
            if (h.find("X-Custom: abc") != std::string::npos) custom_ok = true;
        }
        posted_body = RecvN(fd, 4);
        SendRaw(fd, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    });
    net::HttpClient client;
    net::HttpRequestSpec req;
    req.method = "POST";
    req.url = "http://127.0.0.1:" + std::to_string(server.Port()) + "/x";
    req.body = "test";
    req.headers = {{"X-Custom", "abc"}};
    auto resp = client.Request(req);
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_TRUE(request_line_ok);
    EXPECT_TRUE(cl_ok);
    EXPECT_TRUE(custom_ok);
    EXPECT_EQ(posted_body, "test");
}

TEST(HttpClientTest, ChunkedDecoding) {
    TcpTestServer server([](int fd) {
        RecvLine(fd);
        for (;;) {
            std::string h = RecvLine(fd);
            if (h == "\r\n" || h == "\n" || h.empty()) break;
        }
        SendRaw(fd, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                    "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
    });
    net::HttpClient client;
    net::HttpRequestSpec req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(server.Port()) + "/";
    auto resp = client.Request(req);
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_EQ(resp.body, "hello world");
}

TEST(HttpClientTest, StreamingBodyCallback) {
    TcpTestServer server([](int fd) {
        RecvLine(fd);
        for (;;) {
            std::string h = RecvLine(fd);
            if (h == "\r\n" || h == "\n" || h.empty()) break;
        }
        SendRaw(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n");
        SendRaw(fd, "data: a\r\n\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        SendRaw(fd, "data: b\r\n\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    });
    std::string captured;
    std::mutex mu;
    std::atomic<int> chunks{0};
    net::HttpClient client;
    net::HttpRequestSpec req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(server.Port()) + "/";
    auto resp = client.Request(req, [&](std::string_view chunk) {
        std::lock_guard<std::mutex> lock(mu);
        captured.append(chunk);
        ++chunks;
    });
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_GE(chunks.load(), 2);
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_NE(captured.find("data: a"), std::string::npos);
}

TEST(HttpClientTest, CloseInterruptsStreaming) {
    TcpTestServer server([](int) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
    });
    net::HttpClient client;
    net::HttpRequestSpec req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(server.Port()) + "/";
    std::thread requester([&] {
        EXPECT_THROW(client.Request(req, [](std::string_view) {}), mcp::McpError);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    client.Close();
    requester.join();
}

TEST(HttpClientTest, MalformedStatusLineThrows) {
    TcpTestServer server([](int fd) {
        SendRaw(fd, "BOGUS\r\n\r\n");
    });
    net::HttpClient client;
    net::HttpRequestSpec req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(server.Port()) + "/";
    EXPECT_THROW(client.Request(req), mcp::McpError);
}

TEST(HttpClientTest, ClAndTeTogetherThrows) {
    TcpTestServer server([](int fd) {
        SendRaw(fd, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                    "Content-Length: 4\r\n\r\n0\r\n\r\n");
    });
    net::HttpClient client;
    net::HttpRequestSpec req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(server.Port()) + "/";
    EXPECT_THROW(client.Request(req), mcp::McpError);
}

TEST(HttpClientTest, HeaderInjectionRejected) {
    uint16_t port = PickFreePort(20000);
    net::HttpClient client;
    net::HttpRequestSpec req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(port) + "/";
    req.headers = {{"X-Custom", "a\r\nb"}};
    EXPECT_THROW(client.Request(req), mcp::McpError);
}

TEST(HttpClientTest, KeepAliveReusesConnection) {
    TcpTestServer server([](int fd) {
        for (int i = 0; i < 2; ++i) {
            RecvLine(fd);
            for (;;) {
                std::string h = RecvLine(fd);
                if (h == "\r\n" || h == "\n" || h.empty()) break;
            }
            SendRaw(fd, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
        }
    });
    net::HttpClient client;
    net::HttpRequestSpec req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(server.Port()) + "/";
    auto r1 = client.Request(req);
    auto r2 = client.Request(req);
    EXPECT_EQ(r1.status_code, 200);
    EXPECT_EQ(r2.status_code, 200);
    EXPECT_EQ(server.accept_count.load(), 1);
}

TEST(HttpClientTest, ConnectionCloseResponseCloses) {
    TcpTestServer server([](int fd) {
        RecvLine(fd);
        for (;;) {
            std::string h = RecvLine(fd);
            if (h == "\r\n" || h == "\n" || h.empty()) break;
        }
        SendRaw(fd, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    });
    net::HttpClient client;
    net::HttpRequestSpec req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(server.Port()) + "/";
    auto r1 = client.Request(req);
    auto r2 = client.Request(req);
    EXPECT_EQ(r1.status_code, 200);
    EXPECT_EQ(r2.status_code, 200);
    EXPECT_EQ(server.accept_count.load(), 2);
}

TEST(HttpClientTest, BodyTooLargeThrows) {
    TcpTestServer server([](int fd) {
        SendRaw(fd, "HTTP/1.1 200 OK\r\nContent-Length: 8388609\r\n\r\n");
    });
    net::HttpClient client;
    net::HttpRequestSpec req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(server.Port()) + "/";
    EXPECT_THROW(client.Request(req), mcp::McpError);
}

TEST(HttpClientTest, ChunkedBodyTooLargeThrows) {
    TcpTestServer server([](int fd) {
        RecvLine(fd);
        for (;;) {
            std::string h = RecvLine(fd);
            if (h == "\r\n" || h == "\n" || h.empty()) break;
        }
        SendRaw(fd, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                    "20\r\n0123456789abcdef0123456789abcdef\r\n"
                    "20\r\n0123456789abcdef0123456789abcdef\r\n0\r\n\r\n");
    });
    net::HttpClient client;
    net::HttpRequestSpec req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(server.Port()) + "/";
    req.max_response_bytes = 32;
    EXPECT_THROW(client.Request(req), mcp::McpError);
}

TEST(HttpFramingTest, CheckedDecimalOverflowBeforeLimit) {
    const auto overflow = mcp::detail::ParseCheckedDecimal(
        "18446744073709551616", (std::numeric_limits<std::size_t>::max)());
    EXPECT_EQ(overflow.error, mcp::detail::HttpNumberError::Overflow);

    const auto far_overflow = mcp::detail::ParseCheckedDecimal(
        std::string(64, '9'), (std::numeric_limits<std::size_t>::max)());
    EXPECT_EQ(far_overflow.error, mcp::detail::HttpNumberError::Overflow);

    const auto limit = mcp::detail::ParseCheckedDecimal("4194305", 4194304);
    EXPECT_EQ(limit.error, mcp::detail::HttpNumberError::LimitExceeded);

    const auto at_limit = mcp::detail::ParseCheckedDecimal("4194304", 4194304);
    EXPECT_EQ(at_limit.error, mcp::detail::HttpNumberError::None);
    EXPECT_EQ(at_limit.value, std::size_t{4194304});

    EXPECT_EQ(mcp::detail::ParseCheckedDecimal("", 100).error,
              mcp::detail::HttpNumberError::Empty);
    EXPECT_EQ(mcp::detail::ParseCheckedDecimal("12x", 100).error,
              mcp::detail::HttpNumberError::InvalidDigit);
}

TEST(HttpFramingTest, CheckedHexRejectsOverflow) {
    const auto overflow = mcp::detail::ParseCheckedHex(
        "FFFFFFFFFFFFFFFFF", (std::numeric_limits<std::size_t>::max)());
    EXPECT_EQ(overflow.error, mcp::detail::HttpNumberError::Overflow);
    const auto valid = mcp::detail::ParseCheckedHex("00000010", 16);
    EXPECT_EQ(valid.error, mcp::detail::HttpNumberError::None);
    EXPECT_EQ(valid.value, std::size_t{16});
}

TEST(HttpClientTest, InvalidAndDuplicateContentLengthRejected) {
    struct Case {
        const char* headers;
    };
    const Case cases[] = {
        {"Content-Length: \r\n"},
        {"Content-Length: 12x\r\n"},
        {"Content-Length: 18446744073709551616\r\n"},
        {"Content-Length: 4\r\nContent-Length: 5\r\n"},
    };
    for (const auto& item : cases) {
        TcpTestServer server([&](int fd) {
            SendRaw(fd, std::string("HTTP/1.1 200 OK\r\n") + item.headers + "\r\n");
        });
        net::HttpClient client;
        net::HttpRequestSpec req;
        req.method = "GET";
        req.url = "http://127.0.0.1:" + std::to_string(server.Port()) + "/";
        EXPECT_THROW(client.Request(req), mcp::McpError);
    }
}

TEST(HttpClientTest, ChunkSizeOverflowRejected) {
    TcpTestServer server([](int fd) {
        RecvLine(fd);
        for (;;) {
            std::string h = RecvLine(fd);
            if (h == "\r\n" || h == "\n" || h.empty()) break;
        }
        SendRaw(fd, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                    "FFFFFFFFFFFFFFFFF\r\n0\r\n\r\n");
    });
    net::HttpClient client;
    net::HttpRequestSpec req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(server.Port()) + "/";
    EXPECT_THROW(client.Request(req), mcp::McpError);
}

namespace {

std::vector<mcp::detail::SseEvent> FeedEveryByte(const std::string& stream,
                                                 std::size_t max_bytes) {
    mcp::detail::SseIncrementalParser parser(max_bytes);
    std::vector<mcp::detail::SseEvent> events;
    for (char c : stream) {
        auto fed = parser.Feed(std::string_view(&c, 1));
        for (auto& evt : fed.events) events.push_back(std::move(evt));
    }
    for (auto& evt : parser.Flush().events) events.push_back(std::move(evt));
    return events;
}

} // namespace

TEST(SseParserTest, LineEndingsAndArbitraryChunksProduceSameEvents) {
    const std::string crlf = "id: 1\r\ndata: first\r\ndata: second\r\n\r\n: ping\r\n\r\n";
    const std::string lf = "id: 1\ndata: first\ndata: second\n\n: ping\n\n";
    const std::string cr = "id: 1\rdata: first\rdata: second\r\r: ping\r\r";
    const std::string mixed = "id: 1\rdata: first\ndata: second\r\n\r: ping\r\n\r";

    auto expected = FeedEveryByte(crlf, 1024);
    ASSERT_EQ(expected.size(), std::size_t{1});
    EXPECT_EQ(expected[0].id, "1");
    EXPECT_EQ(expected[0].data, "first\nsecond");

    for (const auto& stream : {lf, cr, mixed}) {
        auto whole = mcp::detail::SseIncrementalParser(1024).Feed(stream).events;
        auto split = FeedEveryByte(stream, 1024);
        ASSERT_EQ(whole.size(), std::size_t{1});
        ASSERT_EQ(split.size(), std::size_t{1});
        EXPECT_EQ(split[0].id, expected[0].id);
        EXPECT_EQ(split[0].data, expected[0].data);
    }
}

TEST(SseParserTest, TrailingCrRetainedAcrossChunks) {
    mcp::detail::SseIncrementalParser parser(1024);
    auto first = parser.Feed("data: abc\r");
    EXPECT_TRUE(first.events.empty());
    auto second = parser.Feed("\nid: 9\r");
    EXPECT_TRUE(second.events.empty());
    auto third = parser.Feed("\n\n");
    ASSERT_EQ(third.events.size(), std::size_t{1});
    EXPECT_EQ(third.events[0].data, "abc");
    EXPECT_EQ(third.events[0].id, "9");
}

TEST(SseParserTest, CommentsAndAllFieldsFollowOneRule) {
    mcp::detail::SseIncrementalParser parser(4096);
    auto fed = parser.Feed(": comment\n"
                           "event: endpoint\n"
                           "id: e-1\n"
                           "retry: 250\n"
                           "data: one\n"
                           "data: two\n"
                           ": another\n"
                           "data:\n"
                           "\n");
    ASSERT_EQ(fed.events.size(), std::size_t{1});
    EXPECT_EQ(fed.events[0].event_type, "endpoint");
    EXPECT_EQ(fed.events[0].id, "e-1");
    ASSERT_TRUE(fed.events[0].retry_ms.has_value());
    EXPECT_EQ(*fed.events[0].retry_ms, uint64_t{250});
    EXPECT_EQ(fed.events[0].data, "one\ntwo\n");
}

TEST(SseParserTest, EventSizeLimitEnforced) {
    mcp::detail::SseIncrementalParser parser(8);
    auto fed = parser.Feed("data: 1234567890\n\n");
    EXPECT_TRUE(fed.limit_exceeded);
    EXPECT_TRUE(fed.events.empty());
}
