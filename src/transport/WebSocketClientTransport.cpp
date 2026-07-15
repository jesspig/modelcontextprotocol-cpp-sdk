#include <mcp/transport/WebSocketClientTransport.hpp>
#include <mcp/JsonRpc.hpp>

#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#ifdef MCP_HAVE_OPENSSL
#include <asio/ssl.hpp>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#ifdef MCP_HAVE_OPENSSL
#include <openssl/sha.h>
#endif

namespace mcp {

// JSON parse safety limits
#define K_MAX_MESSAGE_SIZE (8 * 1024 * 1024)  // 8MB
// K_MAX_JSON_DEPTH removed — nlohmann-json v3.11.3 parse() accepts 4 args max

namespace {

// ═══════════════════════════════════════════════════════════════════════
// WebSocket protocol helpers (RFC 6455)
// ═══════════════════════════════════════════════════════════════════════

const char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::vector<uint8_t>& data) {
    std::string result;
    result.reserve(((data.size() + 2) / 3) * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t val = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size())
            val |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size())
            val |= static_cast<uint32_t>(data[i + 2]);
        result += kBase64[(val >> 18) & 0x3F];
        result += kBase64[(val >> 12) & 0x3F];
        result += (i + 1 < data.size()) ? kBase64[(val >> 6) & 0x3F] : '=';
        result += (i + 2 < data.size()) ? kBase64[val & 0x3F] : '=';
    }
    return result;
}

std::vector<uint8_t> GenerateKey() {
    std::vector<uint8_t> key(16);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(0, 255);
    for (auto& b : key)
        b = static_cast<uint8_t>(dis(gen));
    return key;
}

#ifdef MCP_HAVE_OPENSSL
// Compute SHA-1 hash for Sec-WebSocket-Accept validation
std::vector<uint8_t> Sha1(const std::string& data) {
    std::vector<uint8_t> hash(SHA_DIGEST_LENGTH);
    SHA1(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash.data());
    return hash;
}

// Validate Sec-WebSocket-Accept header value
bool ValidateWebSocketAccept(const std::string& client_key_b64, const std::string& accept_header) {
    std::string concat = client_key_b64 + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    auto hash = Sha1(concat);
    std::string expected = Base64Encode(hash);
    return accept_header == expected;
}
#endif

// WebSocket opcodes
enum WSOpcode : uint8_t {
    kOpcodeContinuation = 0x0,
    kOpcodeText         = 0x1,
    kOpcodeBinary       = 0x2,
    kOpcodeClose        = 0x8,
    kOpcodePing         = 0x9,
    kOpcodePong         = 0xA,
};

struct WSFrame {
    bool fin;
    uint8_t opcode;
    std::vector<uint8_t> payload;
};

// Read exactly n bytes from socket (blocking)
void ReadAll(asio::ip::tcp::socket& socket, uint8_t* buf, size_t n) {
    size_t offset = 0;
    while (offset < n) {
        size_t read = socket.read_some(asio::buffer(buf + offset, n - offset));
        offset += read;
    }
}

// Read one WebSocket frame from socket
WSFrame ReadFrame(asio::ip::tcp::socket& socket) {
    uint8_t hdr[2];
    ReadAll(socket, hdr, 2);

    bool fin = (hdr[0] & 0x80) != 0;
    uint8_t opcode = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t payload_len = hdr[1] & 0x7F;

    if (payload_len == 126) {
        uint8_t ext[2];
        ReadAll(socket, ext, 2);
        payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        ReadAll(socket, ext, 8);
        payload_len = 0;
        for (int i = 0; i < 8; ++i)
            payload_len = (payload_len << 8) | ext[i];
    }

    std::vector<uint8_t> mask_key;
    if (masked) {
        mask_key.resize(4);
        ReadAll(socket, mask_key.data(), 4);
    }

    std::vector<uint8_t> payload(static_cast<size_t>(payload_len));
    if (payload_len > 0)
        ReadAll(socket, payload.data(), static_cast<size_t>(payload_len));

    // Unmask if needed
    if (masked && !payload.empty()) {
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] ^= mask_key[i % 4];
    }

    return WSFrame{fin, opcode, std::move(payload)};
}

