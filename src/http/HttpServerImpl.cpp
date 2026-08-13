// HttpServerImpl.cpp — 自研 HTTP/1.1 服务器（替换 libhv）

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// Windows.h defines GetObject macro which conflicts with JsonValue::GetObject
#pragma push_macro("GetObject")
#undef GetObject
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include <http/HttpServerImpl.hpp>
#include <mcp/Log.hpp>
#include <mcp/detail/ThreadUtils.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <transport/detail/net/TcpSocket.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace mcp { namespace detail { namespace http_server_impl {

inline constexpr std::size_t kMaxRequestLine = 8 * 1024;
inline constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
inline constexpr std::size_t kMaxHeaderCount = 100;
inline constexpr std::size_t kMaxBodyBytes = mcp::detail::kMaxMessageSize;
inline constexpr std::size_t kMaxConnections = 256;
inline constexpr std::chrono::milliseconds kKeepAliveIdleTimeout(30000);
inline constexpr std::chrono::milliseconds kIoTimeout(30000);

inline void CloseFd(int fd) {
#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(fd);
#endif
}

inline int LastSocketError() {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

inline std::string SocketErrorText(int error) {
#ifdef _WIN32
    char buf[256] = {0};
    DWORD written = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, static_cast<DWORD>(error), 0,
                                     buf, static_cast<DWORD>(sizeof(buf)), nullptr);
    if (written == 0)
        return "Winsock error " + std::to_string(error);
    std::string text(buf, written);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' '))
        text.pop_back();
    return text;
#else
    return std::strerror(error);
#endif
}

#ifdef _WIN32
inline void EnsureWinsockOnce() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA data;
        ok = (::WSAStartup(MAKEWORD(2, 2), &data) == 0);
    });
    if (!ok)
        throw std::runtime_error("HttpServer: WSAStartup failed");
}
#endif

inline bool HasCloseToken(std::string_view value) {
    std::size_t pos = 0;
    while (pos <= value.size()) {
        auto end = value.find(',', pos);
        auto token = value.substr(pos, end - pos);
        auto first = token.find_first_not_of(" \t");
        auto last = token.find_last_not_of(" \t");
        if (first != std::string_view::npos) {
            auto t = token.substr(first, last - first + 1);
            if (t.size() == 5 &&
                std::tolower(static_cast<unsigned char>(t[0])) == 'c' &&
                std::tolower(static_cast<unsigned char>(t[1])) == 'l' &&
                std::tolower(static_cast<unsigned char>(t[2])) == 'o' &&
                std::tolower(static_cast<unsigned char>(t[3])) == 's' &&
                std::tolower(static_cast<unsigned char>(t[4])) == 'e')
                return true;
        }
        if (end == std::string_view::npos)
            break;
        pos = end + 1;
    }
    return false;
}

inline void AppendCorsHeaders(std::string& out) {
    out += "Access-Control-Allow-Origin: *\r\n";
    out += "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n";
    out += "Access-Control-Allow-Headers: Content-Type, Authorization, Mcp-Method, "
           "Mcp-Param-*, Mcp-Name, Last-Event-ID, If-None-Match\r\n";
    out += "Access-Control-Expose-Headers: Mcp-Session-Id, WWW-Authenticate\r\n";
}

Impl::~Impl() {
    Stop();
}

void Impl::Start(uint16_t port, const HandlerMap& handlers,
                 const HttpServerOptions& options) {
    options_ = options;
    running_.store(true);

#ifdef _WIN32
    EnsureWinsockOnce();
#endif

    int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (fd < 0)
        throw std::runtime_error("HttpServer: socket() failed: " +
                                 SocketErrorText(LastSocketError()));

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        CloseFd(fd);
        throw std::runtime_error("HttpServer: bind failed on port " +
                                 std::to_string(port) + ": " +
                                 SocketErrorText(LastSocketError()));
    }
    if (::listen(fd, 16) < 0) {
        CloseFd(fd);
        throw std::runtime_error("HttpServer: listen failed on port " +
                                 std::to_string(port) + ": " +
                                 SocketErrorText(LastSocketError()));
    }

    listen_fd_ = fd;
    try {
        accept_thread_ = std::thread(
            [this, port, handlers]() mutable {
                AcceptLoop(port, std::move(handlers));
            });
    } catch (...) {
        CloseFd(fd);
        listen_fd_ = -1;
        running_.store(false);
        throw;
    }
}

