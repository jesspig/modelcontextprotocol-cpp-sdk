// NetStackTests.cpp — 自研网络栈单元测试

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#endif

#include <transport/detail/net/TcpSocket.hpp>
#include <transport/detail/net/HttpClient.hpp>
#include <mcp/McpError.hpp>
#include "TestServerUtil.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
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

void SendRaw(int fd, std::string_view data) {
    while (!data.empty()) {
#ifdef _WIN32
        int s = ::send(static_cast<SOCKET>(fd), data.data(), static_cast<int>(data.size()), 0);
#else
        ssize_t s = ::send(fd, data.data(), data.size(), 0);
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
        CloseFd(listen_fd_);
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
            SOCKET c = ::accept(static_cast<SOCKET>(listen_fd_), nullptr, nullptr);
            if (c == INVALID_SOCKET) {
                if (stop_.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            int client_fd = static_cast<int>(c);
#else
            int client_fd = ::accept(listen_fd_, nullptr, nullptr);
            if (client_fd < 0) {
                if (stop_.load()) break;
                if (errno == EINTR) continue;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
#endif
            accept_count.fetch_add(1, std::memory_order_relaxed);
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
    std::size_t n = client.Read(buf, 4);
    EXPECT_EQ(n, 0);
    EXPECT_TRUE(client.IsEof());
}

TEST(TcpSocketTest, CloseInterruptsBlockingRead) {
    TcpTestServer server([](int) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
    });
    net::TcpSocket client;
    client.Connect("127.0.0.1", server.Port(), std::chrono::seconds(5));
    char buf[4] = {};
    std::thread reader([&] {
        EXPECT_THROW(client.Read(buf, 4, std::chrono::seconds(5)), mcp::McpError);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    client.Close();
    reader.join();
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
        SendRaw(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n");
        SendRaw(fd, "data: a\r\n\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        SendRaw(fd, "data: b\r\n\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
