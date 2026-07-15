#include <mcp/transport/SseClientTransport.hpp>
#include <mcp/JsonRpc.hpp>

#include <asio/experimental/channel.hpp>
#include <asio/post.hpp>

#ifdef _WIN32
#include <winhttp.h>
#include <windows.h>
#else
#include <mcp/transport/detail/http_client.hpp>
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
#include <vector>

#ifdef _WIN32
#pragma comment(lib, "winhttp.lib")
#endif

namespace mcp {

// JSON parse safety limits
#define K_MAX_MESSAGE_SIZE (8 * 1024 * 1024)  // 8MB
#define K_MAX_JSON_DEPTH 32

namespace {

// ═══════════════════════════════════════════════════════════════════════
// URL parsing helpers
// ═══════════════════════════════════════════════════════════════════════

struct UrlComponents {
    std::string scheme;
    std::string host;
    uint16_t port = 0;
    std::string path;
};

UrlComponents ParseUrl(const std::string& url) {
    UrlComponents c;
    auto scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos) return c;

    c.scheme = url.substr(0, scheme_pos);
    auto host_start = scheme_pos + 3;
    auto path_start = url.find('/', host_start);

    std::string host_port;
    if (path_start != std::string::npos) {
        host_port = url.substr(host_start, path_start - host_start);
        c.path = url.substr(path_start);
    } else {
        host_port = url.substr(host_start);
        c.path = "/";
    }

    auto colon = host_port.find(':');
    if (colon != std::string::npos) {
        c.host = host_port.substr(0, colon);
        c.port = static_cast<uint16_t>(std::stoul(host_port.substr(colon + 1)));
    } else {
        c.host = host_port;
        c.port = (c.scheme == "https") ? 443 : 80;
    }
    return c;
}

std::string ResolveEndpoint(const std::string& server_url, const std::string& endpoint) {
    // Already absolute
    if (endpoint.find("://") != std::string::npos)
        return endpoint;

    auto srv = ParseUrl(server_url);
    std::string base = srv.scheme + "://" + srv.host;
    if (!((srv.scheme == "https" && srv.port == 443) ||
          (srv.scheme == "http" && srv.port == 80)))
    {
        base += ":" + std::to_string(srv.port);
    }

    if (endpoint.empty() || endpoint[0] == '/')
        return base + endpoint;

    auto dir_end = srv.path.find_last_of('/');
    if (dir_end != std::string::npos)
        return base + srv.path.substr(0, dir_end + 1) + endpoint;

    return base + "/" + endpoint;
}

// ═══════════════════════════════════════════════════════════════════════
// SSE event parsing
// ═══════════════════════════════════════════════════════════════════════

struct SseEvent {
    std::string event_type;
    std::string data;
    std::string id;
    std::optional<int> retry;
};

SseEvent ParseSseEvent(const std::string& block) {
    SseEvent evt;
    evt.event_type = "message";

    std::istringstream stream(block);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        // event:<spaces?>value
        if (line.size() > 6 && line.compare(0, 6, "event:") == 0) {
            auto val = line.substr(6);
            auto n = val.find_first_not_of(" \t");
            if (n != std::string::npos) val = val.substr(n);
            else val.clear();
            evt.event_type = std::move(val);
        }
        // data:<spaces?>value
        else if (line.size() > 5 && line.compare(0, 5, "data:") == 0) {
            auto val = line.substr(5);
            auto n = val.find_first_not_of(" \t");
            if (n != std::string::npos) val = val.substr(n);
            else val.clear();
            if (!evt.data.empty()) evt.data += "\n";
            evt.data += val;
        }
        // id:<spaces?>value
        else if (line.size() > 3 && line.compare(0, 3, "id:") == 0) {
            auto val = line.substr(3);
            auto n = val.find_first_not_of(" \t");
            if (n != std::string::npos) val = val.substr(n);
            else val.clear();
            evt.id = std::move(val);
        }
        // retry:<spaces?>value
        else if (line.size() > 6 && line.compare(0, 6, "retry:") == 0) {
            auto val = line.substr(6);
            auto n = val.find_first_not_of(" \t");
            if (n != std::string::npos) val = val.substr(n);
            try {
                evt.retry = std::stoi(val);
            } catch (...) {
                // ignore invalid retry value
            }
        }
    }
    return evt;
}