void Impl::Stop() {
    if (!running_.exchange(false))
        return;
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        if (listen_fd_ != -1) {
            CloseFd(listen_fd_);
            listen_fd_ = -1;
        }
    }
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        fds = conn_fds_;
    }
    for (int cfd : fds)
        CloseFd(cfd);
    detail::JoinThreadSafely(accept_thread_);
    for (auto& e : conn_threads_)
        detail::JoinThreadSafely(e.thread);
    conn_threads_.clear();
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        conn_fds_.clear();
    }
}

void Impl::AcceptLoop(uint16_t port, HandlerMap handlers) {
    (void)port;
    int listen_fd = listen_fd_;
    for (;;) {
        if (!running_.load())
            break;
        std::vector<ConnEntry> finished;
        {
            std::lock_guard<std::mutex> lock(conns_mutex_);
            for (auto it = conn_threads_.begin(); it != conn_threads_.end();) {
                if (it->done->load()) {
                    finished.push_back(std::move(*it));
                    it = conn_threads_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& e : finished)
            detail::JoinThreadSafely(e.thread);
        int cfd = static_cast<int>(::accept(listen_fd, nullptr, nullptr));
        if (cfd < 0) {
            if (!running_.load())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        bool over_limit = false;
        {
            std::lock_guard<std::mutex> lock(conns_mutex_);
            if (conn_fds_.size() >= kMaxConnections) {
                over_limit = true;
            } else {
                conn_fds_.push_back(cfd);
            }
        }
        if (over_limit) {
            try {
                net::TcpSocket conn = net::TcpSocket::FromFd(cfd);
                WriteResponse(conn, 503, "Service Unavailable", {}, {}, false);
            } catch (const std::exception&) {
            }
            continue;
        }
        try {
            auto done = std::make_shared<std::atomic<bool>>(false);
            conn_threads_.emplace_back(ConnEntry{
                std::thread(&Impl::HandleConnection, this, cfd, handlers, done), done});
        } catch (const std::exception&) {
            CloseFd(cfd);
        }
    }
    std::lock_guard<std::mutex> lock(conns_mutex_);
    if (listen_fd_ != -1) {
        CloseFd(listen_fd_);
        listen_fd_ = -1;
    }
}

void Impl::HandleConnection(int fd, HandlerMap handlers,
                            std::shared_ptr<std::atomic<bool>> done) {
    try {
        HandleConnectionInner(fd, std::move(handlers));
    } catch (const std::exception& e) {
        MCP_LOG(Warning, std::string("connection handler error: ") + e.what());
    }
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        auto it = std::find(conn_fds_.begin(), conn_fds_.end(), fd);
        if (it != conn_fds_.end())
            conn_fds_.erase(it);
    }
    done->store(true);
}

void Impl::HandleConnectionInner(int fd, HandlerMap handlers) {
    auto conn = std::make_shared<net::TcpSocket>(net::TcpSocket::FromFd(fd));
    std::string buffer;
    bool keep_alive = true;
    bool first_request = true;

    while (running_.load() && keep_alive) {
        auto read_timeout = first_request ? kIoTimeout : kKeepAliveIdleTimeout;
        first_request = false;

        std::string method, path, version;
        auto rl = ReadRequestLine(*conn, buffer, method, path, version, read_timeout);
        if (rl == RequestLineResult::Close)
            return;
        if (rl == RequestLineResult::BadRequest) {
            try {
                WriteResponse(*conn, 400, "Bad Request", {}, {}, false);
            } catch (const std::exception&) {
            }
            return;
        }

        std::unordered_map<std::string, std::string> headers;
        std::size_t content_length = 0;
        auto hr = ReadHeaderBlock(*conn, buffer, headers, content_length, kIoTimeout);
        if (hr == HeaderResult::Close)
            return;
        if (hr == HeaderResult::BadRequest) {
            try {
                WriteResponse(*conn, 400, "Bad Request", {}, {}, false);
            } catch (const std::exception&) {
            }
            return;
        }
        if (hr == HeaderResult::PayloadTooLarge) {
            try {
                WriteResponse(*conn, 413, "Payload Too Large", {}, {}, false);
            } catch (const std::exception&) {
            }
            return;
        }

        auto conn_it = headers.find("connection");
        if (conn_it != headers.end() && HasCloseToken(conn_it->second))
            keep_alive = false;

        std::string body;
        if (!ReadBody(*conn, buffer, content_length, body, kIoTimeout))
            return;

        HttpRequest req;
        req.method = method;
        req.path = path;
        req.headers = headers;
        req.body = body;

        if (options_.on_request) {
            try {
                options_.on_request(req);
            } catch (const std::exception& e) {
                MCP_LOG(Warning, std::string("on_request callback threw: ") + e.what());
            }
        }

        if (method == "OPTIONS") {
            try {
                WriteResponse(*conn, 204, "No Content", {}, {}, keep_alive);
            } catch (const std::exception&) {
                return;
            }
            continue;
        }

        HttpResponse resp;
        if (!IsRequestAllowed(req, options_)) {
            resp.status_code = 403;
            resp.status_text = "Forbidden";
            MCP_LOG(Warning, "HTTP request rejected: Host/Origin not allowed");
        } else {
            auto it = handlers.find({method, path});
            if (it == handlers.end()) {
                resp.status_code = 404;
                resp.status_text = "Not Found";
            } else {
                try {
                    it->second(req, resp);
                } catch (const std::exception& e) {
                    MCP_LOG(Error, std::string("HTTP handler threw: ") + e.what());
                    resp.status_code = 500;
                    resp.status_text = "Internal Server Error";
                }
            }
        }

        std::unordered_map<std::string, std::string> safe_headers;
        for (const auto& [k, v] : resp.headers) {
            if (k.find('\r') != std::string::npos || k.find('\n') != std::string::npos ||
                v.find('\r') != std::string::npos || v.find('\n') != std::string::npos) {
                MCP_LOG(Warning, "HTTP response header with CR/LF dropped");
                continue;
            }
            safe_headers[k] = v;
        }

        if (resp.is_sse) {
            try {
                WriteSseHeaders(*conn, safe_headers);
                if (!resp.body.empty())
                    conn->Write(resp.body.data(), resp.body.size());
            } catch (const std::exception&) {
                return;
            }
            auto id = AddSseClient([conn](std::string_view data) {
                conn->Write(data.data(), data.size());
            });
            if (options_.on_connect) {
                try {
                    options_.on_connect();
                } catch (const std::exception& e) {
                    MCP_LOG(Warning, std::string("on_connect callback threw: ") + e.what());
                }
            }
            while (running_.load() && !conn->IsEof()) {
                char byte = 0;
                try {
                    if (conn->Read(&byte, 1, kIoTimeout) == 0 && conn->IsEof())
                        break;
                } catch (const std::exception&) {
                    break;
                }
            }
            RemoveSseClient(id, true);
            return;
        }

        try {
            WriteResponse(*conn, resp.status_code, resp.status_text,
                          safe_headers, resp.body, keep_alive);
        } catch (const std::exception&) {
            return;
        }
    }
}

Impl::LineResult Impl::ReadLine(net::TcpSocket& conn, std::string& buffer,
                                std::string& line, std::chrono::milliseconds timeout,
                                std::size_t max_line_bytes) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        auto nl = buffer.find('\n');
        if (nl != std::string::npos) {
            line.assign(buffer, 0, nl);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            buffer.erase(0, nl + 1);
            if (line.size() > max_line_bytes)
                return LineResult::TooLong;
            return LineResult::Ok;
        }
        if (buffer.size() > max_line_bytes)
            return LineResult::TooLong;
        char chunk[4096];
        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::milliseconds(0))
            return LineResult::Timeout;
        auto n = conn.Read(chunk, sizeof(chunk), std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
        if (n > 0) {
            buffer.append(chunk, n);
            continue;
        }
        if (conn.IsEof())
            return LineResult::Eof;
        return LineResult::Timeout;
    }
}

