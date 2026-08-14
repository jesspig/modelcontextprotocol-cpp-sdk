// HttpClient.cpp — blocking HTTP/1.1 client implementation

#include <transport/detail/net/HttpClient.hpp>
#include <transport/detail/net/TlsSocket.hpp>
#include <mcp/transport/detail/Url.hpp>
#include <mcp/McpError.hpp>
#include <mcp/ErrorCodes.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace mcp { namespace detail { namespace net {

namespace {

constexpr std::size_t kMaxLineBytes = 8 * 1024;
constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
constexpr std::size_t kMaxHeaderCount = 100;
constexpr std::size_t kMaxBodyBytes = 8 * 1024 * 1024;
constexpr std::size_t kReadChunk = 8 * 1024;
constexpr std::chrono::milliseconds kMaxConnectTimeout(10000);

std::string ToLower(std::string_view text) {
    std::string result(text);
    for (char& c : result)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

void TrimInPlace(std::string& text) {
    std::size_t first = text.find_first_not_of(" \t");
    std::size_t last = text.find_last_not_of(" \t");
    if (first == std::string::npos) text.clear();
    else text = text.substr(first, last - first + 1);
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

std::chrono::milliseconds Remaining(const std::chrono::steady_clock::time_point& deadline) {
    auto left = deadline - std::chrono::steady_clock::now();
    if (left <= std::chrono::milliseconds(0)) return std::chrono::milliseconds(0);
    return std::chrono::duration_cast<std::chrono::milliseconds>(left);
}

} // anonymous namespace

HttpClient::HttpClient()
    : tcp_(std::make_unique<TcpSocket>()) {}

HttpClient::~HttpClient() {
    Close();
}

void HttpClient::Close() {
    if (closed_) return;
    closed_ = true;
    CloseConnection();
}

void HttpClient::CloseConnection() {
    connected_ = false;
    if (tls_ != nullptr) tls_->Close();
    if (tcp_ != nullptr) tcp_->Close();
}

bool HttpClient::IsEof() const {
    if (use_tls_) return tls_ == nullptr || tls_->IsEof();
    return tcp_ == nullptr || tcp_->IsEof();
}

HttpResponseInfo HttpClient::Request(const HttpRequestSpec& req,
                                     const std::function<void(std::string_view)>& body_cb) {
    if (closed_)
        throw McpError(McpErrorCode::ConnectionClosed, "HTTP client is closed");

    UrlParts url = ParseUrl(req.url);
    if (url.host.empty() || url.port == 0)
        throw McpError(McpErrorCode::ProtocolViolation, "invalid URL: " + req.url);
    bool want_tls = (url.scheme == "https");
    if (url.scheme != "http" && url.scheme != "https")
        throw McpError(McpErrorCode::ProtocolViolation, "unsupported scheme in URL: " + req.url);

    if (!connected_ || use_tls_ != want_tls || host_ != url.host || port_ != url.port || IsEof()) {
        CloseConnection();
        use_tls_ = want_tls;
        host_ = url.host;
        port_ = url.port;
        auto connect_timeout = std::min(req.timeout, kMaxConnectTimeout);
        if (use_tls_) {
            tls_ = std::make_unique<TlsSocket>(req.verify_tls);
            tls_->Connect(host_, port_, connect_timeout);
        } else {
            tcp_ = std::make_unique<TcpSocket>();
            tcp_->Connect(host_, port_, connect_timeout);
        }
        connected_ = true;
    }

    std::string path = url.path;
    if (path.empty()) path = "/";

    std::string request;
    request.reserve(256 + req.body.size());
    request += req.method + " " + path + " HTTP/1.1\r\n";
    std::string host_header = host_;
    bool default_port = (use_tls_ && port_ == 443) || (!use_tls_ && port_ == 80);
    if (!default_port) host_header += ":" + std::to_string(port_);
    request += "Host: " + host_header + "\r\n";
    for (const auto& [name, value] : req.headers) {
        if (name.find_first_of("\r\n") != std::string::npos ||
            value.find_first_of("\r\n") != std::string::npos)
            throw McpError(McpErrorCode::ProtocolViolation, "header contains CR or LF");
        request += name + ": " + value + "\r\n";
    }
    if (!req.body.empty())
        request += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
    request += "Connection: keep-alive\r\n\r\n";
    request += req.body;

    auto deadline = std::chrono::steady_clock::now() + req.timeout;
    HttpResponseInfo resp;
    bool http10 = false;
    bool keep_alive = true;

    try {
        WriteAll(request, deadline);

        std::string status_line = ReadLine(deadline);
        const std::string& sl = status_line;
        if (sl.size() < 12 || sl.compare(0, 7, "HTTP/1.") != 0 || sl[8] != ' ')
            throw McpError(McpErrorCode::ProtocolViolation, "malformed HTTP status line");
        http10 = (sl[7] == '0');
        if (!http10 && sl[7] != '1')
            throw McpError(McpErrorCode::ProtocolViolation, "unsupported HTTP version in status line");
        if (!std::isdigit(static_cast<unsigned char>(sl[9])) ||
            !std::isdigit(static_cast<unsigned char>(sl[10])) ||
            !std::isdigit(static_cast<unsigned char>(sl[11])))
            throw McpError(McpErrorCode::ProtocolViolation, "malformed HTTP status code");
        if (sl.size() > 12 && sl[12] != ' ')
            throw McpError(McpErrorCode::ProtocolViolation, "malformed HTTP status line");
        resp.status_code = (sl[9] - '0') * 100 + (sl[10] - '0') * 10 + (sl[11] - '0');
        if (sl.size() > 13) resp.status_text = sl.substr(13);

        std::size_t header_bytes = 0;
        std::size_t header_count = 0;
        bool chunked = false;
        bool has_content_length = false;
        std::size_t content_length = 0;
        for (;;) {
            std::string header = ReadLine(deadline);
            header_bytes += header.size() + 2;
            if (header_bytes > kMaxHeaderBytes)
                throw McpError(McpErrorCode::ProtocolViolation, "HTTP response headers too large");
            if (header.empty()) break;
            if (++header_count > kMaxHeaderCount)
                throw McpError(McpErrorCode::ProtocolViolation, "too many HTTP response headers");
            std::size_t colon = header.find(':');
            if (colon == std::string::npos)
                throw McpError(McpErrorCode::ProtocolViolation, "malformed HTTP header line");
            std::string name = ToLower(header.substr(0, colon));
            std::string value = header.substr(colon + 1);
            TrimInPlace(value);
            resp.headers[name] = value;
            if (name == "transfer-encoding") {
                std::string te = ToLower(value);
                if (te.find("chunked") != std::string::npos) chunked = true;
                else throw McpError(McpErrorCode::ProtocolViolation,
                                    "unsupported transfer encoding: " + value);
            } else if (name == "content-length") {
                if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                    throw McpError(McpErrorCode::ProtocolViolation, "malformed Content-Length");
                unsigned long long parsed;
                try {
                    parsed = std::stoull(value);
                } catch (const std::out_of_range&) {
                    throw McpError(McpErrorCode::ProtocolViolation, "Content-Length out of range");
                }
                if (parsed > kMaxBodyBytes)
                    throw McpError(McpErrorCode::ProtocolViolation, "HTTP response body exceeds size limit");
                content_length = static_cast<std::size_t>(parsed);
                has_content_length = true;
            } else if (name == "connection") {
                if (ToLower(value).find("close") != std::string::npos) keep_alive = false;
            }
        }

        if (chunked && has_content_length)
            throw McpError(McpErrorCode::ProtocolViolation,
                           "response has both Transfer-Encoding and Content-Length");

        if (chunked) {
            ReadChunkedBody(resp, body_cb, deadline);
        } else if (has_content_length) {
            ReadFixedBody(resp, content_length, body_cb, deadline);
        } else if (body_cb) {
            StreamBody(body_cb, req.timeout);
        } else {
            ReadUntilEof(resp, deadline);
        }
    } catch (...) {
        CloseConnection();
        throw;
    }

    if (http10 || !keep_alive) CloseConnection();
    return resp;
}

void HttpClient::WriteAll(std::string_view data, const std::chrono::steady_clock::time_point& deadline) {
    if (data.empty()) return;
    auto remaining = Remaining(deadline);
    if (remaining.count() <= 0)
        throw McpError(McpErrorCode::RequestTimeout, "HTTP request write timed out");
    if (use_tls_)
        tls_->Write(data.data(), data.size(), remaining);
    else
        tcp_->Write(data.data(), data.size(), remaining);
}

int HttpClient::ReadByte(const std::chrono::steady_clock::time_point& deadline) {
    auto remaining = Remaining(deadline);
    if (remaining.count() <= 0)
        throw McpError(McpErrorCode::RequestTimeout, "HTTP response read timed out");
    char byte = 0;
    std::size_t n = use_tls_ ? tls_->Read(&byte, 1, remaining) : tcp_->Read(&byte, 1, remaining);
    if (n == 1) return static_cast<unsigned char>(byte);
    if (IsEof())
        throw McpError(McpErrorCode::ConnectionClosed, "connection closed while reading HTTP response");
    throw McpError(McpErrorCode::RequestTimeout, "HTTP response read timed out");
}

std::string HttpClient::ReadLine(const std::chrono::steady_clock::time_point& deadline) {
    std::string line;
    line.reserve(128);
    for (;;) {
        int c = ReadByte(deadline);
        if (c == '\n') break;
        if (c == '\r') {
            int next = ReadByte(deadline);
            if (next != '\n')
                throw McpError(McpErrorCode::ProtocolViolation, "malformed line ending in HTTP response");
            break;
        }
        if (line.size() >= kMaxLineBytes)
            throw McpError(McpErrorCode::ProtocolViolation, "HTTP response line too long");
        line.push_back(static_cast<char>(c));
    }
    return line;
}

std::size_t HttpClient::ReadRaw(void* buf, std::size_t len, const std::chrono::steady_clock::time_point& deadline) {
    auto remaining = Remaining(deadline);
    if (remaining.count() <= 0)
        throw McpError(McpErrorCode::RequestTimeout, "HTTP response read timed out");
    std::size_t n = use_tls_ ? tls_->Read(buf, len, remaining) : tcp_->Read(buf, len, remaining);
    if (n > 0) return n;
    if (IsEof())
        throw McpError(McpErrorCode::ConnectionClosed, "connection closed while reading HTTP response body");
    return 0;
}

void HttpClient::ReadFixedBody(HttpResponseInfo& resp, std::size_t length,
                               const std::function<void(std::string_view)>& body_cb,
                               const std::chrono::steady_clock::time_point& deadline) {
    char buffer[kReadChunk];
    std::size_t total = 0;
    while (total < length) {
        std::size_t want = std::min(length - total, sizeof(buffer));
        std::size_t n = ReadRaw(buffer, want, deadline);
        if (n == 0) continue;
        total += n;
        if (body_cb) body_cb(std::string_view(buffer, n));
        else resp.body.append(buffer, n);
    }
}

void HttpClient::ReadChunkedBody(HttpResponseInfo& resp,
                                 const std::function<void(std::string_view)>& body_cb,
                                 const std::chrono::steady_clock::time_point& deadline) {
    char buffer[kReadChunk];
    std::size_t total = 0;
    for (;;) {
        std::string size_line = ReadLine(deadline);
        std::size_t semicolon = size_line.find(';');
        std::string hex = (semicolon == std::string::npos) ? size_line : size_line.substr(0, semicolon);
        TrimInPlace(hex);
        if (hex.empty() || hex.size() > 8 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            throw McpError(McpErrorCode::ProtocolViolation, "malformed chunk size line");
        std::size_t chunk_len = 0;
        for (char c : hex) chunk_len = chunk_len * 16 + static_cast<std::size_t>(HexValue(c));

        if (chunk_len == 0) {
            for (;;) {
                if (ReadLine(deadline).empty()) return;
            }
        }
        if (chunk_len > kMaxBodyBytes - total)
            throw McpError(McpErrorCode::ProtocolViolation, "HTTP response body exceeds size limit");

        std::size_t remaining = chunk_len;
        while (remaining > 0) {
            std::size_t want = std::min(remaining, sizeof(buffer));
            std::size_t n = ReadRaw(buffer, want, deadline);
            if (n == 0) continue;
            remaining -= n;
            total += n;
            if (body_cb) body_cb(std::string_view(buffer, n));
            else resp.body.append(buffer, n);
        }

        if (ReadByte(deadline) != '\r' || ReadByte(deadline) != '\n')
            throw McpError(McpErrorCode::ProtocolViolation, "malformed chunk terminator");
    }
}

void HttpClient::ReadUntilEof(HttpResponseInfo& resp, const std::chrono::steady_clock::time_point& deadline) {
    char buffer[kReadChunk];
    for (;;) {
        std::size_t n = ReadRaw(buffer, sizeof(buffer), deadline);
        if (n > 0) {
            if (resp.body.size() + n > kMaxBodyBytes)
                throw McpError(McpErrorCode::ProtocolViolation, "HTTP response body exceeds size limit");
            resp.body.append(buffer, n);
            continue;
        }
        if (IsEof()) return;
        if (Remaining(deadline).count() <= 0)
            throw McpError(McpErrorCode::RequestTimeout, "HTTP response read timed out");
    }
}

void HttpClient::StreamBody(const std::function<void(std::string_view)>& body_cb,
                            std::chrono::milliseconds idle_timeout) {
    char buffer[kReadChunk];
    for (;;) {
        if (closed_)
            throw McpError(McpErrorCode::ConnectionClosed, "HTTP client closed during streaming read");
        std::size_t n = use_tls_ ? tls_->Read(buffer, sizeof(buffer), idle_timeout)
                                 : tcp_->Read(buffer, sizeof(buffer), idle_timeout);
        if (n > 0) {
            body_cb(std::string_view(buffer, n));
            continue;
        }
        if (IsEof()) return;
        throw McpError(McpErrorCode::RequestTimeout, "HTTP stream idle timeout");
    }
}

}}} // namespace mcp::detail::net
