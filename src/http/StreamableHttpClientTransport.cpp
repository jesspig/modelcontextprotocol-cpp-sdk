#include <mcp/transport/StreamableHttpClientTransport.hpp>
#include <mcp/transport/detail/PlatformIO.hpp>
#include <mcp/transport/detail/Url.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/Log.hpp>

#include <asio/experimental/channel.hpp>
#include <asio/post.hpp>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <winhttp.h>
#include <windows.h>
#else
#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#endif

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#pragma comment(lib, "winhttp.lib")
#endif

namespace mcp {

// JSON parse safety limits
#define K_MAX_MESSAGE_SIZE (8 * 1024 * 1024)  // 8MB

// ═══════════════════════════════════════════════════════════════════════
// Win32 implementation (WinHTTP)
// ═══════════════════════════════════════════════════════════════════════
#ifdef _WIN32
namespace {

std::wstring ToWideStr(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                  static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(),
                        static_cast<int>(s.size()), &w[0], len);
    return w;
}

// ── Win32 StreamableHttpSessionTransport ──
class StreamableHttpSessionTransport : public TransportBase {
public:
    StreamableHttpSessionTransport(
        std::shared_ptr<asio::io_context> io_ctx,
        HttpClientTransportOptions options)
        : TransportBase(*io_ctx)
        , io_ctx_(std::move(io_ctx))
        , options_(std::move(options))
    {
        channel_ = std::make_unique<MessageChannel>(*io_ctx_, 64);
    }

    ~StreamableHttpSessionTransport() override { Close(); }

    void Start() {
        if (running_.exchange(true)) return;
        send_thread_ = std::thread([this] {
            detail::SetThreadName("mcp-worker");
            SendLoop();
        });
        io_thread_ = std::thread([this]() {
            detail::SetThreadName("mcp-worker");
            io_ctx_->run();
        });
    }

    void Close() override {
        if (!running_.exchange(false)) return;
        {
            std::lock_guard<std::mutex> lk(send_mutex_);
            send_cv_.notify_one();
        }
        if (sse_request_) {
            WinHttpCloseHandle(sse_request_);
            sse_request_ = nullptr;
        }
        if (sse_thread_.joinable()) sse_thread_.join();
        if (send_thread_.joinable()) send_thread_.join();
        io_ctx_->stop();
        if (io_thread_.joinable()) io_thread_.join();
        if (channel_) channel_->Close();
        SetDisconnected();
    }

    void SendMessageAsync(JsonRpcMessage message) override {
        if (!running_) return;
        nlohmann::json j = message;
        {
            std::lock_guard<std::mutex> lk(send_mutex_);
            send_queue_.push(j.dump());
        }
        send_cv_.notify_one();
    }

private:
    void SendLoop() {
        while (running_) {
            std::string body;
            {
                std::unique_lock<std::mutex> lk(send_mutex_);
                send_cv_.wait(lk, [this] {
                    return !send_queue_.empty() || !running_;
                });
                if (!running_) break;
                body = std::move(send_queue_.front());
                send_queue_.pop();
            }
            DoPost(body);
        }
    }

    void DoPost(const std::string& body) {
        HINTERNET hSession = WinHttpOpen(L"MCP-HTTP-Client/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return;

        auto url = detail::ParseUrl(options_.endpoint);
        auto wh = ToWideStr(url.host);
        auto wp = ToWideStr(url.path);
        if (wp.empty()) wp = L"/";

        HINTERNET hConnect = WinHttpConnect(hSession, wh.c_str(), url.port, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); return; }

        DWORD flags = (url.scheme == "https") ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST",
            wp.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return;
        }

        // Headers per MCP Streamable HTTP spec
        std::wstring hdrs = L"Content-Type: application/json\r\n"
                            L"Accept: application/json, text/event-stream\r\n";
        // Add MCP headers
        hdrs += L"MCP-Protocol-Version: 2026-07-28\r\n";
        try {
            auto body_json = nlohmann::json::parse(body);
            auto method = body_json.value("method", std::string());
            if (!method.empty()) {
                hdrs += L"Mcp-Method: " + ToWideStr(method) + L"\r\n";
            }
            // Extract primitive params as Mcp-Param-* headers for middleware routing
            auto params = body_json.find("params");
            if (params != body_json.end() && params->is_object()) {
                for (auto it = params->begin(); it != params->end(); ++it) {
                    if (it.value().is_string()) {
                        hdrs += L"Mcp-Param-" + ToWideStr(it.key()) + L": " + ToWideStr(it.value().get<std::string>()) + L"\r\n";
                    } else if (it.value().is_number_integer()) {
                        hdrs += L"Mcp-Param-" + ToWideStr(it.key()) + L": " + ToWideStr(std::to_string(it.value().get<int64_t>())) + L"\r\n";
                    } else if (it.value().is_boolean()) {
                        hdrs += L"Mcp-Param-" + ToWideStr(it.key()) + L": " + ToWideStr(it.value().get<bool>() ? "true" : "false") + L"\r\n";
                    }
                }
            }
        } catch (...) {
            MCP_LOG(Warning, "HTTP header parse failed");
            hdrs += L"Mcp-Method: tools/call\r\n";
        }
        for (auto& [k, v] : options_.additional_headers) {
            auto wk = ToWideStr(k);
            auto wv = ToWideStr(v);
            hdrs += wk + L": " + wv + L"\r\n";
        }