Impl::RequestLineResult Impl::ReadRequestLine(
    net::TcpSocket& conn, std::string& buffer,
    std::string& method, std::string& path, std::string& version,
    std::chrono::milliseconds timeout) {
    std::string line;
    auto r = ReadLine(conn, buffer, line, timeout, kMaxRequestLine);
    if (r == LineResult::TooLong)
        return RequestLineResult::BadRequest;
    if (r != LineResult::Ok)
        return RequestLineResult::Close;

    auto sp1 = line.find(' ');
    if (sp1 == std::string::npos)
        return RequestLineResult::BadRequest;
    auto sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos)
        return RequestLineResult::BadRequest;
    if (line.find(' ', sp2 + 1) != std::string::npos)
        return RequestLineResult::BadRequest;

    method.assign(line, 0, sp1);
    path.assign(line, sp1 + 1, sp2 - sp1 - 1);
    version.assign(line, sp2 + 1);
    if (method.empty() || path.empty())
        return RequestLineResult::BadRequest;
    if (version != "HTTP/1.0" && version != "HTTP/1.1")
        return RequestLineResult::BadRequest;
    return RequestLineResult::Ok;
}

Impl::HeaderResult Impl::ReadHeaderBlock(
    net::TcpSocket& conn, std::string& buffer,
    std::unordered_map<std::string, std::string>& headers,
    std::size_t& content_length_out, std::chrono::milliseconds timeout) {
    headers.clear();
    content_length_out = 0;
    std::size_t total = 0;
    for (;;) {
        std::string line;
        auto r = ReadLine(conn, buffer, line, timeout, kMaxHeaderBytes);
        if (r == LineResult::Timeout || r == LineResult::Eof)
            return HeaderResult::Close;
        if (r == LineResult::TooLong)
            return HeaderResult::BadRequest;
        if (line.empty())
            break;
        total += line.size() + 2;
        if (total > kMaxHeaderBytes || headers.size() >= kMaxHeaderCount)
            return HeaderResult::BadRequest;
        auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0)
            return HeaderResult::BadRequest;
        std::string name(line, 0, colon);
        for (auto& c : name)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string value(line, colon + 1);
        auto first = value.find_first_not_of(" \t");
        auto last = value.find_last_not_of(" \t");
        if (first == std::string::npos)
            value.clear();
        else
            value = value.substr(first, last - first + 1);
        headers[std::move(name)] = std::move(value);
    }

    auto cl = headers.find("content-length");
    if (cl != headers.end()) {
        if (cl->second.empty())
            return HeaderResult::BadRequest;
        std::size_t n = 0;
        for (char c : cl->second) {
            if (c < '0' || c > '9')
                return HeaderResult::BadRequest;
            n = n * 10 + static_cast<std::size_t>(c - '0');
            if (n > kMaxBodyBytes)
                return HeaderResult::PayloadTooLarge;
        }
        content_length_out = n;
    }
    if (headers.find("transfer-encoding") != headers.end())
        return HeaderResult::BadRequest;
    return HeaderResult::Ok;
}