// Write a WebSocket frame to socket (client-to-server, always masked)
void WriteFrame(asio::ip::tcp::socket& socket, uint8_t opcode,
                const std::string& data) {
    std::vector<uint8_t> frame;

    // FIN + opcode
    frame.push_back(static_cast<uint8_t>(0x80 | opcode));

    // Masked payload length
    uint64_t len = data.size();
    if (len <= 125) {
        frame.push_back(static_cast<uint8_t>(0x80 | len));
    } else if (len <= 0xFFFF) {
        frame.push_back(static_cast<uint8_t>(0x80 | 126));
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(static_cast<uint8_t>(0x80 | 127));
        for (int i = 7; i >= 0; --i)
            frame.push_back(
                static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
    }

    // Generate masking key
    std::array<uint8_t, 4> mask_key;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(0, 255);
    for (auto& b : mask_key)
        b = static_cast<uint8_t>(dis(gen));

    frame.insert(frame.end(), mask_key.begin(), mask_key.end());

    // Mask and append payload
    size_t payload_start = frame.size();
    frame.resize(frame.size() + static_cast<size_t>(len));
    for (size_t i = 0; i < static_cast<size_t>(len); ++i)
        frame[payload_start + i] =
            static_cast<uint8_t>(data[i]) ^ mask_key[i % 4];

    asio::write(socket, asio::buffer(frame));
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// WebSocketTransport
// ═══════════════════════════════════════════════════════════════════════

WebSocketTransport::WebSocketTransport(
    std::shared_ptr<asio::io_context> io_ctx,
    asio::ip::tcp::socket socket)
    : TransportBase(*io_ctx)
    , io_ctx_(std::move(io_ctx))
    , socket_(std::move(socket))
    , last_pong_time_(std::chrono::steady_clock::now()) {
    channel_ = std::make_unique<MessageChannel>(*io_ctx_, 64);
}

WebSocketTransport::~WebSocketTransport() { Close(); }

void WebSocketTransport::Start() {
    if (running_.exchange(true))
        return;

    SetConnected();
    read_thread_ = std::thread([this] { ReadLoop(); });
    write_thread_ = std::thread([this] { WriteLoop(); });
    io_thread_ = std::thread([this] { io_ctx_->run(); });
}

void WebSocketTransport::Close() {
    if (!running_.exchange(false))
        return;

    // Wake write thread
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        write_cv_.notify_one();
    }

    // Join write thread first — ensures no concurrent access to socket_
    if (write_thread_.joinable())
        write_thread_.join();

    // Close socket — unblocks any pending reads in ReadLoop
    asio::error_code ec;
    socket_.close(ec);

    // Join read thread (unblocked by socket close or saw running_=false)
    if (read_thread_.joinable())
        read_thread_.join();

    io_ctx_->stop();
    if (io_thread_.joinable())
        io_thread_.join();

    if (channel_)
        channel_->Close();
    SetDisconnected();
}

void WebSocketTransport::SendMessageAsync(JsonRpcMessage message) {
    if (!running_)
        return;

    nlohmann::json j = message;
    std::string body = j.dump();

    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        write_queue_.push(std::move(body));
    }
    write_cv_.notify_one();
}

// ── Read loop ──

void WebSocketTransport::ReadLoop() {
    try {
        while (running_) {
            auto frame = ReadFrame(socket_);

            if (!running_)
                break;

            switch (frame.opcode) {
            case kOpcodeText: {
                std::string text(frame.payload.begin(), frame.payload.end());
                if (text.size() > K_MAX_MESSAGE_SIZE) {
                    NotifyError("message size exceeds maximum allowed size");
                    break;
                }
                try {
                    auto j = nlohmann::json::parse(text, nullptr, false, false);
                    auto parsed = j.get<JsonRpcMessage>();
                    asio::post(*io_ctx_,
                               [this, parsed]() mutable {
                                   if (!running_)
                                       return;
                                   if (channel_)
                                       channel_->Send(std::move(parsed));
                               });
                } catch (const std::exception& e) {
                    NotifyError(std::string("WebSocket parse error: ") +
                                e.what());
                }
                break;
            }
            case kOpcodeClose:
                // Echo close
                try {
                    WriteFrame(socket_, kOpcodeClose, "");
                } catch (...) {
                }
                running_ = false;
                {
                    std::lock_guard<std::mutex> lk(write_mutex_);
                    write_cv_.notify_one();
                }
                if (channel_)
                    channel_->Close();
                SetDisconnected();
                break;

            case kOpcodePing:
                try {
                    WriteFrame(socket_, kOpcodePong,
                               std::string(frame.payload.begin(),
                                           frame.payload.end()));
                } catch (...) {
                }
                break;

            case kOpcodePong:
                last_pong_time_ = std::chrono::steady_clock::now();
                break;

            default:
                break;
            }
        }
    } catch (const std::exception& e) {
        if (running_)
            NotifyError(std::string("WebSocket read error: ") + e.what());
    }

    // Connection lost — clean up unless we initiated the close
    if (running_.exchange(false)) {
        {
            std::lock_guard<std::mutex> lk(write_mutex_);
            write_cv_.notify_one();
        }
        if (channel_)
            channel_->Close();
        SetDisconnected();
    }
}

// ── Write loop ──

