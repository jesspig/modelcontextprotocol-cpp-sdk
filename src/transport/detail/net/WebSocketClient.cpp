#include <transport/detail/net/WebSocketClient.hpp>
#include <transport/detail/net/NetIoUtil.hpp>
#include <transport/detail/net/Sha1.hpp>
#include <transport/detail/net/TlsSocket.hpp>
#include <mcp/detail/ThreadUtils.hpp>
#include <mcp/detail/WebSocketFrameParser.hpp>
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
#include <vector>

namespace mcp { namespace detail { namespace net {

namespace {

constexpr std::chrono::milliseconds kFrameIdleTimeout(24 * 60 * 60 * 1000);
constexpr std::chrono::milliseconds kSendTimeout(30000);
constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr uint8_t kOpcodeContinuation = 0x0;
constexpr uint8_t kOpcodeText = 0x1;
constexpr uint8_t kOpcodeBinary = 0x2;
constexpr uint8_t kOpcodeClose = 0x8;
constexpr uint8_t kOpcodePing = 0x9;
constexpr uint8_t kOpcodePong = 0xA;

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

void WebSocketClient::SetOpenCallback(std::function<void()> on_open) {
    on_open_ = std::move(on_open);
}

WebSocketClient::HandshakeState WebSocketClient::State() const {
    return state_.load();
}

void WebSocketClient::Open(std::string_view url, std::chrono::milliseconds timeout, bool verify_tls) {
    Close();
    closed_.store(false);
    close_notified_.store(false);
    peer_closed_.store(false);
    last_fin_ = true;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        pending_.clear();
        pending_bytes_ = 0;
    }
    state_.store(HandshakeState::Connecting);
    io_thread_ = std::thread(&WebSocketClient::IoLoop, this, std::string(url), timeout, verify_tls);
}

void WebSocketClient::IoLoop(std::string url, std::chrono::milliseconds timeout, bool verify_tls) {
    running_.store(true);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string current_url = std::move(url);
    int redirects = 0;
    try {
        for (;;) {
            if (closed_.load() || state_.load() != HandshakeState::Connecting) return;
            if (Remaining(deadline).count() <= 0)
                throw McpError(McpErrorCode::RequestTimeout, "WebSocket handshake timed out");

            HandshakeResult result;
            if (PerformHandshake(current_url, deadline, verify_tls, result)) break;

            if (!IsRedirectStatus(result.status_code) || result.location.empty())
                throw McpError(McpErrorCode::ProtocolViolation,
                               "WebSocket handshake failed: HTTP status " +
                                   std::to_string(result.status_code));
            if (++redirects > kMaxWebSocketRedirects)
                throw McpError(McpErrorCode::ProtocolViolation,
                               "WebSocket handshake failed: too many redirects");

            std::string next;
            try {
                next = ResolveRedirectUrl(current_url, result.location);
            } catch (const std::exception& e) {
                throw McpError(McpErrorCode::ProtocolViolation,
                               "WebSocket handshake failed: malformed Location: " +
                                   std::string(e.what()));
            }
            if (!IsSafeWebSocketRedirect(current_url, next))
                throw McpError(McpErrorCode::ProtocolViolation,
                               "WebSocket handshake failed: unsafe redirect");
            CloseSockets(true);
            current_url = std::move(next);
        }

        CompleteOpen();
        std::string payload;
        std::string fragment;
        bool in_fragment = false;
        bool fragment_is_text = false;
        int opcode = 0;
        while (!closed_.load() && !peer_closed_.load() &&
               state_.load() == HandshakeState::Open) {
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
    CloseSockets(true);
    running_.store(false);
    NotifyClose();
}

bool WebSocketClient::PerformHandshake(const std::string& url,
                                       const std::chrono::steady_clock::time_point& deadline,
                                       bool verify_tls, HandshakeResult& result) {
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

    bool tls = parts.scheme == "wss";
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (closed_.load() || state_.load() != HandshakeState::Connecting)
            throw McpError(McpErrorCode::ConnectionClosed, "WebSocket closed during handshake");
        if (tls) {
            tls_ = std::make_unique<TlsSocket>(verify_tls);
        } else {
            tcp_ = std::make_unique<TcpSocket>();
        }
        use_tls_.store(tls);
    }
    if (tls) tls_->Connect(parts.host, parts.port, Remaining(deadline));
    else tcp_->Connect(parts.host, parts.port, Remaining(deadline));
    if (closed_.load())
        throw McpError(McpErrorCode::ConnectionClosed, "WebSocket closed during handshake");

    std::string path = parts.path.empty() ? "/" : parts.path;
    std::string key = RandomBytesBase64(16);
    result.key = key;

    std::string host_header = parts.host;
    if (parts.host.find(':') != std::string::npos)
        host_header = "[" + parts.host + "]";
    bool default_port = tls ? (parts.port == 443) : (parts.port == 80);
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
    result.status_code = status_code;

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
        else if (name == "location") result.location = value;
    }
    if (status_code != 101) return false;

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
    return true;
}

void WebSocketClient::CompleteOpen() {
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (closed_.load() || state_.load() != HandshakeState::Connecting) return;
        state_.store(HandshakeState::Open);
    }
    if (on_open_) {
        try {
            on_open_();
        } catch (...) {
        }
    }
    FlushQueuedMessages();
}

void WebSocketClient::FlushQueuedMessages() {
    std::vector<std::string> dropped;
    bool broken = false;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        while (!pending_.empty()) {
            std::string message = std::move(pending_.front());
            pending_.pop_front();
            pending_bytes_ -= message.size();
            if (broken || closed_.load() || !IsConnected()) {
                broken = true;
                dropped.push_back(std::move(message));
                continue;
            }
            try {
                WriteFrameLocked(kOpcodeText, message);
            } catch (...) {
                broken = true;
                dropped.push_back(std::move(message));
            }
        }
    }
    if (!dropped.empty())
        ReportDroppedMessages(std::move(dropped), "WebSocket message dropped before delivery");
    if (broken)
        throw McpError(McpErrorCode::ConnectionClosed, "WebSocket send failed after handshake");
}

void WebSocketClient::ReportDroppedMessages(std::vector<std::string> messages,
                                            std::string_view reason) {
    if (!on_error_) return;
    for (const auto& message : messages) {
        std::string detail(reason);
        detail += " (payload bytes=" + std::to_string(message.size()) + ")";
        on_error_(detail);
    }
}

bool WebSocketClient::ReadFrame(std::string& payload, int& opcode) {
    uint8_t b0 = 0;
    uint8_t b1 = 0;
    if (!ReadExact(&b0, 1, kFrameIdleTimeout)) return false;
    if (!ReadExact(&b1, 1, kFrameIdleTimeout)) return false;

    std::string header;
    header.reserve(10);
    header.push_back(static_cast<char>(b0));
    header.push_back(static_cast<char>(b1));
    const uint8_t length_byte = b1 & 0x7F;
    if (length_byte == 126) {
        uint8_t ext[2];
        if (!ReadExact(ext, 2, kFrameIdleTimeout)) return false;
        header.push_back(static_cast<char>(ext[0]));
        header.push_back(static_cast<char>(ext[1]));
    } else if (length_byte == 127) {
        uint8_t ext[8];
        if (!ReadExact(ext, 8, kFrameIdleTimeout)) return false;
        for (int index = 0; index < 8; ++index)
            header.push_back(static_cast<char>(ext[index]));
    }

    const WebSocketFrameHeader parsed = ParseWebSocketFrameHeader(header);
    last_fin_ = parsed.fin;
    opcode = parsed.opcode;
    const uint64_t length = parsed.length;

    payload.resize(static_cast<std::size_t>(length));
    if (length > 0 && !ReadExact(payload.data(), payload.size(), kFrameIdleTimeout)) return false;
    return true;
}

void WebSocketClient::SendFrame(int opcode, std::string_view payload) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    WriteFrameLocked(opcode, payload);
}