        WinHttpAddRequestHeaders(hRequest, hdrs.data(),
            static_cast<DWORD>(hdrs.size()), WINHTTP_ADDREQ_FLAG_ADD);

        BOOL sent = WinHttpSendRequest(hRequest,
            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            const_cast<char*>(body.data()),
            static_cast<DWORD>(body.size()),
            static_cast<DWORD>(body.size()), 0);

        if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
            // Read response headers to determine content type
            wchar_t contentType[64] = {};
            DWORD ctSize = sizeof(contentType);
            bool isSse = false;
            if (WinHttpQueryHeaders(hRequest,
                    WINHTTP_QUERY_CONTENT_TYPE, nullptr,
                    contentType, &ctSize, nullptr)) {
                isSse = (wcsstr(contentType, L"text/event-stream") != nullptr);
            }

            if (isSse && !sse_thread_.joinable()) {
                sse_request_ = hRequest;
                sse_thread_ = std::thread([this, hRequest, hConnect, hSession]() {
                    detail::SetThreadName("mcp-worker");
                    SseReadLoop(hRequest);
                    WinHttpCloseHandle(hRequest);
                    WinHttpCloseHandle(hConnect);
                    WinHttpCloseHandle(hSession);
                    sse_request_ = nullptr;
                });
            } else {
                // Drain response (single JSON response)
                std::string resp_body;
                char buf[4096];
                DWORD read = 0;
                while (WinHttpReadData(hRequest, buf, sizeof(buf), &read) && read > 0) {
                    resp_body.append(buf, read);
                    read = 0;
                }
                // Try to parse as JSON-RPC response and enqueue
                if (!resp_body.empty()) {
                    if (resp_body.size() > K_MAX_MESSAGE_SIZE) {
                        WinHttpCloseHandle(hRequest);
                        WinHttpCloseHandle(hConnect);
                        WinHttpCloseHandle(hSession);
                        return;
                    }
                    try {
                        auto j = nlohmann::json::parse(resp_body, nullptr, false, false);
                        JsonRpcMessage msg = j.get<JsonRpcMessage>();
                        asio::post(*io_ctx_, [this, msg = std::move(msg)]() {
                            if (channel_) channel_->Send(std::move(msg));
                        });
                    } catch (...) { MCP_LOG(Error, "HTTP response parse failed"); }
                }
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
            }
        } else {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
        }
    }

    void SseReadLoop(HINTERNET hRequest) {
        std::string buffer;
        while (running_) {
            char chunk[4096];
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, chunk, sizeof(chunk), &read)) break;
            if (read == 0) break;

            buffer.append(chunk, read);

            size_t pos;
            while ((pos = buffer.find("\n\n")) != std::string::npos) {
                std::string block = buffer.substr(0, pos);
                buffer.erase(0, pos + 2);
                DispatchSseBlock(block);
            }
        }
    }

    void DispatchSseBlock(const std::string& block) {
        // Parse SSE: event: message\ndata: {...}
        std::string data;
        std::istringstream ss(block);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.compare(0, 5, "data:") == 0) {
                auto val = line.substr(5);
                auto n = val.find_first_not_of(" \t");
                if (n != std::string::npos) val = val.substr(n);
                if (!data.empty()) data += "\n";
                data += val;
            }
        }
        if (!data.empty()) {
            if (data.size() > K_MAX_MESSAGE_SIZE) return;
            try {
                auto j = nlohmann::json::parse(data, nullptr, false, false);
                JsonRpcMessage msg = j.get<JsonRpcMessage>();
                asio::post(*io_ctx_, [this, msg = std::move(msg)]() {
                    if (channel_) channel_->Send(std::move(msg));
                });
            } catch (...) { MCP_LOG(Error, "HTTP SSE block parse failed"); }
        }
    }

    std::shared_ptr<asio::io_context> io_ctx_;
    HttpClientTransportOptions options_;
    std::thread send_thread_;
    std::thread sse_thread_;
    std::thread io_thread_;
    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    std::queue<std::string> send_queue_;
    std::atomic<bool> running_{false};
    HINTERNET sse_request_ = nullptr;
};

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// POSIX implementation (asio sockets)
// ═══════════════════════════════════════════════════════════════════════
#else
namespace {

class StreamableHttpSessionTransport : public TransportBase {
public:
    StreamableHttpSessionTransport(
        std::shared_ptr<asio::io_context> io_ctx,
        HttpClientTransportOptions options)
        : TransportBase(*io_ctx)
        , io_ctx_(std::move(io_ctx))
        , options_(std::move(options))
    {
        channel_ = std::make_unique<MessageChannel>(*io_ctx_, 64);
    }