#ifdef _WIN32
// Widen a UTF-8 string for WinAPI
std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                  static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                        static_cast<int>(s.size()), &w[0], len);
    return w;
}
#endif

// ═══════════════════════════════════════════════════════════════════════
// SseClientSessionTransport  (anonymous namespace)
// ═══════════════════════════════════════════════════════════════════════

class SseClientSessionTransport final : public TransportBase {
public:
    SseClientSessionTransport(std::shared_ptr<asio::io_context> io_ctx,
                              std::string server_url,
                              std::string name)
        : TransportBase(*io_ctx)
        , io_ctx_(std::move(io_ctx))
        , server_url_(std::move(server_url))
        , name_(std::move(name))
    {
        channel_ = std::make_unique<MessageChannel>(*io_ctx_, 64);
    }

    ~SseClientSessionTransport() override {
        Close();
    }

    void Start() {
        if (running_.exchange(true))
            return;

        url_ = ParseUrl(server_url_);
        sse_thread_ = std::thread([this] { SseReadLoop(); });
        send_thread_ = std::thread([this] { SendLoop(); });
        io_thread_ = std::thread([this]() { io_ctx_->run(); });
        SetConnected();
    }

    void Close() override {
        if (!running_.exchange(false))
            return;

        // Wake send thread
        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            send_cv_.notify_one();
        }

#ifdef _WIN32
        // Abort GET request — unblocks WinHttpReadData in SseReadLoop
        if (hGetRequest_) {
            WinHttpCloseHandle(hGetRequest_);
            hGetRequest_ = nullptr;
        }
        // Clean up cached POST handles
        if (post_connect_) {
            WinHttpCloseHandle(post_connect_);
            post_connect_ = nullptr;
        }
        if (post_session_) {
            WinHttpCloseHandle(post_session_);
            post_session_ = nullptr;
        }
        if (hConnect_) {
            WinHttpCloseHandle(hConnect_);
            hConnect_ = nullptr;
        }
        if (hSession_) {
            WinHttpCloseHandle(hSession_);
            hSession_ = nullptr;
        }
#else
        // Close SSE socket — unblocks read_some in SseReadLoop
        if (sse_socket_) {
            asio::error_code ec;
            sse_socket_->close(ec);
            sse_socket_.reset();
        }
#endif

        // Join threads
        if (send_thread_.joinable())
            send_thread_.join();
        if (sse_thread_.joinable())
            sse_thread_.join();

        io_ctx_->stop();
        if (io_thread_.joinable())
            io_thread_.join();

        if (channel_) channel_->Close();
        SetDisconnected();
    }

    void SendMessageAsync(JsonRpcMessage message) override {
        if (!running_)
            return;

        nlohmann::json j = message;
        std::string body = j.dump();

        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            send_queue_.push(std::move(body));
        }
        send_cv_.notify_one();
    }

