// TlsSocket.cpp — TLS socket implementation over TcpSocket using OpenSSL

#include <transport/detail/net/TlsSocket.hpp>
#include <mcp/McpError.hpp>
#include <mcp/ErrorCodes.hpp>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>

#ifdef MCP_HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#endif

namespace mcp { namespace detail { namespace net {

#ifdef MCP_HAVE_OPENSSL

namespace {

constexpr std::chrono::milliseconds kTlsCloseNotifyWait(500);

SSL_CTX* GetSharedCtx() {
    // Process-wide singleton, intentionally never freed; OpenSSL cleans up
    // all its global state at process exit.
    static SSL_CTX* ctx = [] {
        OPENSSL_init_ssl(0, nullptr);
        SSL_CTX* created = SSL_CTX_new(TLS_client_method());
        if (created != nullptr)
            SSL_CTX_set_default_verify_paths(created);
        return created;
    }();
    return ctx;
}

std::string SslErrorText(int ssl_error) {
    unsigned long error = ERR_get_error();
    char buf[256] = {0};
    if (error != 0) {
        ERR_error_string_n(error, buf, sizeof(buf));
        return std::string(buf);
    }
    if (ssl_error == SSL_ERROR_SYSCALL)
        return std::string("SSL_ERROR_SYSCALL, errno=") + std::strerror(errno);
    return "unknown SSL error";
}

} // anonymous namespace

TlsSocket::TlsSocket(bool verify_peer)
    : verify_peer_(verify_peer) {}

TlsSocket::~TlsSocket() {
    Close();
}

void TlsSocket::Connect(std::string_view host, uint16_t port, std::chrono::milliseconds timeout) {
    Close();
    tcp_.Connect(host, port, timeout);
    std::string host_str(host);

    try {
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            ctx_ = GetSharedCtx();
            if (ctx_ == nullptr)
                throw McpError(McpErrorCode::TlsHandshakeFailed, "failed to initialize OpenSSL");
            ssl_ = SSL_new(ctx_);
            if (ssl_ == nullptr)
                throw McpError(McpErrorCode::TlsHandshakeFailed, "SSL_new failed");
            SSL_set_fd(ssl_, static_cast<int>(tcp_.NativeHandle()));
            if (verify_peer_) {
                SSL_set_verify(ssl_, SSL_VERIFY_PEER, nullptr);
                X509_VERIFY_PARAM* param = SSL_get0_param(ssl_);
                X509_VERIFY_PARAM_set1_host(param, host_str.c_str(), 0);
            }
        }

        auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            int rc;
            int err;
            {
                std::lock_guard<std::mutex> lock(io_mutex_);
                if (ssl_ == nullptr)
                    throw McpError(McpErrorCode::ConnectionClosed,
                                   "connection closed during TLS handshake with " + host_str);
                rc = SSL_connect(ssl_);
                if (rc == 1) break;
                err = SSL_get_error(ssl_, rc);
            }
            auto remaining = deadline - std::chrono::steady_clock::now();
            if (err == SSL_ERROR_WANT_READ) {
                if (remaining <= std::chrono::milliseconds(0))
                    throw McpError(McpErrorCode::RequestTimeout, "TLS handshake timed out for " + host_str);
                tcp_.WaitReadable(std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
                if (tcp_.IsEof())
                    throw McpError(McpErrorCode::ConnectionClosed,
                                   "connection closed during TLS handshake with " + host_str);
                continue;
            }
            if (err == SSL_ERROR_WANT_WRITE) {
                if (remaining <= std::chrono::milliseconds(0))
                    throw McpError(McpErrorCode::RequestTimeout, "TLS handshake timed out for " + host_str);
                tcp_.WaitWriteable(std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
                continue;
            }
            throw McpError(McpErrorCode::TlsHandshakeFailed,
                           "TLS handshake failed for " + host_str + ": " + SslErrorText(err));
        }

        std::lock_guard<std::mutex> lock(io_mutex_);
        if (ssl_ == nullptr)
            throw McpError(McpErrorCode::ConnectionClosed,
                           "connection closed during TLS handshake with " + host_str);
        if (verify_peer_) {
            long result = SSL_get_verify_result(ssl_);
            if (result != X509_V_OK)
                throw McpError(McpErrorCode::TlsHandshakeFailed,
                               "TLS certificate verification failed for " + host_str + ": " +
                                   X509_verify_cert_error_string(result));
        }
    } catch (...) {
        Close();
        throw;
    }
}

std::size_t TlsSocket::Read(void* buf, std::size_t len, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    bool want_write = false;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            if (ssl_ == nullptr)
                throw McpError(McpErrorCode::ConnectionClosed, "read on closed TLS socket");
            if (eof_) return 0;

            if (SSL_pending(ssl_) > 0) {
                int n = SSL_read(ssl_, buf, static_cast<int>(len));
                if (n > 0) return static_cast<std::size_t>(n);
                int err = SSL_get_error(ssl_, n);
                if (err == SSL_ERROR_WANT_READ) {
                    want_write = false;
                } else if (err == SSL_ERROR_WANT_WRITE) {
                    want_write = true;
                } else if (err == SSL_ERROR_ZERO_RETURN) {
                    eof_ = true;
                    return 0;
                } else if (err == SSL_ERROR_SYSCALL && n == 0 && errno == 0 && tcp_.IsEof()) {
                    eof_ = true;
                    return 0;
                } else {
                    throw McpError(McpErrorCode::ConnectionClosed,
                                   std::string("TLS read failed: ") + SslErrorText(err));
                }
            }
        }

        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::milliseconds(0)) return 0;