bool WebSocketClient::IsConnected() const {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (use_tls_.load())
        return tls_ != nullptr && tls_->IsConnected();
    return tcp_ != nullptr && tcp_->IsConnected();
}

bool WebSocketClient::IsEof() const {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (use_tls_.load())
        return tls_ == nullptr || tls_->IsEof();
    return tcp_ == nullptr || tcp_->IsEof();
}

bool WebSocketClient::ReadExact(void* buf, std::size_t len, std::chrono::milliseconds timeout) {
    if (len == 0) return true;
    char* p = static_cast<char*>(buf);
    std::size_t got = 0;
    while (got < len) {
        void* socket = nullptr;
        bool tls = false;
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            tls = use_tls_.load();
            socket = tls ? static_cast<void*>(tls_.get()) : static_cast<void*>(tcp_.get());
        }
        if (socket == nullptr)
            throw McpError(McpErrorCode::ConnectionClosed, "read on unconnected WebSocket");
        std::size_t n = tls ? static_cast<TlsSocket*>(socket)->Read(p + got, len - got, timeout)
                            : static_cast<TcpSocket*>(socket)->Read(p + got, len - got, timeout);
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
    if (timeout.count() <= 0)
        throw McpError(McpErrorCode::RequestTimeout, "WebSocket handshake timed out");
    void* socket = nullptr;
    bool tls = false;
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        tls = use_tls_.load();
        socket = tls ? static_cast<void*>(tls_.get()) : static_cast<void*>(tcp_.get());
    }
    if (socket == nullptr)
        throw McpError(McpErrorCode::ConnectionClosed, "write on unconnected WebSocket");
    if (tls) static_cast<TlsSocket*>(socket)->Write(data.data(), data.size(), timeout);
    else static_cast<TcpSocket*>(socket)->Write(data.data(), data.size(), timeout);
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
    std::string error;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        auto state = state_.load();
        if (state == HandshakeState::Connecting) {
            if (pending_.size() >= kMaxWebSocketHandshakeQueueMessages ||
                text.size() > kMaxWebSocketHandshakeQueueBytes - pending_bytes_) {
                error = "WebSocket handshake queue full: message dropped (payload bytes=" +
                        std::to_string(text.size()) + ")";
            } else {
                pending_.emplace_back(text);
                pending_bytes_ += text.size();
            }
        } else if (state == HandshakeState::Open) {
            if (!IsConnected()) {
                error = "WebSocket send failed: connection closed (payload bytes=" +
                        std::to_string(text.size()) + ")";
            } else {
                try {
                    WriteFrameLocked(kOpcodeText, text);
                } catch (const std::exception& e) {
                    error = std::string("WebSocket send failed: ") + e.what();
                }
            }
        }
    }
    if (!error.empty() && on_error_) on_error_(error);
}

