// WebSocketClientTests.cpp — WebSocket 客户端单元测试

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#endif

#include <transport/detail/net/WebSocketClient.hpp>
#include <transport/detail/net/Sha1.hpp>
#include "TestServerUtil.hpp"

#include <mcp/test/McpTest.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
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

constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

#ifdef _WIN32
void EnsureWinsockOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0)
            throw std::runtime_error("WsEchoServer: WSAStartup failed");
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

bool ReadHandshakeRequest(int fd, std::string& out_key) {
    for (;;) {
        std::string line = RecvLine(fd);
        if (line == "\r\n" || line == "\n" || line.empty()) break;
        constexpr std::string_view kKeyPrefix = "Sec-WebSocket-Key:";
        if (line.compare(0, kKeyPrefix.size(), kKeyPrefix) == 0) {
            std::size_t start = kKeyPrefix.size();
            while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
            std::size_t end = line.find('\r', start);
            out_key = line.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }
    }
    return !out_key.empty();
}

void SendHandshakeResponse(int fd, std::string_view key) {
    std::string accept =
        net::Base64Encode(net::Sha1Raw(std::string(key) + std::string(kWebSocketGuid)));
    std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    SendRaw(fd, response);
}

void SendServerFrame(int fd, int opcode, std::string_view payload, bool fin = true) {
    std::string frame;
    frame.push_back(static_cast<char>((fin ? 0x80 : 0x00) | opcode));
    std::size_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(len));
    } else if (len <= 0xFFFF) {
        frame.push_back(126);
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
    }
    frame.append(payload);
    SendRaw(fd, frame);
}

bool ReadClientFrame(int fd, std::string& payload, int& opcode) {
    std::string header = RecvN(fd, 2);
    if (header.size() < 2) return false;
    uint8_t b0 = static_cast<uint8_t>(header[0]);
    uint8_t b1 = static_cast<uint8_t>(header[1]);
    opcode = b0 & 0x0F;
    uint64_t len = b1 & 0x7F;
    if (len == 126) {
        std::string ext = RecvN(fd, 2);
        if (ext.size() < 2) return false;
        len = (static_cast<uint64_t>(static_cast<uint8_t>(ext[0])) << 8) |
              static_cast<uint8_t>(ext[1]);
    } else if (len == 127) {
        std::string ext = RecvN(fd, 8);
        if (ext.size() < 8) return false;
        len = 0;
        for (int i = 0; i < 8; ++i)
            len = (len << 8) | static_cast<uint8_t>(ext[i]);
    }
    uint8_t mask[4] = {};
    if ((b1 & 0x80) != 0) {
        std::string mask_bytes = RecvN(fd, 4);
        if (mask_bytes.size() < 4) return false;
        std::memcpy(mask, mask_bytes.data(), 4);
    }
    std::string body = RecvN(fd, static_cast<std::size_t>(len));
    if (body.size() < len) return false;
    for (std::size_t i = 0; i < body.size(); ++i)
        body[i] = static_cast<char>(body[i] ^ mask[i % 4]);
    payload = std::move(body);
    return true;
}

template <typename Predicate>
bool WaitFor(Predicate pred, int timeout_ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

class WsEchoServer {
public:
    explicit WsEchoServer(std::function<void(int)> handler)
        : handler_(std::move(handler)) {
#ifdef _WIN32
        EnsureWinsockOnce();
        SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == INVALID_SOCKET)
            throw std::runtime_error("WsEchoServer: socket failed");
#else
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            throw std::runtime_error("WsEchoServer: socket failed");
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
            throw std::runtime_error("WsEchoServer: bind failed");
        }
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            CloseFd(static_cast<int>(fd));
            throw std::runtime_error("WsEchoServer: getsockname failed");
        }
        if (::listen(fd, 16) != 0) {
            CloseFd(static_cast<int>(fd));
            throw std::runtime_error("WsEchoServer: listen failed");
        }
        listen_fd_ = static_cast<int>(fd);
        port_ = ntohs(addr.sin_port);
        accept_thread_ = std::thread([this] { AcceptLoop(); });
    }

    ~WsEchoServer() {
        stop_.store(true);
        CloseFd(listen_fd_);
        if (accept_thread_.joinable()) accept_thread_.join();
        for (std::thread& t : conn_threads_) {
            if (t.joinable()) t.join();
        }
    }

    WsEchoServer(const WsEchoServer&) = delete;
    WsEchoServer& operator=(const WsEchoServer&) = delete;

    int Port() const { return port_; }

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

