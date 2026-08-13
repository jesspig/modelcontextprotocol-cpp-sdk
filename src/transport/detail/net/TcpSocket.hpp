#pragma once

// TcpSocket.hpp — blocking TCP socket with poll-based timeouts

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#endif

namespace mcp { namespace detail { namespace net {

class TlsSocket;

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket() { Close(); }
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    static TcpSocket FromFd(int fd);
    void Connect(std::string_view host, uint16_t port, std::chrono::milliseconds timeout);
    std::size_t Read(void* buf, std::size_t len, std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
    void Write(const void* buf, std::size_t len, std::chrono::milliseconds timeout = std::chrono::milliseconds(30000));
    void Close();
    bool IsEof() const { return closed_ || eof_; }
    bool IsConnected() const { return !closed_ && !eof_ && fd_ != kInvalidFd; }

    int NativeHandle() const { return static_cast<int>(fd_); }

private:
    friend class TlsSocket;
    void WaitWriteable(std::chrono::milliseconds timeout);
    void WaitReadable(std::chrono::milliseconds timeout);

#ifdef _WIN32
    static constexpr SOCKET kInvalidFd = INVALID_SOCKET;
    SOCKET fd_ = INVALID_SOCKET;
#else
    static constexpr int kInvalidFd = -1;
    int fd_ = -1;
#endif
    bool eof_ = false;
    bool closed_ = false;
};

}}} // namespace mcp::detail::net