bool Impl::ReadBody(net::TcpSocket& conn, std::string& buffer, std::size_t content_length,
                    std::string& body, std::chrono::milliseconds timeout) {
    body.clear();
    if (content_length == 0)
        return true;
    body.reserve(content_length);
    std::size_t from_buffer = std::min(content_length, buffer.size());
    body.append(buffer, 0, from_buffer);
    buffer.erase(0, from_buffer);
    std::size_t got = body.size();
    char chunk[16384];
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (got < content_length) {
        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::milliseconds(0))
            return false;
        auto want = std::min(sizeof(chunk), content_length - got);
        auto n = conn.Read(chunk, want, std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
        if (n > 0) {
            body.append(chunk, n);
            got += n;
            continue;
        }
        return false;
    }
    return true;
}

void Impl::WriteResponse(net::TcpSocket& conn, int status,
                         std::string_view status_text,
                         const std::unordered_map<std::string, std::string>& headers,
                         std::string_view body, bool keep_alive) {
    std::string out;
    out.reserve(512 + body.size() + headers.size() * 32);
    out += "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += std::string(status_text);
    out += "\r\n";
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n";
    out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    for (const auto& [k, v] : headers) {
        out += k;
        out += ": ";
        out += v;
        out += "\r\n";
    }
    AppendCorsHeaders(out);
    out += "\r\n";
    out.append(body.data(), body.size());
    conn.Write(out.data(), out.size());
}