    ~StreamableHttpSessionTransport() override { Close(); }

    void Start() {
        if (running_.exchange(true)) return;
        send_thread_ = std::thread([this] {
            detail::SetThreadName("mcp-worker");
            SendLoop();
        });
        io_thread_ = std::thread([this]() {
            detail::SetThreadName("mcp-worker");
            io_ctx_->run();
        });
    }

    void Close() override {
        if (!running_.exchange(false)) return;
        {
            std::lock_guard<std::mutex> lk(send_mutex_);
            send_cv_.notify_one();
        }
        if (sse_socket_) {
            asio::error_code ec;
            sse_socket_->close(ec);
            sse_socket_.reset();
        }
        if (sse_thread_.joinable()) sse_thread_.join();
        if (send_thread_.joinable()) send_thread_.join();
        io_ctx_->stop();
        if (io_thread_.joinable()) io_thread_.join();
        if (channel_) channel_->Close();
        SetDisconnected();
    }

    void SendMessageAsync(JsonRpcMessage message) override {
        if (!running_) return;
        nlohmann::json j = message;
        {
            std::lock_guard<std::mutex> lk(send_mutex_);
            send_queue_.push(j.dump());
        }
        send_cv_.notify_one();
    }

private:
    void SendLoop() {
        while (running_) {
            std::string body;
            {
                std::unique_lock<std::mutex> lk(send_mutex_);
                send_cv_.wait(lk, [this] {
                    return !send_queue_.empty() || !running_;
                });
                if (!running_) break;
                body = std::move(send_queue_.front());
                send_queue_.pop();
            }
            DoPost(body);
        }
    }

    // Build HTTP/1.1 POST request with MCP headers
    std::string BuildRequest(const std::string& body) {
        auto url = detail::ParseUrl(options_.endpoint);
        auto path = url.path.empty() ? "/" : url.path;

        std::string req = "POST " + path + " HTTP/1.1\r\n";
        req += "Host: " + url.host + "\r\n";
        req += "Content-Type: application/json\r\n";
        req += "Accept: application/json, text/event-stream\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "MCP-Protocol-Version: 2026-07-28\r\n";
        req += "Connection: keep-alive\r\n";

        try {
            auto body_json = nlohmann::json::parse(body);
            auto method = body_json.value("method", std::string());
            if (!method.empty()) {
                req += "Mcp-Method: " + method + "\r\n";
            }
            auto params = body_json.find("params");
            if (params != body_json.end() && params->is_object()) {
                for (auto it = params->begin(); it != params->end(); ++it) {
                    if (it.value().is_string()) {
                        req += "Mcp-Param-" + it.key() + ": " + it.value().get<std::string>() + "\r\n";
                    } else if (it.value().is_number_integer()) {
                        req += "Mcp-Param-" + it.key() + ": " + std::to_string(it.value().get<int64_t>()) + "\r\n";
                    } else if (it.value().is_boolean()) {
                        req += "Mcp-Param-" + it.key() + ": " + (it.value().get<bool>() ? "true" : "false") + "\r\n";
                    }
                }
            }
        } catch (...) {
            MCP_LOG(Warning, "HTTP header parse failed");
            req += "Mcp-Method: tools/call\r\n";
        }
        for (auto& [k, v] : options_.additional_headers) {
            req += k + ": " + v + "\r\n";
        }

        req += "\r\n";
        req += body;
        return req;
    }

