// WebSocketClient.cpp — RFC 6455 WebSocket 客户端

#include <transport/detail/net/WebSocketClient.hpp>
#include <transport/detail/net/Sha1.hpp>
#include <transport/detail/net/TlsSocket.hpp>
#include <mcp/detail/ThreadUtils.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <mcp/transport/detail/Url.hpp>
#include <mcp/McpError.hpp>
#include <mcp/ErrorCodes.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>

namespace mcp { namespace detail { namespace net {

namespace {

constexpr std::size_t kMaxLineBytes = 8 * 1024;
constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
constexpr std::chrono::milliseconds kFrameIdleTimeout(24 * 60 * 60 * 1000);
constexpr std::chrono::milliseconds kSendTimeout(30000);
constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr uint8_t kOpcodeContinuation = 0x0;
constexpr uint8_t kOpcodeText = 0x1;
constexpr uint8_t kOpcodeBinary = 0x2;
constexpr uint8_t kOpcodeClose = 0x8;
constexpr uint8_t kOpcodePing = 0x9;
constexpr uint8_t kOpcodePong = 0xA;

std::chrono::milliseconds Remaining(const std::chrono::steady_clock::time_point& deadline) {
    auto left = deadline - std::chrono::steady_clock::now();
    if (left <= std::chrono::milliseconds(0)) return std::chrono::milliseconds(0);
    return std::chrono::duration_cast<std::chrono::milliseconds>(left);
}

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

} // anonymous namespace

WebSocketClient::WebSocketClient() = default;

WebSocketClient::~WebSocketClient() {
    Close();
}

void WebSocketClient::SetCallbacks(MessageCallback on_message, CloseCallback on_close,
                                   ErrorCallback on_error) {
    on_message_ = std::move(on_message);
    on_close_ = std::move(on_close);
    on_error_ = std::move(on_error);
}

void WebSocketClient::Open(std::string_view url, std::chrono::milliseconds timeout, bool verify_tls) {
    Close();
    closed_.store(false);
    close_notified_.store(false);
    io_thread_ = std::thread(&WebSocketClient::IoLoop, this, std::string(url), timeout, verify_tls);
}

void WebSocketClient::IoLoop(std::string url, std::chrono::milliseconds timeout, bool verify_tls) {
    running_.store(true);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    try {
        if (closed_.load()) return;

        UrlParts parts;
        try {
            parts = ParseUrl(url);
        } catch (const std::exception& e) {
            throw McpError(McpErrorCode::ProtocolViolation,
                           "WebSocket handshake failed: invalid URL: " + url + ": " + e.what());
        }
        if (parts.scheme != "ws" && parts.scheme != "wss")
            throw McpError(McpErrorCode::ProtocolViolation,
                           "WebSocket handshake failed: unsupported scheme: " + parts.scheme);
        if (parts.host.empty() || parts.port == 0)
            throw McpError(McpErrorCode::ProtocolViolation,
                           "WebSocket handshake failed: invalid URL: " + url);

        use_tls_ = (parts.scheme == "wss");
        if (use_tls_) {
            tls_ = std::make_unique<TlsSocket>(verify_tls);
            tls_->Connect(parts.host, parts.port, Remaining(deadline));
        } else {
            tcp_ = std::make_unique<TcpSocket>();
            tcp_->Connect(parts.host, parts.port, Remaining(deadline));
        }

        std::string path = parts.path.empty() ? "/" : parts.path;
        std::string key = RandomBytesBase64(16);

        std::string host_header = parts.host;
        if (parts.host.find(':') != std::string::npos)
            host_header = "[" + parts.host + "]";
        bool default_port = use_tls_ ? (parts.port == 443) : (parts.port == 80);
        if (!default_port)
            host_header += ":" + std::to_string(parts.port);

        std::string request = "GET " + path + " HTTP/1.1\r\n";
        request += "Host: " + host_header + "\r\n";
        request += "Upgrade: websocket\r\n";
        request += "Connection: Upgrade\r\n";
        request += "Sec-WebSocket-Key: " + key + "\r\n";
        request += "Sec-WebSocket-Version: 13\r\n\r\n";
        WriteAll(request, Remaining(deadline));

        std::string status_line = ReadLine(deadline);
        if (status_line.size() < 12 || status_line.compare(0, 9, "HTTP/1.1 ") != 0)
            throw McpError(McpErrorCode::ProtocolViolation,
                           "WebSocket handshake failed: malformed status line");
        if (!std::isdigit(static_cast<unsigned char>(status_line[9])) ||
            !std::isdigit(static_cast<unsigned char>(status_line[10])) ||
            !std::isdigit(static_cast<unsigned char>(status_line[11])))
            throw McpError(McpErrorCode::ProtocolViolation,
                           "WebSocket handshake failed: malformed status code");
        int status_code = (status_line[9] - '0') * 100 + (status_line[10] - '0') * 10 +
                          (status_line[11] - '0');
        if (status_code != 101)
            throw McpError(McpErrorCode::ProtocolViolation,
                           "WebSocket handshake failed: HTTP status " + std::to_string(status_code));

        std::string upgrade_value;
        std::string connection_value;
        std::string accept_value;
        std::size_t header_bytes = 0;
        for (;;) {
            std::string header = ReadLine(deadline);
            header_bytes += header.size() + 2;
            if (header_bytes > kMaxHeaderBytes)
                throw McpError(McpErrorCode::ProtocolViolation,
                               "WebSocket handshake failed: response headers too large");
            if (header.empty()) break;
            std::size_t colon = header.find(':');
            if (colon == std::string::npos)
                throw McpError(McpErrorCode::ProtocolViolation,
                               "WebSocket handshake failed: malformed header line");
            std::string name = ToLower(header.substr(0, colon));
            std::string value = header.substr(colon + 1);
            TrimInPlace(value);
            if (name == "upgrade") upgrade_value = ToLower(value);
            else if (name == "connection") connection_value = ToLower(value);
            else if (name == "sec-websocket-accept") accept_value = value;
        }
        if (upgrade_value != "websocket")
            throw McpError(McpErrorCode::ProtocolViolation,
                           "WebSocket handshake failed: missing Upgrade: websocket");
        if (connection_value.find("upgrade") == std::string::npos)
            throw McpError(McpErrorCode::ProtocolViolation,
                           "WebSocket handshake failed: missing Connection: Upgrade");
        std::string expected_accept = Base64Encode(Sha1Raw(key + std::string(kWebSocketGuid)));
        if (accept_value != expected_accept)
            throw McpError(McpErrorCode::ProtocolViolation,
                           "WebSocket handshake failed: Sec-WebSocket-Accept mismatch");

        std::string payload;
        std::string fragment;
        bool in_fragment = false;
        bool fragment_is_text = false;
        int opcode = 0;
        while (!closed_.load() && !peer_closed_.load()) {
            if (!ReadFrame(payload, opcode)) break;
            switch (opcode) {
            case kOpcodeText:
                if (in_fragment)
                    throw McpError(McpErrorCode::ProtocolViolation,
                                   "WebSocket text frame received during fragmented message");
                if (last_fin_) {
                    if (on_message_) on_message_(payload);
                } else {
                    fragment = payload;
                    fragment_is_text = true;
                    in_fragment = true;
                }
                break;
            case kOpcodeBinary:
                if (in_fragment)
                    throw McpError(McpErrorCode::ProtocolViolation,
                                   "WebSocket binary frame received during fragmented message");
                if (!last_fin_) {
                    fragment.clear();
                    fragment_is_text = false;
                    in_fragment = true;
                }
                break;
            case kOpcodeContinuation:
                if (!in_fragment)
                    throw McpError(McpErrorCode::ProtocolViolation,
                                   "WebSocket continuation frame without fragmented message");
                if (fragment_is_text) {
                    if (fragment.size() > kMaxMessageSize - payload.size())
                        throw McpError(McpErrorCode::ProtocolViolation,
                                       "WebSocket fragmented message exceeds size limit");
                    fragment += payload;
                }
                if (last_fin_) {
                    if (fragment_is_text && on_message_) on_message_(fragment);
                    in_fragment = false;
                    fragment.clear();
                }
                break;
            case kOpcodeClose: {
                peer_closed_.store(true);
                try {
                    SendFrame(kOpcodeClose,
                              std::string_view(payload.data(),
                                               std::min<std::size_t>(2, payload.size())));
                } catch (...) {
                }
                break;
            }
            case kOpcodePing: {
                try {
                    SendFrame(kOpcodePong, payload);
                } catch (...) {
                }
                break;
            }
            case kOpcodePong:
                break;
            default:
                throw McpError(McpErrorCode::ProtocolViolation,
                               "WebSocket unsupported opcode: " + std::to_string(opcode));
            }
        }
    } catch (const std::exception& e) {
        Fail(e.what());
    } catch (...) {
        Fail("unknown WebSocket error");
    }
    if (tls_ != nullptr) tls_->Close();
    if (tcp_ != nullptr) tcp_->Close();
    running_.store(false);
    NotifyClose();
}

bool WebSocketClient::ReadFrame(std::string& payload, int& opcode) {
    uint8_t b0 = 0;
    uint8_t b1 = 0;
    if (!ReadExact(&b0, 1, kFrameIdleTimeout)) return false;
    if (!ReadExact(&b1, 1, kFrameIdleTimeout)) return false;

    last_fin_ = (b0 & 0x80) != 0;
    if ((b0 & 0x70) != 0)
        throw McpError(McpErrorCode::ProtocolViolation, "WebSocket frame has non-zero RSV bits");
    opcode = b0 & 0x0F;
    if ((b1 & 0x80) != 0)
        throw McpError(McpErrorCode::ProtocolViolation,
                       "WebSocket frame from server must not be masked");
    uint64_t length = b1 & 0x7F;
    if (length == 126) {
        uint8_t ext[2];
        if (!ReadExact(ext, 2, kFrameIdleTimeout)) return false;
        length = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (length == 127) {
        uint8_t ext[8];
        if (!ReadExact(ext, 8, kFrameIdleTimeout)) return false;
        if ((ext[0] & 0x80) != 0)
            throw McpError(McpErrorCode::ProtocolViolation,
                           "WebSocket frame length exceeds 63 bits");
        length = 0;
        for (int i = 0; i < 8; ++i)
            length = (length << 8) | ext[i];
    }
    if (length > kMaxMessageSize)
        throw McpError(McpErrorCode::ProtocolViolation, "WebSocket frame exceeds size limit");

    bool control = (opcode & 0x08) != 0;
    if (control) {
        if (!last_fin_)
            throw McpError(McpErrorCode::ProtocolViolation,
                           "WebSocket control frame must not be fragmented");
        if (length > 125)
            throw McpError(McpErrorCode::ProtocolViolation,
                           "WebSocket control frame payload too large");
    }

    payload.resize(static_cast<std::size_t>(length));
    if (length > 0 && !ReadExact(payload.data(), payload.size(), kFrameIdleTimeout)) return false;
    return true;
}

void WebSocketClient::SendFrame(int opcode, std::string_view payload) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    WriteFrameLocked(opcode, payload);
}

bool WebSocketClient::IsConnected() const {
    if (use_tls_)
        return tls_ != nullptr && tls_->IsConnected();
    return tcp_ != nullptr && tcp_->IsConnected();
}

bool WebSocketClient::IsEof() const {
    if (use_tls_)
        return tls_ == nullptr || tls_->IsEof();
    return tcp_ == nullptr || tcp_->IsEof();
}

bool WebSocketClient::ReadExact(void* buf, std::size_t len, std::chrono::milliseconds timeout) {
    if (len == 0) return true;
    char* p = static_cast<char*>(buf);
    std::size_t got = 0;
    while (got < len) {
        std::size_t n = use_tls_ ? tls_->Read(p + got, len - got, timeout)
                                 : tcp_->Read(p + got, len - got, timeout);
        if (n > 0) {
            got += n;
            continue;
        }
        if (IsEof()) return false;
        throw McpError(McpErrorCode::RequestTimeout, "WebSocket read timed out");
    }
    return true;
}

int WebSocketClient::ReadByte(const std::chrono::steady_clock::time_point& deadline) {
    auto remaining = Remaining(deadline);
    if (remaining.count() <= 0)
        throw McpError(McpErrorCode::RequestTimeout, "WebSocket handshake timed out");
    char byte = 0;
    if (ReadExact(&byte, 1, remaining)) return static_cast<unsigned char>(byte);
    throw McpError(McpErrorCode::ConnectionClosed, "connection closed during WebSocket handshake");
}

std::string WebSocketClient::ReadLine(const std::chrono::steady_clock::time_point& deadline) {
    std::string line;
    line.reserve(128);
    for (;;) {
        int c = ReadByte(deadline);
        if (c == '\n') break;
        if (c == '\r') {
            int next = ReadByte(deadline);
            if (next != '\n')
                throw McpError(McpErrorCode::ProtocolViolation,
                               "WebSocket handshake failed: malformed line ending");
            break;
        }
        if (line.size() >= kMaxLineBytes)
            throw McpError(McpErrorCode::ProtocolViolation,
                           "WebSocket handshake failed: line too long");
        line.push_back(static_cast<char>(c));
    }
    return line;
}

void WebSocketClient::WriteAll(std::string_view data, std::chrono::milliseconds timeout) {
    if (use_tls_) tls_->Write(data.data(), data.size(), timeout);
    else tcp_->Write(data.data(), data.size(), timeout);
}

void WebSocketClient::WriteFrameLocked(int opcode, std::string_view payload) {
    if (!IsConnected()) return;

    uint8_t mask_key[4];
    RandomBytes(mask_key, 4);

    std::string frame;
    frame.reserve(payload.size() + 14);
    frame.push_back(static_cast<char>(0x80 | opcode));
    std::size_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(0x80 | len));
    } else if (len <= 0xFFFF) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
    }
    frame.append(reinterpret_cast<const char*>(mask_key), 4);
    for (std::size_t i = 0; i < len; ++i)
        frame.push_back(static_cast<char>(payload[i] ^ mask_key[i % 4]));

    WriteAll(frame, kSendTimeout);
}

void WebSocketClient::Send(std::string_view text) {
    if (!running_.load() || closed_.load()) return;
    try {
        SendFrame(kOpcodeText, text);
    } catch (...) {
    }
}

void WebSocketClient::Close() {
    if (closed_.exchange(true)) return;
    bool was_open = io_thread_.joinable();
    if (was_open) {
        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            if (!peer_closed_.load() && IsConnected()) {
                try {
                    WriteFrameLocked(kOpcodeClose, std::string_view());
                } catch (...) {
                }
            }
        }
        if (tls_ != nullptr) tls_->Close();
        if (tcp_ != nullptr) tcp_->Close();
        JoinThreadSafely(io_thread_);
        NotifyClose();
    }
}

void WebSocketClient::NotifyClose() {
    bool expected = false;
    if (!close_notified_.compare_exchange_strong(expected, true)) return;
    if (on_close_) on_close_();
}

void WebSocketClient::Fail(std::string_view message) {
    if (!closed_.load()) {
        if (on_error_) on_error_(message);
    }
    NotifyClose();
}

}}}