void Impl::WriteSseHeaders(
    net::TcpSocket& conn,
    const std::unordered_map<std::string, std::string>& headers) {
    std::string out;
    out.reserve(512 + headers.size() * 32);
    out += "HTTP/1.1 200 OK\r\n";
    out += "Content-Type: text/event-stream\r\n";
    out += "Cache-Control: no-cache\r\n";
    out += "Connection: keep-alive\r\n";
    for (const auto& [k, v] : headers) {
        out += k;
        out += ": ";
        out += v;
        out += "\r\n";
    }
    AppendCorsHeaders(out);
    out += "\r\n";
    conn.Write(out.data(), out.size());
}

bool Impl::IsRequestAllowed(const HttpRequest& req, const HttpServerOptions& options) {
    auto host_it = req.headers.find("host");
    if (host_it == req.headers.end())
        return false;
    bool host_ok = false;
    if (!options.allowed_hosts.empty()) {
        for (const auto& allowed : options.allowed_hosts) {
            if (host_it->second == allowed) {
                host_ok = true;
                break;
            }
        }
    } else {
        host_ok = IsLocalhostHost(host_it->second);
    }
    if (!host_ok)
        return false;

    auto origin_it = req.headers.find("origin");
    if (origin_it == req.headers.end() || options.allowed_origins.empty())
        return true;
    for (const auto& allowed : options.allowed_origins) {
        if (origin_it->second == allowed)
            return true;
    }
    return false;
}

bool Impl::IsLocalhostHost(const std::string& host_with_port) {
    std::string h = host_with_port;
    auto colon = h.rfind(':');
    if (colon != std::string::npos) {
        if (h.front() == '[' && h.back() == ']') {
            h = h.substr(1, h.size() - 2);
        } else if (h.front() == '[') {
            auto close = h.find(']');
            if (close != std::string::npos)
                h = h.substr(1, close - 1);
        } else {
            h = h.substr(0, colon);
        }
    }
    return h == "localhost" || h == "127.0.0.1" || h == "::1";
}

uint64_t Impl::AddSseClient(std::function<void(std::string_view)> send_fn) {
    auto entry = std::make_shared<SseClientEntry>();
    entry->send_fn = std::move(send_fn);
    std::lock_guard<std::mutex> lock(sse_mutex_);
    auto id = next_sse_id_++;
    sse_clients_[id] = std::move(entry);
    return id;
}

bool Impl::RemoveSseClient(uint64_t id, bool call_on_disconnect) {
    HttpDisconnectCallback on_disconnect;
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        removed = sse_clients_.erase(id) != 0;
        on_disconnect = options_.on_disconnect;
    }
    if (removed && call_on_disconnect && on_disconnect) {
        try {
            on_disconnect();
        } catch (const std::exception& e) {
            MCP_LOG(Warning, std::string("on_disconnect callback threw: ") + e.what());
        }
    }
    return removed;
}

void Impl::RemoveSseClientEntry(const std::shared_ptr<SseClientEntry>& entry,
                                bool call_on_disconnect) {
    HttpDisconnectCallback on_disconnect;
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        for (auto it = sse_clients_.begin(); it != sse_clients_.end(); ++it) {
            if (it->second == entry) {
                sse_clients_.erase(it);
                removed = true;
                break;
            }
        }
        on_disconnect = options_.on_disconnect;
    }
    if (removed && call_on_disconnect && on_disconnect) {
        try {
            on_disconnect();
        } catch (const std::exception& e) {
            MCP_LOG(Warning, std::string("on_disconnect callback threw: ") + e.what());
        }
    }
}

void Impl::BroadcastSse(std::string_view event) {
    std::vector<std::shared_ptr<SseClientEntry>> entries;
    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        entries.reserve(sse_clients_.size());
        for (auto& [id, entry] : sse_clients_)
            entries.push_back(entry);
    }
    for (auto& entry : entries) {
        try {
            std::lock_guard<std::mutex> write_lock(entry->write_mutex);
            entry->send_fn(event);
        } catch (const std::exception& e) {
            MCP_LOG(Warning, std::string("SSE send failed: ") + e.what());
            RemoveSseClientEntry(entry, true);
        }
    }
}

}}} // namespace mcp::detail::http_server_impl
