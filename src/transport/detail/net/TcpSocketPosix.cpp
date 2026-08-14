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
#include <limits>
#include <string>
#include <thread>

namespace mcp { namespace detail { namespace net {

namespace {

#ifdef __APPLE__
constexpr int kSendFlags = 0;
#else
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

template <typename Rep, typename Period>
int ClampTimeoutMs(std::chrono::duration<Rep, Period> ms) {
    auto count = std::chrono::duration_cast<std::chrono::milliseconds>(ms).count();
    if (count > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
    if (count < 0) return 0;
    return static_cast<int>(count);
}

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

bool TcpSocket::WaitForEvents(short events, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (closed_.load() || fd_ < 0) return false;
        long long remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
        int slice = remaining > kPollSliceMs
                        ? kPollSliceMs
                        : remaining > 0 ? static_cast<int>(remaining) : 0;
        if (WaitForSocket(fd_, events, slice)) return true;
        if (slice <= 0) return false;
    }
}

TcpSocket TcpSocket::FromFd(NativeFd fd) {
    TcpSocket s;
    s.fd_ = fd;
    s.closed_.store(false);
    s.eof_ = false;
    return s;
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : fd_(other.fd_), eof_(other.eof_), closed_(other.closed_.load()) {
    other.fd_ = kInvalidFd;
    other.closed_.store(true);
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        eof_ = other.eof_;
        closed_.store(other.closed_.load());
        other.fd_ = kInvalidFd;
        other.closed_.store(true);
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
        if (rc != 0 && errno != EINPROGRESS && errno != EINTR) {
            last_error = errno;
            ::close(fd);
            continue;
        }

        if (!WaitForSocket(fd, POLLOUT, ClampTimeoutMs(remaining))) {
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
#ifdef __APPLE__
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    closed_.store(false);
    eof_ = false;
}

std::size_t TcpSocket::Read(void* buf, std::size_t len, std::chrono::milliseconds timeout) {
    if (closed_.load())
        throw McpError(McpErrorCode::ConnectionClosed, "read on closed socket");
    if (eof_ || fd_ < 0) return 0;

    if (!WaitForEvents(POLLIN, ClampTimeoutMs(timeout))) {
        if (closed_.load())
            throw McpError(McpErrorCode::ConnectionClosed, "read on closed socket");
        return 0;
    }

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
    if (closed_.load())
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
        if (!WaitForEvents(POLLOUT, ClampTimeoutMs(remaining))) {
            if (closed_.load())
                throw McpError(McpErrorCode::ConnectionClosed, "write on closed socket");
            throw McpError(McpErrorCode::RequestTimeout, "socket write timed out");
        }

        ssize_t n;
        do {
            n = ::send(fd_, data + total, len - total, kSendFlags);
        } while (n < 0 && errno == EINTR);

        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            throw McpError(McpErrorCode::ConnectionClosed,
                           std::string("socket write failed: ") + std::strerror(errno));
        }
        total += static_cast<std::size_t>(n);
    }
}

void TcpSocket::WaitWriteable(std::chrono::milliseconds timeout) {
    if (fd_ < 0) return;
    WaitForEvents(POLLOUT, ClampTimeoutMs(timeout));
}

void TcpSocket::WaitReadable(std::chrono::milliseconds timeout) {
    if (fd_ < 0) return;
    WaitForEvents(POLLIN, ClampTimeoutMs(timeout));
}

void TcpSocket::Close() {
    closed_.store(true);
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
}

}}} // namespace mcp::detail::net