TEST(WebSocketClientTest, Sha1Vectors) {
    EXPECT_EQ(net::Sha1Hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
    EXPECT_EQ(net::Sha1Hex(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST(WebSocketClientTest, Base64Vectors) {
    EXPECT_EQ(net::Base64Encode("hello"), "aGVsbG8=");
    EXPECT_EQ(net::Base64Encode("f"), "Zg==");
    EXPECT_EQ(net::Base64Encode(""), "");
}

TEST(WebSocketClientTest, Rfc6455HandshakeVector) {
    std::string accept =
        net::Base64Encode(net::Sha1Raw("dGhlIHNhbXBsZSBub25jZQ==" + std::string(kWebSocketGuid)));
    EXPECT_EQ(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WebSocketClientTest, EchoRoundTrip) {
    std::atomic<bool> handshake_done{false};
    WsEchoServer server([&](int fd) {
        std::string key;
        if (!ReadHandshakeRequest(fd, key)) return;
        SendHandshakeResponse(fd, key);
        handshake_done.store(true);
        for (;;) {
            std::string payload;
            int opcode = 0;
            if (!ReadClientFrame(fd, payload, opcode)) break;
            if (opcode == 0x1)
                SendServerFrame(fd, 0x1, payload);
        }
    });

    std::promise<std::string> echo;
    net::WebSocketClient client;
    client.SetCallbacks(
        [&](std::string_view text) {
            try {
                echo.set_value(std::string(text));
            } catch (...) {
            }
        },
        []() {},
        [](std::string_view) {});
    client.Open("ws://127.0.0.1:" + std::to_string(server.Port()) + "/mcp");

    ASSERT_TRUE(WaitFor([&] { return handshake_done.load(); }));
    client.Send("hello");
    auto future = echo.get_future();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_EQ(future.get(), "hello");
    client.Close();
}

TEST(WebSocketClientTest, FragmentedMessageAggregated) {
    std::atomic<bool> handshake_done{false};
    WsEchoServer server([&](int fd) {
        std::string key;
        if (!ReadHandshakeRequest(fd, key)) return;
        SendHandshakeResponse(fd, key);
        handshake_done.store(true);
        for (;;) {
            std::string payload;
            int opcode = 0;
            if (!ReadClientFrame(fd, payload, opcode)) break;
            if (opcode == 0x1) {
                SendServerFrame(fd, 0x1, "hel", false);
                SendServerFrame(fd, 0x0, "lo", true);
            }
        }
    });

    std::promise<std::string> message;
    net::WebSocketClient client;
    client.SetCallbacks(
        [&](std::string_view text) {
            try {
                message.set_value(std::string(text));
            } catch (...) {
            }
        },
        []() {},
        [](std::string_view) {});
    client.Open("ws://127.0.0.1:" + std::to_string(server.Port()) + "/mcp");

    ASSERT_TRUE(WaitFor([&] { return handshake_done.load(); }));
    client.Send("hello");
    auto future = message.get_future();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_EQ(future.get(), "hello");
    client.Close();
}

TEST(WebSocketClientTest, CloseHandshake) {
    std::atomic<bool> handshake_done{false};
    std::atomic<bool> server_got_close{false};
    WsEchoServer server([&](int fd) {
        std::string key;
        if (!ReadHandshakeRequest(fd, key)) return;
        SendHandshakeResponse(fd, key);
        handshake_done.store(true);
        for (;;) {
            std::string payload;
            int opcode = 0;
            if (!ReadClientFrame(fd, payload, opcode)) break;
            if (opcode == 0x8) {
                server_got_close.store(true);
                SendServerFrame(fd, 0x8, payload);
                break;
            }
        }
    });

    std::promise<void> closed;
    net::WebSocketClient client;
    client.SetCallbacks(
        [](std::string_view) {},
        [&] { closed.set_value(); },
        [](std::string_view) {});
    client.Open("ws://127.0.0.1:" + std::to_string(server.Port()) + "/mcp");

    ASSERT_TRUE(WaitFor([&] { return handshake_done.load(); }));
    client.Close();
    EXPECT_TRUE(WaitFor([&] { return server_got_close.load(); }));
    EXPECT_EQ(closed.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
}

TEST(WebSocketClientTest, ServerClosesConnection) {
    WsEchoServer server([](int fd) {
        std::string key;
        if (!ReadHandshakeRequest(fd, key)) return;
        SendHandshakeResponse(fd, key);
        SendServerFrame(fd, 0x8, "");
    });

    std::promise<void> closed;
    net::WebSocketClient client;
    client.SetCallbacks(
        [](std::string_view) {},
        [&] { closed.set_value(); },
        [](std::string_view) {});
    client.Open("ws://127.0.0.1:" + std::to_string(server.Port()) + "/mcp");

    EXPECT_EQ(closed.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
    client.Close();
}

TEST(WebSocketClientTest, ConnectionRefusedNotifiesClose) {
    uint16_t port = PickFreePort(30000);
    std::promise<void> error;
    std::promise<void> closed;
    net::WebSocketClient client;
    client.SetCallbacks(
        [](std::string_view) {},
        [&] { closed.set_value(); },
        [&](std::string_view) { error.set_value(); });
    client.Open("ws://127.0.0.1:" + std::to_string(port) + "/mcp", std::chrono::seconds(3));

    EXPECT_EQ(closed.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_EQ(error.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);
}

TEST(WebSocketClientTest, PingPong) {
    std::atomic<bool> handshake_done{false};
    std::atomic<bool> got_pong{false};
    WsEchoServer server([&](int fd) {
        std::string key;
        if (!ReadHandshakeRequest(fd, key)) return;
        SendHandshakeResponse(fd, key);
        handshake_done.store(true);
        SendServerFrame(fd, 0x9, "hi");
        for (;;) {
            std::string payload;
            int opcode = 0;
            if (!ReadClientFrame(fd, payload, opcode)) break;
            if (opcode == 0xA) {
                got_pong.store(true);
                break;
            }
        }
    });

    net::WebSocketClient client;
    client.SetCallbacks(
        [](std::string_view) {},
        []() {},
        [](std::string_view) {});
    client.Open("ws://127.0.0.1:" + std::to_string(server.Port()) + "/mcp");

    ASSERT_TRUE(WaitFor([&] { return handshake_done.load(); }));
    EXPECT_TRUE(WaitFor([&] { return got_pong.load(); }));
    client.Close();
}