        if (want_write) {
            tcp_.WaitWriteable(std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
            continue;
        }
        tcp_.WaitReadable(std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
    }
}

void TlsSocket::Write(const void* buf, std::size_t len, std::chrono::milliseconds timeout) {
    const char* data = static_cast<const char*>(buf);
    std::size_t total = 0;
    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (total < len) {
        int n;
        int err;
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            if (ssl_ == nullptr)
                throw McpError(McpErrorCode::ConnectionClosed, "write on closed TLS socket");
            n = SSL_write(ssl_, data + total, static_cast<int>(len - total));
            if (n > 0) {
                total += static_cast<std::size_t>(n);
                continue;
            }
            err = SSL_get_error(ssl_, n);
        }
        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::milliseconds(0))
            throw McpError(McpErrorCode::RequestTimeout, "TLS write timed out");
        if (err == SSL_ERROR_WANT_READ) {
            tcp_.WaitReadable(std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
            if (tcp_.IsEof())
                throw McpError(McpErrorCode::ConnectionClosed, "connection closed during TLS write");
        } else if (err == SSL_ERROR_WANT_WRITE) {
            tcp_.WaitWriteable(std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
        } else {
            throw McpError(McpErrorCode::ConnectionClosed,
                           std::string("TLS write failed: ") + SslErrorText(err));
        }
    }
}

void TlsSocket::Close() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (ssl_ != nullptr) {
        if (SSL_shutdown(ssl_) == 0) {
            if (tcp_.WaitReadable(kTlsCloseNotifyWait))
                SSL_shutdown(ssl_);
        }
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    tcp_.Close();
}

bool TlsSocket::IsEof() const {
    std::lock_guard<std::mutex> lock(io_mutex_);
    return eof_ || tcp_.IsEof();
}

bool TlsSocket::IsConnected() const {
    std::lock_guard<std::mutex> lock(io_mutex_);
    return ssl_ != nullptr && tcp_.IsConnected() && !eof_;
}

#else // !MCP_HAVE_OPENSSL

TlsSocket::TlsSocket(bool) {}

TlsSocket::~TlsSocket() {
    Close();
}

void TlsSocket::Connect(std::string_view, uint16_t, std::chrono::milliseconds) {
    throw McpError(McpErrorCode::TlsHandshakeFailed, "TLS support not compiled in");
}

std::size_t TlsSocket::Read(void*, std::size_t, std::chrono::milliseconds) {
    throw McpError(McpErrorCode::TlsHandshakeFailed, "TLS support not compiled in");
}

void TlsSocket::Write(const void*, std::size_t, std::chrono::milliseconds) {
    throw McpError(McpErrorCode::TlsHandshakeFailed, "TLS support not compiled in");
}

void TlsSocket::Close() {
    tcp_.Close();
}

bool TlsSocket::IsEof() const {
    return tcp_.IsEof();
}

bool TlsSocket::IsConnected() const {
    return false;
}

#endif // MCP_HAVE_OPENSSL

}}} // namespace mcp::detail::net