void WebSocketTransport::WriteLoop() {
    auto last_ping_time = std::chrono::steady_clock::now();
    while (running_) {
        std::string body;
        bool has_message = false;
        {
            std::unique_lock<std::mutex> lock(write_mutex_);
            write_cv_.wait_for(lock, std::chrono::seconds(15), [this] {
                return !write_queue_.empty() || !running_;
            });
            if (!running_)
                break;
            if (!write_queue_.empty()) {
                body = std::move(write_queue_.front());
                write_queue_.pop();
                has_message = true;
            }
        }

        if (has_message) {
            try {
                WriteFrame(socket_, kOpcodeText, body);
            } catch (const std::exception& e) {
                if (running_) {
                    NotifyError(std::string("WebSocket write error: ") + e.what());
                    break;
                }
            }
        } else {
            // Send PING to keep connection alive
            auto now = std::chrono::steady_clock::now();
            if (now - last_ping_time > std::chrono::seconds(30)) {
                try {
                    WriteFrame(socket_, kOpcodePing, "");
                } catch (...) {
                }
                last_ping_time = now;
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// WebSocketClientTransport
// ═══════════════════════════════════════════════════════════════════════

WebSocketClientTransport::WebSocketClientTransport(
    asio::io_context& io_ctx, std::string host, std::string port,
    std::string path, std::string name, bool use_tls)
    : io_ctx_(io_ctx)
    , host_(std::move(host))
    , port_(std::move(port))
    , path_(std::move(path))
    , name_(std::move(name))
    , use_tls_(use_tls) {}

WebSocketClientTransport::~WebSocketClientTransport() = default;

std::string_view WebSocketClientTransport::Name() const { return name_; }

std::shared_ptr<ITransport> WebSocketClientTransport::Connect() {
    // Resolve hostname
    asio::ip::tcp::resolver resolver(io_ctx_);
    asio::error_code ec;
    auto endpoints = resolver.resolve(host_, port_, ec);
    if (ec)
        throw std::runtime_error("WebSocketClient: resolve failed (" +
                                 host_ + ":" + port_ + "): " + ec.message());

    asio::ip::tcp::socket socket(io_ctx_);

    // TLS handshake if needed
#ifdef MCP_HAVE_OPENSSL
    std::unique_ptr<asio::ssl::context> ssl_ctx;
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> ssl_stream;
    if (use_tls_) {
        ssl_ctx = std::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);
        ssl_ctx->set_default_verify_paths();
        ssl_stream = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(socket, *ssl_ctx);
        asio::connect(ssl_stream->lowest_layer(), endpoints, ec);
        if (ec)
            throw std::runtime_error("WebSocketClient: TLS connect failed: " + ec.message());
        ssl_stream->handshake(asio::ssl::stream_base::client, ec);
        if (ec)
            throw std::runtime_error("WebSocketClient: TLS handshake failed: " + ec.message());
    } else
#endif
    {
        asio::connect(socket, endpoints, ec);
        if (ec)
            throw std::runtime_error("WebSocketClient: connect failed (" +
                                     host_ + ":" + port_ + "): " + ec.message());
    }

    // ── WebSocket handshake ──
    auto key = GenerateKey();
    std::string key_b64 = Base64Encode(key);

    std::string request = "GET " + path_ + " HTTP/1.1\r\n"
                          "Host: " + host_ + ":" + port_ + "\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Key: " + key_b64 + "\r\n"
                          "Sec-WebSocket-Version: 13\r\n"
                          "\r\n";

#ifdef MCP_HAVE_OPENSSL
    if (use_tls_) {
        asio::write(*ssl_stream, asio::buffer(request), ec);
    } else
#endif
    {
        asio::write(socket, asio::buffer(request), ec);
    }
    if (ec)
        throw std::runtime_error("WebSocketClient: handshake write failed: " +
                                 ec.message());

    // Read response headers
    std::string response;
    auto read_byte = [&](char& c) -> bool {
#ifdef MCP_HAVE_OPENSSL
        if (use_tls_) {
            size_t n = ssl_stream->read_some(asio::buffer(&c, 1), ec);
            return !ec && n > 0;
        } else
#endif
        {
            size_t n = socket.read_some(asio::buffer(&c, 1), ec);
            return !ec && n > 0;
        }
    };

    while (response.find("\r\n\r\n") == std::string::npos) {
        char c = 0;
        if (!read_byte(c))
            throw std::runtime_error(
                "WebSocketClient: handshake read failed: " + ec.message());
        response += c;
    }

    if (response.find("101") == std::string::npos) {
        auto crlf = response.find("\r\n");
        std::string status_line =
            (crlf != std::string::npos) ? response.substr(0, crlf) : response;
        throw std::runtime_error(
            "WebSocketClient: handshake failed (expected 101): " +
            status_line);
    }

#ifdef MCP_HAVE_OPENSSL
    // Validate Sec-WebSocket-Accept header
    auto accept_pos = response.find("Sec-WebSocket-Accept:");
    if (accept_pos != std::string::npos) {
        auto val_start = response.find_first_not_of(" \t", accept_pos + 22);
        auto val_end = response.find("\r\n", val_start);
        std::string accept_val = (val_start != std::string::npos && val_end != std::string::npos)
            ? response.substr(val_start, val_end - val_start) : "";
        // Trim trailing whitespace
        while (!accept_val.empty() && (accept_val.back() == ' ' || accept_val.back() == '\r'))
            accept_val.pop_back();
        if (!ValidateWebSocketAccept(key_b64, accept_val)) {
            throw std::runtime_error("WebSocketClient: Sec-WebSocket-Accept validation failed");
        }
    }
#endif

    // Create session transport with connected socket
    auto session_io = std::make_shared<asio::io_context>();
    auto session = std::make_shared<WebSocketTransport>(
        std::move(session_io), std::move(socket));

    session->Start();
    return session;
}

} // namespace mcp
