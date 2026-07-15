#include <mcp/transport/WebSocketClientTransport.hpp>
#include <mcp/JsonRpc.hpp>

#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

namespace mcp {

// JSON parse safety limits
#define K_MAX_MESSAGE_SIZE (8 * 1024 * 1024)  // 8MB
#define K_MAX_JSON_DEPTH 32

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
    , socket_(std::move(socket)) {
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

    // Close socket — unblocks any pending reads/writes
    asio::error_code ec;
    socket_.close(ec);

    // Join threads
    if (write_thread_.joinable())
        write_thread_.join();
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
                    auto j = nlohmann::json::parse(text, nullptr, false, K_MAX_JSON_DEPTH);
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
                // Ignored
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
    while (running_) {
        std::string body;
        {
            std::unique_lock<std::mutex> lock(write_mutex_);
            write_cv_.wait(lock, [this] {
                return !write_queue_.empty() || !running_;
            });
            if (!running_ || write_queue_.empty())
                continue;
            body = std::move(write_queue_.front());
            write_queue_.pop();
        }

        try {
            WriteFrame(socket_, kOpcodeText, body);
        } catch (const std::exception& e) {
            if (running_) {
                NotifyError(std::string("WebSocket write error: ") + e.what());
                break;
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// WebSocketClientTransport
// ═══════════════════════════════════════════════════════════════════════

WebSocketClientTransport::WebSocketClientTransport(
    asio::io_context& io_ctx, std::string host, std::string port,
    std::string path, std::string name)
    : io_ctx_(io_ctx)
    , host_(std::move(host))
    , port_(std::move(port))
    , path_(std::move(path))
    , name_(std::move(name)) {}

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

    // TCP connect
    asio::ip::tcp::socket socket(io_ctx_);
    asio::connect(socket, endpoints, ec);
    if (ec)
        throw std::runtime_error("WebSocketClient: connect failed (" +
                                 host_ + ":" + port_ + "): " + ec.message());

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

    asio::write(socket, asio::buffer(request), ec);
    if (ec)
        throw std::runtime_error("WebSocketClient: handshake write failed: " +
                                 ec.message());

    // Read response headers
    std::string response;
    while (response.find("\r\n\r\n") == std::string::npos) {
        char c = 0;
        size_t n = socket.read_some(asio::buffer(&c, 1), ec);
        if (ec || n == 0)
            throw std::runtime_error(
                "WebSocketClient: handshake read failed: " + ec.message());
        response += c;
    }

    if (response.find("101") == std::string::npos) {
        // Extract status line for error message
        auto crlf = response.find("\r\n");
        std::string status_line =
            (crlf != std::string::npos) ? response.substr(0, crlf) : response;
        throw std::runtime_error(
            "WebSocketClient: handshake failed (expected 101): " +
            status_line);
    }

    // Create session transport with connected socket
    auto session_io = std::make_shared<asio::io_context>();
    auto session = std::make_shared<WebSocketTransport>(
        std::move(session_io), std::move(socket));

    session->Start();
    return session;
}

} // namespace mcp
