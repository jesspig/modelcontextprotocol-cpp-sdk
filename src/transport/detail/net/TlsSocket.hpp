#pragma once

// TlsSocket.hpp — TLS socket wrapping TcpSocket

#include <transport/detail/net/TcpSocket.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Forward declarations of OpenSSL types so this header stays free of
// OpenSSL dependencies (real definitions come from OpenSSL headers).
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

namespace mcp { namespace detail { namespace net {

class TlsSocket {
public:
    explicit TlsSocket(bool verify_peer);
    ~TlsSocket();
    TlsSocket(const TlsSocket&) = delete;
    TlsSocket& operator=(const TlsSocket&) = delete;

    void Connect(std::string_view host, uint16_t port, std::chrono::milliseconds timeout);
    std::size_t Read(void* buf, std::size_t len, std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
    void Write(const void* buf, std::size_t len, std::chrono::milliseconds timeout = std::chrono::milliseconds(30000));
    void Close();
    bool IsEof() const;
    bool IsConnected() const;

private:
    TcpSocket tcp_;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    bool verify_peer_;
    bool eof_ = false;
};

}}} // namespace mcp::detail::net
