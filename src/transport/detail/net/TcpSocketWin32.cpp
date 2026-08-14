// TcpSocketWin32.cpp — Windows TCP socket implementation

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <transport/detail/net/TcpSocket.hpp>
#include <mcp/McpError.hpp>
#include <mcp/ErrorCodes.hpp>

#include <windows.h>
#include <mutex>
#include <cstring>
#include <string>

namespace mcp { namespace detail { namespace net {

namespace {

bool EnsureWinsock() {
    // WSACleanup is intentionally never called: Winsock resources are
    // reclaimed by the OS at process exit.
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA data;
        ok = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
    });
    return ok;
}

std::string WinsockErrorText(int error) {
    char buf[256] = {0};
    DWORD written = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, static_cast<DWORD>(error), 0,
                                   buf, static_cast<DWORD>(sizeof(buf)), nullptr);
    if (written == 0) return "error " + std::to_string(error);
    std::string text(buf, written);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' '))
        text.pop_back();
    return text;
}

bool WaitForSocket(SOCKET sock, short events, int timeout_ms) {
    WSAPOLLFD pfd;
    pfd.fd = sock;
    pfd.events = events;
    pfd.revents = 0;
    int rc;
    do {
        rc = ::WSAPoll(&pfd, 1, timeout_ms);
    } while (rc == SOCKET_ERROR && WSAGetLastError() == WSAEINTR);
    return rc > 0;
}

} // anonymous namespace

TcpSocket TcpSocket::FromFd(int fd) {
    TcpSocket s;
    s.fd_ = static_cast<SOCKET>(fd);
    s.closed_ = false;
    s.eof_ = false;
    return s;
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : fd_(other.fd_), eof_(other.eof_), closed_(other.closed_) {
    other.fd_ = INVALID_SOCKET;
    other.closed_ = true;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        eof_ = other.eof_;
        closed_ = other.closed_;
        other.fd_ = INVALID_SOCKET;
        other.closed_ = true;
    }
    return *this;
}

void TcpSocket::Connect(std::string_view host, uint16_t port, std::chrono::milliseconds timeout) {
    Close();
    if (!EnsureWinsock())
        throw McpError(McpErrorCode::ConnectionRefused, "Winsock initialization failed");

    std::string host_str(host);
    std::string port_str = std::to_string(port);

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* results = nullptr;
    int gai = ::getaddrinfo(host_str.c_str(), port_str.c_str(), &hints, &results);
    if (gai != 0)
        throw McpError(McpErrorCode::ConnectionRefused,
                       "DNS resolution failed for " + host_str + ": " + gai_strerrorA(gai));

    auto deadline = std::chrono::steady_clock::now() + timeout;
    int last_error = WSAECONNREFUSED;
    SOCKET connected_sock = INVALID_SOCKET;

    for (struct addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        SOCKET sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == INVALID_SOCKET) continue;

        u_long nonblocking = 1;
        ::ioctlsocket(sock, FIONBIO, &nonblocking);

        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::milliseconds(0)) {
            ::closesocket(sock);
            throw McpError(McpErrorCode::RequestTimeout,
                           "connect timed out for " + host_str + ":" + port_str);
        }

        int rc = ::connect(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        if (rc == 0) {
            connected_sock = sock;
            break;
        }
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
            last_error = error;
            ::closesocket(sock);
            continue;
        }

        if (!WaitForSocket(sock, POLLOUT, static_cast<int>(remaining.count()))) {
            ::closesocket(sock);
            throw McpError(McpErrorCode::RequestTimeout,
                           "connect timed out for " + host_str + ":" + port_str);
        }

        int so_error = 0;
        int so_len = sizeof(so_error);
        ::getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &so_len);
        if (so_error != 0) {
            last_error = so_error;
            ::closesocket(sock);
            continue;
        }
        connected_sock = sock;
        break;
    }

    ::freeaddrinfo(results);

    if (connected_sock == INVALID_SOCKET)
        throw McpError(McpErrorCode::ConnectionRefused,
                       "connect failed for " + host_str + ":" + port_str + ": " + WinsockErrorText(last_error));

    fd_ = connected_sock;
    closed_ = false;
    eof_ = false;
}

std::size_t TcpSocket::Read(void* buf, std::size_t len, std::chrono::milliseconds timeout) {
    if (closed_)
        throw McpError(McpErrorCode::ConnectionClosed, "read on closed socket");
    if (eof_ || fd_ == INVALID_SOCKET) return 0;

    if (!WaitForSocket(fd_, POLLIN, static_cast<int>(timeout.count()))) return 0;

    int n = ::recv(fd_, static_cast<char*>(buf), static_cast<int>(len), 0);
    if (n == 0) {
        eof_ = true;
        return 0;
    }
    if (n == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) return 0;
        if (error == WSAECONNRESET) {
            eof_ = true;
            return 0;
        }
        throw McpError(McpErrorCode::ConnectionClosed,
                       "socket read failed: " + WinsockErrorText(error));
    }
    return static_cast<std::size_t>(n);
}

void TcpSocket::Write(const void* buf, std::size_t len, std::chrono::milliseconds timeout) {
    if (closed_)
        throw McpError(McpErrorCode::ConnectionClosed, "write on closed socket");
    if (fd_ == INVALID_SOCKET)
        throw McpError(McpErrorCode::ConnectionClosed, "write on unconnected socket");

    const char* data = static_cast<const char*>(buf);
    std::size_t total = 0;
    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (total < len) {
        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::milliseconds(0))
            throw McpError(McpErrorCode::RequestTimeout, "socket write timed out");
        if (!WaitForSocket(fd_, POLLOUT, static_cast<int>(remaining.count())))
            throw McpError(McpErrorCode::RequestTimeout, "socket write timed out");

        int n = ::send(fd_, data + total, static_cast<int>(len - total), 0);
        if (n == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) continue;
            throw McpError(McpErrorCode::ConnectionClosed,
                           "socket write failed: " + WinsockErrorText(error));
        }
        total += static_cast<std::size_t>(n);
    }
}

void TcpSocket::WaitWriteable(std::chrono::milliseconds timeout) {
    if (fd_ == INVALID_SOCKET) return;
    WSAPOLLFD pfd;
    pfd.fd = fd_;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    int rc;
    do {
        rc = ::WSAPoll(&pfd, 1, static_cast<int>(timeout.count()));
    } while (rc == SOCKET_ERROR && WSAGetLastError() == WSAEINTR);
}

void TcpSocket::WaitReadable(std::chrono::milliseconds timeout) {
    if (fd_ == INVALID_SOCKET) return;
    WSAPOLLFD pfd;
    pfd.fd = fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int rc;
    do {
        rc = ::WSAPoll(&pfd, 1, static_cast<int>(timeout.count()));
    } while (rc == SOCKET_ERROR && WSAGetLastError() == WSAEINTR);
}

void TcpSocket::Close() {
    if (fd_ != INVALID_SOCKET) {
        ::shutdown(fd_, SD_BOTH);
        ::closesocket(fd_);
        fd_ = INVALID_SOCKET;
    }
    closed_ = true;
}

}}} // namespace mcp::detail::net