    // Open an asio socket, send POST, read response.
    // If response is SSE (text/event-stream), start SSE reader.
    void DoPost(const std::string& body) {
        try {
            auto url = detail::ParseUrl(options_.endpoint);
            auto sse_ctx = std::make_shared<asio::io_context>();
            auto sock = std::make_shared<asio::ip::tcp::socket>(*sse_ctx);

            asio::ip::tcp::resolver resolver(*sse_ctx);
            auto endpoints = resolver.resolve(url.host, std::to_string(url.port));
            asio::connect(*sock, endpoints);

            std::string request = BuildRequest(body);
            asio::write(*sock, asio::buffer(request));

            // Read response headers (byte-by-byte until \r\n\r\n)
            asio::error_code ec;
            std::string header_buf;
            while (running_) {
                char c = 0;
                size_t n = sock->read_some(asio::buffer(&c, 1), ec);
                if (ec || n == 0) break;
                header_buf += c;
                if (header_buf.size() >= 4 &&
                    header_buf.substr(header_buf.size() - 4) == "\r\n\r\n")
                    break;
            }

            if (!running_) {
                asio::error_code close_ec;
                sock->close(close_ec);
                return;
            }

            // Check Content-Type for SSE vs JSON
            bool isSse = false;
            {
                std::string hdr_lower = header_buf;
                for (auto& ch : hdr_lower)
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                auto ct_pos = hdr_lower.find("content-type:");
                if (ct_pos != std::string::npos) {
                    auto val = hdr_lower.substr(ct_pos + 13);
                    auto nl = val.find("\r\n");
                    if (nl != std::string::npos) val = val.substr(0, nl);
                    auto start = val.find_first_not_of(" \t");
                    if (start != std::string::npos) val = val.substr(start);
                    isSse = (val.find("text/event-stream") != std::string::npos);
                }
            }

            if (isSse && !sse_thread_.joinable()) {
                sse_socket_ = sock;
                sse_ctx_ = sse_ctx;
                sse_thread_ = std::thread([this, sock]() {
                    detail::SetThreadName("mcp-worker");
                    SseReadLoop(sock);
                });
            } else {
                // Drain response (single JSON response)
                std::string resp_body;
                char buf[4096];
                while (true) {
                    size_t len = sock->read_some(asio::buffer(buf), ec);
                    if (ec) break;
                    resp_body.append(buf, len);
                }

                if (!resp_body.empty()) {
                    if (resp_body.size() > K_MAX_MESSAGE_SIZE) return;
                    try {
                        auto j = nlohmann::json::parse(resp_body, nullptr, false, false);
                        JsonRpcMessage msg = j.get<JsonRpcMessage>();
                        asio::post(*io_ctx_, [this, msg = std::move(msg)]() {
                            if (channel_) channel_->Send(std::move(msg));
                        });
                    } catch (...) { MCP_LOG(Error, "HTTP response parse failed"); }
                }
            }
        } catch (const std::exception& e) {
            MCP_LOG(Error, std::string("HTTP POST failed: ") + e.what());
        }
    }

    void SseReadLoop(std::shared_ptr<asio::ip::tcp::socket> sock) {
        std::string buffer;
        asio::error_code ec;
        while (running_) {
            char chunk[4096];
            size_t len = sock->read_some(asio::buffer(chunk), ec);
            if (ec) break;

            buffer.append(chunk, len);

            size_t pos;
            while ((pos = buffer.find("\n\n")) != std::string::npos) {
                std::string block = buffer.substr(0, pos);
                buffer.erase(0, pos + 2);
                DispatchSseBlock(block);
            }
        }
    }

    void DispatchSseBlock(const std::string& block) {
        std::string data;
        std::istringstream ss(block);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.compare(0, 5, "data:") == 0) {
                auto val = line.substr(5);
                auto n = val.find_first_not_of(" \t");
                if (n != std::string::npos) val = val.substr(n);
                if (!data.empty()) data += "\n";
                data += val;
            }
        }
        if (!data.empty()) {
            if (data.size() > K_MAX_MESSAGE_SIZE) return;
            try {
                auto j = nlohmann::json::parse(data, nullptr, false, false);
                JsonRpcMessage msg = j.get<JsonRpcMessage>();
                asio::post(*io_ctx_, [this, msg = std::move(msg)]() {
                    if (channel_) channel_->Send(std::move(msg));
                });
            } catch (...) { MCP_LOG(Error, "HTTP SSE block parse failed"); }
        }
    }

    std::shared_ptr<asio::io_context> io_ctx_;
    HttpClientTransportOptions options_;
    std::thread send_thread_;
    std::thread sse_thread_;
    std::thread io_thread_;
    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    std::queue<std::string> send_queue_;
    std::atomic<bool> running_{false};
    std::shared_ptr<asio::ip::tcp::socket> sse_socket_;
    std::shared_ptr<asio::io_context> sse_ctx_;
};

} // namespace
#endif

// ═══════════════════════════════════════════════════════════════════════
// Common StreamableHttpClientTransport
// ═══════════════════════════════════════════════════════════════════════

StreamableHttpClientTransport::StreamableHttpClientTransport(
    const HttpClientTransportOptions& options)
    : options_(options) {}

StreamableHttpClientTransport::~StreamableHttpClientTransport() = default;

std::string_view StreamableHttpClientTransport::Name() const {
    return options_.name.empty() ? std::string_view{"streamable-http"} : options_.name;
}

std::shared_ptr<ITransport> StreamableHttpClientTransport::Connect() {
    auto io_ctx = std::make_shared<asio::io_context>();
    auto session = std::make_shared<StreamableHttpSessionTransport>(
        std::move(io_ctx), options_);
    session->Start();
    return session;
}

} // namespace mcp
