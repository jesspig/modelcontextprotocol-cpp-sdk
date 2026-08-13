// TcpSocketPosix.cpp — POSIX TCP socket implementation

#include <transport/detail/net/TcpSocket.hpp>
#include <mcp/McpError.hpp>
#include <mcp/ErrorCodes.hpp>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <string>

namespace mcp { namespace detail { namespace net {

namespace {

bool WaitForSocket(int fd, short events, int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    int rc;
    do {
        rc = ::poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);
    return rc > 0;
}

} // anonymous namespace

TcpSocket TcpSocket::FromFd(int fd) {
    TcpSocket s;
    s.fd_ = fd;
    s.closed_ = false;
    s.eof_ = false;
    return s;
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : fd_(other.fd_), eof_(other.eof_), closed_(other.closed_) {
    other.fd_ = kInvalidFd;
    other.closed_ = true;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        eof_ = other.eof_;
        closed_ = other.closed_;
        other.fd_ = kInvalidFd;
        other.closed_ = true;
    }
    return *this;
}

void TcpSocket::Connect(std::string_view host, uint16_t port, std::chrono::milliseconds timeout) {
    Close();

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
                       "DNS resolution failed for " + host_str + ": " + gai_strerror(gai));

    auto deadline = std::chrono::steady_clock::now() + timeout;
    int last_error = ECONNREFUSED;
    int connected_fd = -1;

    for (struct addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            ::close(fd);
            continue;
        }

        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::milliseconds(0)) {
            ::close(fd);
            throw McpError(McpErrorCode::RequestTimeout,
                           "connect timed out for " + host_str + ":" + port_str);
        }

        int rc = ::connect(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
        if (rc == 0) {
            connected_fd = fd;
            break;
        }
        if (errno != EINPROGRESS) {
            last_error = errno;
            ::close(fd);
            continue;
        }

        if (!WaitForSocket(fd, POLLOUT, static_cast<int>(remaining.count()))) {
            ::close(fd);
            throw McpError(McpErrorCode::RequestTimeout,
                           "connect timed out for " + host_str + ":" + port_str);
        }

        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len);
        if (so_error != 0) {
            last_error = so_error;
            ::close(fd);
            continue;
        }
        connected_fd = fd;
        break;
    }

    ::freeaddrinfo(results);

    if (connected_fd < 0)
        throw McpError(McpErrorCode::ConnectionRefused,
                       "connect failed for " + host_str + ":" + port_str + ": " + std::strerror(last_error));

    fd_ = connected_fd;
    closed_ = false;
    eof_ = false;
}

std::size_t TcpSocket::Read(void* buf, std::size_t len, std::chrono::milliseconds timeout) {
    if (closed_)
        throw McpError(McpErrorCode::ConnectionClosed, "read on closed socket");
    if (eof_ || fd_ < 0) return 0;

    if (!WaitForSocket(fd_, POLLIN, static_cast<int>(timeout.count()))) return 0;

    ssize_t n;
    do {
        n = ::read(fd_, buf, len);
    } while (n < 0 && errno == EINTR);

    if (n == 0) {
        eof_ = true;
        return 0;
    }
    if (n < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) return 0;
        throw McpError(McpErrorCode::ConnectionClosed,
                       std::string("socket read failed: ") + std::strerror(errno));
    }
    return static_cast<std::size_t>(n);
}

void TcpSocket::Write(const void* buf, std::size_t len, std::chrono::milliseconds timeout) {
    if (closed_)
        throw McpError(McpErrorCode::ConnectionClosed, "write on closed socket");
    if (fd_ < 0)
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

        ssize_t n;
        do {
            n = ::write(fd_, data + total, len - total);
        } while (n < 0 && errno == EINTR);

        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) continue;
            throw McpError(McpErrorCode::ConnectionClosed,
                           std::string("socket write failed: ") + std::strerror(errno));
        }
        total += static_cast<std::size_t>(n);
    }
}

void TcpSocket::WaitWriteable(std::chrono::milliseconds timeout) {
    if (fd_ < 0) return;
    struct pollfd pfd;
    pfd.fd = fd_;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    int rc;
    do {
        rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
    } while (rc < 0 && errno == EINTR);
}

void TcpSocket::WaitReadable(std::chrono::milliseconds timeout) {
    if (fd_ < 0) return;
    struct pollfd pfd;
    pfd.fd = fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int rc;
    do {
        rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
    } while (rc < 0 && errno == EINTR);
}

void TcpSocket::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    closed_ = true;
}

}}} // namespace mcp::detail::net