void WebSocketClient::CloseSockets(bool reset) {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (tls_ != nullptr) {
        tls_->Close();
        if (reset) tls_.reset();
    }
    if (tcp_ != nullptr) {
        tcp_->Close();
        if (reset) tcp_.reset();
    }
}

void WebSocketClient::Terminate() {
    HandshakeState current = state_.load();
    while (current != HandshakeState::Closed) {
        if (state_.compare_exchange_weak(current, HandshakeState::Closed)) return;
    }
}

void WebSocketClient::Close() {
    if (closed_.exchange(true)) return;
    if (!io_thread_.joinable()) return;
    HandshakeState previous = state_.load();
    HandshakeState current = previous;
    while (current != HandshakeState::Closed && current != HandshakeState::Closing) {
        if (state_.compare_exchange_weak(current, HandshakeState::Closing)) break;
    }
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (!peer_closed_.load() && previous == HandshakeState::Open && IsConnected()) {
            try {
                WriteFrameLocked(kOpcodeClose, std::string_view());
            } catch (...) {
            }
        }
    }
    CloseSockets(false);
    JoinThreadSafely(io_thread_);
    NotifyClose();
}

void WebSocketClient::NotifyClose() {
    bool expected = false;
    if (!close_notified_.compare_exchange_strong(expected, true)) return;
    Terminate();
    if (on_close_) on_close_();
}

void WebSocketClient::Fail(std::string_view message) {
    std::vector<std::string> dropped;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        while (!pending_.empty()) {
            dropped.push_back(std::move(pending_.front()));
            pending_.pop_front();
            pending_bytes_ -= dropped.back().size();
        }
    }
    ReportDroppedMessages(std::move(dropped), "WebSocket message dropped before handshake completed");
    if (!closed_.load() && on_error_) on_error_(message);
    NotifyClose();
}

}}}