#include <mcp/transport/StreamableHttpClientTransport.hpp>
#include <mcp/JsonRpc.hpp>

#include <asio/experimental/channel.hpp>
#include <asio/post.hpp>
#include <nlohmann/json.hpp>

#include <winhttp.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace mcp {

namespace {

struct UrlParts {
    std::string scheme;
    std::string host;
    uint16_t port = 0;
    std::string path;
};

UrlParts ParseHttpUrl(const std::string& url) {
    UrlParts c;
    auto sp = url.find("://");
    if (sp == std::string::npos) return c;
    c.scheme = url.substr(0, sp);
    auto hs = sp + 3;
    auto ps = url.find('/', hs);
    std::string hp;
    if (ps != std::string::npos) {
        hp = url.substr(hs, ps - hs);
        c.path = url.substr(ps);
    } else {
        hp = url.substr(hs);
        c.path = "/";
    }
    auto col = hp.find(':');
    if (col != std::string::npos) {
        c.host = hp.substr(0, col);
        c.port = static_cast<uint16_t>(std::stoul(hp.substr(col + 1)));
    } else {
        c.host = hp;
        c.port = (c.scheme == "https") ? 443 : 80;
    }
    return c;
}

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

// ── StreamableHttpClientSessionTransport ──
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
        send_thread_ = std::thread([this] { SendLoop(); });
    }

    void Close() override {
        if (!running_.exchange(false)) return;
        {
            std::lock_guard<std::mutex> lk(send_mutex_);
            send_cv_.notify_one();
        }
        if (sse_thread_.joinable()) sse_thread_.join();
        if (send_thread_.joinable()) send_thread_.join();
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

    // Open a WinHTTP POST, send body, read response.
    // If response is SSE (text/event-stream), start SSE reader.
    void DoPost(const std::string& body) {
        HINTERNET hSession = WinHttpOpen(L"MCP-HTTP-Client/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return;

        auto url = ParseHttpUrl(options_.endpoint);
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
        hdrs += L"Mcp-Method: tools/call\r\n";
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
                // Start SSE reader thread using this request handle
                // Transfer ownership by starting a reader
                sse_thread_ = std::thread([this, hRequest, hConnect, hSession]() {
                    SseReadLoop(hRequest);
                    WinHttpCloseHandle(hRequest);
                    WinHttpCloseHandle(hConnect);
                    WinHttpCloseHandle(hSession);
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
                    try {
                        auto j = nlohmann::json::parse(resp_body);
                        JsonRpcMessage msg = j.get<JsonRpcMessage>();
                        asio::post(*io_ctx_, [this, msg = std::move(msg)]() {
                            if (channel_) channel_->Send(std::move(msg));
                        });
                    } catch (...) {}
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
            try {
                auto j = nlohmann::json::parse(data);
                JsonRpcMessage msg = j.get<JsonRpcMessage>();
                asio::post(*io_ctx_, [this, msg = std::move(msg)]() {
                    if (channel_) channel_->Send(std::move(msg));
                });
            } catch (...) {}
        }
    }

    std::shared_ptr<asio::io_context> io_ctx_;
    HttpClientTransportOptions options_;
    std::thread send_thread_;
    std::thread sse_thread_;
    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    std::queue<std::string> send_queue_;
    std::atomic<bool> running_{false};
};

} // namespace

StreamableHttpClientTransport::StreamableHttpClientTransport(
    const HttpClientTransportOptions& options)
    : options_(options) {}

StreamableHttpClientTransport::~StreamableHttpClientTransport() = default;

std::string_view StreamableHttpClientTransport::Name() const {
    return options_.name.empty() ? "streamable-http" : options_.name;
}

std::shared_ptr<ITransport> StreamableHttpClientTransport::Connect() {
    auto io_ctx = std::make_shared<asio::io_context>();
    auto session = std::make_shared<StreamableHttpSessionTransport>(
        std::move(io_ctx), options_);
    session->Start();
    return session;
}

} // namespace mcp
