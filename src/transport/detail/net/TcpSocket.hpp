#pragma once

// TcpSocket.hpp — blocking TCP socket with poll-based timeouts

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#endif

namespace mcp { namespace detail { namespace net {

class TlsSocket;

#ifdef _WIN32
using NativeFd = SOCKET;
#else
using NativeFd = int;
#endif

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket() { Close(); }
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    static TcpSocket FromFd(NativeFd fd);
    void Connect(std::string_view host, uint16_t port, std::chrono::milliseconds timeout);
    std::size_t Read(void* buf, std::size_t len, std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
    void Write(const void* buf, std::size_t len, std::chrono::milliseconds timeout = std::chrono::milliseconds(30000));
    void Close();
    bool IsEof() const { return closed_.load() || eof_; }
    bool IsConnected() const { return !closed_.load() && !eof_ && fd_ != kInvalidFd; }

    NativeFd NativeHandle() const { return fd_; }

private:
    friend class TlsSocket;
    bool WaitForEvents(short events, int timeout_ms);
    bool WaitWriteable(std::chrono::milliseconds timeout);
    bool WaitReadable(std::chrono::milliseconds timeout);

    static constexpr int kPollSliceMs = 100;

#ifdef _WIN32
    static constexpr SOCKET kInvalidFd = INVALID_SOCKET;
    SOCKET fd_ = INVALID_SOCKET;
#else
    static constexpr int kInvalidFd = -1;
    int fd_ = -1;
#endif
    bool eof_ = false;
    std::atomic<bool> closed_{false};
};

}}} // namespace mcp::detail::net