private:
    // ── SSE read thread ──

    void SseReadLoop() {
#ifdef _WIN32
        hSession_ = WinHttpOpen(L"MCP-SSE-Client/1.0",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession_) {
            NotifyError("WinHttpOpen failed");
            running_ = false;
            return;
        }

        WinHttpSetTimeouts(hSession_, 5000, 5000, 5000, 30000);

        auto w_host = ToWide(url_.host);
        auto w_path = ToWide(url_.path);
        if (w_host.empty()) {
            NotifyError("Invalid server URL: host empty");
            running_ = false;
            return;
        }

        hConnect_ = WinHttpConnect(hSession_, w_host.c_str(), url_.port, 0);
        if (!hConnect_) {
            NotifyError("WinHttpConnect failed");
            running_ = false;
            return;
        }

        DWORD flags = (url_.scheme == "https") ? WINHTTP_FLAG_SECURE : 0;
        hGetRequest_ = WinHttpOpenRequest(hConnect_, L"GET", w_path.c_str(),
                                          nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hGetRequest_) {
            NotifyError("WinHttpOpenRequest(GET) failed");
            running_ = false;
            return;
        }

        static const wchar_t accept_hdr[] = L"Accept: text/event-stream\r\n";
        WinHttpAddRequestHeaders(hGetRequest_, accept_hdr,
                                 static_cast<DWORD>(wcslen(accept_hdr)),
                                 WINHTTP_ADDREQ_FLAG_ADD);

        if (!WinHttpSendRequest(hGetRequest_, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        {
            NotifyError("WinHttpSendRequest(GET) failed");
            running_ = false;
            return;
        }

        if (!WinHttpReceiveResponse(hGetRequest_, nullptr)) {
            NotifyError("WinHttpReceiveResponse(GET) failed");
            running_ = false;
            return;
        }

        // Read SSE stream
        std::string buffer;
        while (running_) {
            char chunk[4096];
            DWORD read = 0;

            if (!WinHttpReadData(hGetRequest_, chunk, sizeof(chunk), &read)) {
                if (running_)
                    NotifyError("WinHttpReadData failed");
                break;
            }
            if (read == 0)
                break;  // graceful close

            buffer.append(chunk, read);

            // Extract complete SSE blocks delimited by \n\n
            size_t pos;
            while ((pos = buffer.find("\n\n")) != std::string::npos) {
                auto block = buffer.substr(0, pos);
                buffer.erase(0, pos + 2);

                DispatchSseEvent(block);
            }
        }
#else
        try {
            asio::io_context sse_ctx;
            asio::ip::tcp::resolver resolver(sse_ctx);
            sse_socket_ = std::make_shared<asio::ip::tcp::socket>(sse_ctx);

            auto endpoints = resolver.resolve(url_.host,
                                              std::to_string(url_.port));
            asio::connect(*sse_socket_, endpoints);

            std::string request =
                "GET " + url_.path + " HTTP/1.1\r\n"
                "Host: " + url_.host + "\r\n"
                "Accept: text/event-stream\r\n"
                "Connection: keep-alive\r\n"
                "\r\n";

            asio::write(*sse_socket_, asio::buffer(request));

            // Read response headers
            asio::error_code ec;
            std::string header_buf;
            while (running_) {
                char c = 0;
                size_t n = sse_socket_->read_some(asio::buffer(&c, 1), ec);
                if (ec || n == 0) break;
                header_buf += c;
                if (header_buf.size() >= 4 &&
                    header_buf.substr(header_buf.size() - 4) == "\r\n\r\n")
                    break;
            }

            if (!running_) {
                sse_socket_->close(ec);
                sse_socket_.reset();
                return;
            }

            // Read SSE stream
            std::string buffer;
            while (running_) {
                char chunk[4096];
                size_t len = sse_socket_->read_some(asio::buffer(chunk), ec);
                if (ec) break;

                buffer.append(chunk, len);

                size_t pos;
                while ((pos = buffer.find("\n\n")) != std::string::npos) {
                    auto block = buffer.substr(0, pos);
                    buffer.erase(0, pos + 2);
                    DispatchSseEvent(block);
                }
            }
        } catch (const std::exception& e) {
            if (running_)
                NotifyError(std::string("SSE read error: ") + e.what());
        }

        sse_socket_.reset();
#endif

        // Connection lost — notify unless we initiated the close
        if (running_.exchange(false)) {
            {
                std::lock_guard<std::mutex> lk(send_mutex_);
                send_cv_.notify_one();
            }
            if (channel_) channel_->Close();
            SetDisconnected();
        }
    }

    void DispatchSseEvent(const std::string& block) {
        auto evt = ParseSseEvent(block);

        // Store last event ID for reconnection
        if (!evt.id.empty())
            last_event_id_ = evt.id;

        if (evt.event_type == "endpoint") {
            endpoint_url_ = ResolveEndpoint(server_url_, evt.data);
        } else if (evt.event_type == "message" && !evt.data.empty()) {
            if (evt.data.size() > K_MAX_MESSAGE_SIZE) {
                NotifyError("message size exceeds maximum allowed size");
                return;
            }
            try {
                auto j = nlohmann::json::parse(evt.data, nullptr, false, K_MAX_JSON_DEPTH);
                JsonRpcMessage msg = j.get<JsonRpcMessage>();
                EnqueueMessage(std::move(msg));
            } catch (const std::exception& e) {
                NotifyError(std::string("SSE parse error: ") + e.what());
            }
        }
        // Other event types are silently ignored
    }

    void EnqueueMessage(JsonRpcMessage msg) {
        asio::post(*io_ctx_, [this, msg = std::move(msg)]() mutable {
            if (!running_) return;
            if (channel_) channel_->Send(std::move(msg));
        });
    }

    // ── Send thread ──

    void SendLoop() {
        while (running_) {
            std::string body;
            {
                std::unique_lock<std::mutex> lock(send_mutex_);
                send_cv_.wait(lock, [this] {
                    return !send_queue_.empty() || !running_;
                });
                if (!running_ || send_queue_.empty())
                    continue;
                body = std::move(send_queue_.front());
                send_queue_.pop();
            }

            if (!endpoint_url_.empty())
                DoPost(body);
        }
    }

    void DoPost(const std::string& body) {
        auto ep = ParseUrl(endpoint_url_);
        if (ep.host.empty()) {
            endpoint_url_ = ResolveEndpoint(server_url_, endpoint_url_);
            ep = ParseUrl(endpoint_url_);
        }

#ifdef _WIN32
        // Lazily initialize cached WinHTTP handles for POST
        if (!post_session_) {
            post_session_ = WinHttpOpen(
                L"MCP-SSE-Client/1.0",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS, 0);
            if (!post_session_) return;
            WinHttpSetTimeouts(post_session_, 5000, 5000, 5000, 30000);
        }

        auto w_host = ToWide(ep.host.empty() ? url_.host : ep.host);
        auto w_path = ToWide(ep.path.empty() ? "/" : ep.path);
        uint16_t port = ep.port ? ep.port : url_.port;

        if (!post_connect_) {
            post_connect_ = WinHttpConnect(post_session_, w_host.c_str(), port, 0);
            if (!post_connect_) return;
        }

        DWORD flags = ((ep.scheme == "https") || (url_.scheme == "https"))
                          ? WINHTTP_FLAG_SECURE
                          : 0;
        HINTERNET hRequest = WinHttpOpenRequest(post_connect_, L"POST", w_path.c_str(),
                                                 nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hRequest) return;

        static const wchar_t ct_hdr[] = L"Content-Type: application/json\r\n";
        WinHttpAddRequestHeaders(hRequest, ct_hdr,
                                 static_cast<DWORD>(wcslen(ct_hdr)),
                                 WINHTTP_ADDREQ_FLAG_ADD);

        BOOL sent = WinHttpSendRequest(
            hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            const_cast<char*>(body.data()),
            static_cast<DWORD>(body.size()),
            static_cast<DWORD>(body.size()), 0);

        if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
            // Drain response body
            char discard[1024];
            DWORD read = 0;
            while (WinHttpReadData(hRequest, discard, sizeof(discard), &read) &&
                   read > 0)
            {
                read = 0;
            }
        }

        WinHttpCloseHandle(hRequest);
#else
        (void)ep;
        try {
            asio::io_context tmp_ctx;
            detail::HttpPost(tmp_ctx, endpoint_url_, body);
        } catch (...) {
            // Silently ignore (matches WinHTTP behavior)
        }
#endif
    }

    // ── Members ──

    std::shared_ptr<asio::io_context> io_ctx_;
    std::string server_url_;
    std::string name_;
    std::string endpoint_url_;
    std::string last_event_id_;
    UrlComponents url_;

#ifdef _WIN32
    // WinHTTP handles (owned by SseReadLoop)
    HINTERNET hSession_ = nullptr;
    HINTERNET hConnect_ = nullptr;
    HINTERNET hGetRequest_ = nullptr;
    // Cached WinHTTP handles for POST (send path)
    HINTERNET post_session_ = nullptr;
    HINTERNET post_connect_ = nullptr;
#else
    // asio socket for SSE streaming (owned by SseReadLoop)
    std::shared_ptr<asio::ip::tcp::socket> sse_socket_;
#endif

    // SSE reader thread + send thread + io_context runner
    std::thread sse_thread_;
    std::thread send_thread_;
    std::thread io_thread_;
    std::atomic<bool> running_{false};

    // Send queue (producer: SendMessageAsync / consumer: SendLoop)
    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    std::queue<std::string> send_queue_;
};

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// SseClientTransport
// ═══════════════════════════════════════════════════════════════════════

SseClientTransport::SseClientTransport(std::string_view server_url, std::string_view name)
    : server_url_(server_url)
    , name_(name.empty() ? std::string("sse") : std::string(name))
{
}

SseClientTransport::~SseClientTransport() = default;

std::string_view SseClientTransport::Name() const { return name_; }

std::shared_ptr<ITransport> SseClientTransport::Connect() {
    auto io_ctx = std::make_shared<asio::io_context>();
    auto session = std::make_shared<SseClientSessionTransport>(std::move(io_ctx),
                                                               server_url_, name_);
    session->Start();
    return session;
}

} // namespace mcp
