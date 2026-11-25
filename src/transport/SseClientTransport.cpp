#include <mcp/detail/SseEventParser.hpp>
#include <mcp/detail/ThreadUtils.hpp>
#include <mcp/transport/SseClientTransport.hpp>
#include <transport/SseClientInternal.hpp>
#include <mcp/transport/detail/Url.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <mcp/JsonRpc.hpp>

#include <transport/detail/net/HttpClient.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>

namespace mcp {

namespace sse_client_impl {

constexpr std::chrono::milliseconds kBackoffBase(1000);
constexpr std::chrono::milliseconds kBackoffCap(30000);
constexpr std::chrono::milliseconds kReconnectWaitFloor(450);
constexpr std::chrono::milliseconds kReconnectWaitCeiling(1000);

std::chrono::milliseconds ReconnectDelayForAttempt(int attempt, std::optional<uint64_t> retry_ms);

} // namespace sse_client_impl

namespace {

constexpr std::chrono::milliseconds kSseClientIdleTimeout(5 * 60 * 1000);
constexpr int kMaxReconnectAttempts = 2;
constexpr std::chrono::milliseconds kSseConnectWait(5000);
constexpr std::chrono::milliseconds kSseCloseWait(5000);

std::string ResolveEndpoint(const std::string& server_url, const std::string& endpoint) {
    if (endpoint.find("://") != std::string::npos)
        return endpoint;

    auto srv = detail::ParseUrl(server_url);
    std::string host = srv.host.find(':') != std::string::npos ? "[" + srv.host + "]" : srv.host;
    std::string base = srv.scheme + "://" + host;
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

class SseClientSessionTransport final : public TransportBase {
public:
    SseClientSessionTransport(SseClientTransportOptions options,
                              std::shared_ptr<detail::SessionLifecycle> lifecycle,
                              uint64_t generation,
                              std::shared_ptr<MessageChannel> channel = nullptr)
        : TransportBase()
        , http_client_(std::make_unique<detail::net::HttpClient>())
        , post_client_(std::make_unique<detail::net::HttpClient>())
        , options_(std::move(options))
        , sse_parser_(SseEventLimit())
    {
        AdoptLifecycle(std::move(lifecycle), generation);
        if (channel) AdoptMessageChannel(std::move(channel));
    }

    ~SseClientSessionTransport() override {
        Close();
    }

    void Start() override {
        if (running_.exchange(true))
            return;

        SetConnected();
        sse_thread_ = std::thread([this] { SseReadLoop(); });
        send_thread_ = std::thread([this] { SendLoop(); });
    }

    void Close() override {
        if (close_entered_.exchange(true)) return;
        running_.store(false);
        BeginShutdown();
        CloseMessageChannel();

        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            send_cv_.notify_one();
        }

        if (post_client_)
            post_client_->Close();
        if (http_client_)
            http_client_->Close();

        detail::JoinThreadSafely(send_thread_);
        detail::JoinThreadSafely(sse_thread_);
        CompleteShutdown();
    }

    void SendMessageAsync(JsonRpcMessage message) override {
        if (!running_ || !IsGenerationActive())
            return;

        std::string body = SerializeMessage(std::move(message));

        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            send_queue_.push(std::move(body));
        }
        send_cv_.notify_one();
    }

private:
    void SseReadLoop() {
        while (running_) {
            detail::net::HttpRequestSpec req;
            req.method = "GET";
            req.url = options_.server_url;
            req.timeout = kSseClientIdleTimeout;
            req.max_response_bytes = std::nullopt;
            req.headers["Accept"] = "text/event-stream";
            req.redirect_policy = options_.redirect_policy;
            req.max_redirects = options_.max_redirects;
            if (!last_event_id_.empty()) {
                req.headers["Last-Event-ID"] = last_event_id_;
            }
            {
                std::string auth = CurrentAuthValue();
                if (!auth.empty()) req.headers["Authorization"] = auth;
            }
            for (const auto& [name, value] : options_.additional_headers) {
                req.headers[name] = value;
            }
            for (const auto& [name, value] : ResolvePerRequestHeaders(
                     "GET", options_.server_url, detail::HttpRequestPurpose::ListenStream, 0)) {
                req.headers[name] = value;
            }

            auto body_cb = [this](std::string_view chunk) {
                if (chunk.empty() || !IsGenerationActive()) return;

                auto fed = sse_parser_.Feed(chunk);
                if (fed.limit_exceeded) {
                    sse_parser_ = detail::SseIncrementalParser(SseEventLimit());
                    NotifyError("SSE event size exceeds maximum allowed size");
                    running_.store(false);
                    http_client_->Close();
                    return;
                }
                for (const auto& evt : fed.events) DispatchSseEvent(evt);
            };

            detail::net::HttpResponseInfo resp;
            try {
                resp = http_client_->Request(req, body_cb);
            } catch (...) {
                if (!running_) break;
            }

            FlushSseBuffer();
            ObserveChallenge(resp);

            if (!running_) break;
            if (ApplyAuthChallenge(resp.status_code, LastHeader(resp, "www-authenticate"))) {
                continue;
            }
            if (reconnect_attempts_ >= kMaxReconnectAttempts) break;
            ++reconnect_attempts_;
            if (!WaitForReconnect(reconnect_attempts_)) break;
        }

        if (running_) NotifyEstablish(false);
        CloseMessageChannel();
        {
            std::lock_guard<std::mutex> lk(send_mutex_);
            send_cv_.notify_one();
        }
        SetDisconnected();
    }

    bool WaitForReconnect(int attempt) {
        auto delay = sse_client_impl::ReconnectDelayForAttempt(attempt, retry_ms_);
        auto deadline = std::chrono::steady_clock::now() + delay;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!running_.load()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return true;
    }

    void DispatchSseEvent(const detail::SseEvent& evt) {
        if (!IsGenerationActive()) return;
        if (evt.event_type == "endpoint" && !evt.data.empty()) {
            reconnect_attempts_ = 0;
            NotifyEstablish(true);
        } else if (evt.event_type == "message" && !evt.data.empty()) {
            reconnect_attempts_ = 0;
            NotifyEstablish(true);
        }
        if (evt.retry_ms.has_value()) {
            retry_ms_ = evt.retry_ms;
        }
        if (evt.event_type == "message" && evt.data.empty() && evt.id.empty() &&
            !evt.retry_ms.has_value()) {
            return;
        }
        if (!evt.id.empty()) {
            last_event_id_ = std::move(evt.id);
        }

        if (evt.event_type == "endpoint") {
            try {
                std::lock_guard<std::mutex> lock(send_mutex_);
                endpoint_url_ = ResolveEndpoint(options_.server_url, evt.data);
            } catch (const std::exception& e) {
                NotifyError(std::string("invalid endpoint URL: ") + e.what());
            }
            send_cv_.notify_one();
        } else if (evt.event_type == "message" && !evt.data.empty()) {
            if (evt.data.size() > SseEventLimit()) {
                NotifyError("SSE event size exceeds maximum allowed size");
                return;
            }
            try {
                JsonRpcMessage msg = DeserializeMessage(evt.data);
                WriteMessage(std::move(msg));
            } catch (const std::exception& e) {
                NotifyError(std::string("SSE parse error: ") + e.what());
            }
        }
    }

    void FlushSseBuffer() {
        auto flushed = sse_parser_.Flush();
        sse_parser_ = detail::SseIncrementalParser(SseEventLimit());
        for (const auto& evt : flushed.events) DispatchSseEvent(evt);
    }

    void SendLoop() {
        while (running_) {
            std::string body;
            {
                std::unique_lock<std::mutex> lock(send_mutex_);
                send_cv_.wait(lock, [this] {
                    if (!running_ || (channel_ != nullptr && !channel_->IsOpen()))
                        return true;
                    return !send_queue_.empty() && !endpoint_url_.empty();
                });
                if (!running_ || (channel_ != nullptr && !channel_->IsOpen()))
                    break;
                body = std::move(send_queue_.front());
                send_queue_.pop();
            }

            std::string endpoint;
            {
                std::lock_guard<std::mutex> lock(send_mutex_);
                endpoint = endpoint_url_;
            }
            if (!endpoint.empty())
                DoPost(endpoint, body);
        }
    }

    void DoPost(const std::string& endpoint, const std::string& body) {
        detail::net::HttpRequestSpec req;
        req.method = "POST";
        req.url = endpoint;
        req.headers["Content-Type"] = "application/json";
        req.body = body;
        req.redirect_policy = options_.redirect_policy;
        req.max_redirects = options_.max_redirects;
        {
            std::string auth = CurrentAuthValue();
            if (!auth.empty()) req.headers["Authorization"] = auth;
        }
        for (const auto& [name, value] : options_.additional_headers) {
            req.headers[name] = value;
        }
        for (const auto& [name, value] : ResolvePerRequestHeaders(
                 "POST", endpoint, detail::HttpRequestPurpose::MessagePost, 0)) {
            req.headers[name] = value;
        }
        try {
            auto resp = post_client_->Request(req);
            ObserveChallenge(resp);
        } catch (...) {
        }
    }

    std::map<std::string, std::string> ResolvePerRequestHeaders(
        std::string_view method, std::string_view url,
        detail::HttpRequestPurpose purpose, int attempt) const {
        std::map<std::string, std::string> headers;
        if (!options_.apply_per_request_headers) return headers;
        detail::HttpRequestHeaderContext context;
        context.http_method = method;
        context.url = url;
        context.purpose = purpose;
        context.attempt_index = attempt;
        options_.apply_per_request_headers(context, headers);
        return headers;
    }

    std::string CurrentAuthValue() const {
        std::lock_guard<std::mutex> lock(auth_mutex_);
        return auth_value_;
    }

    void ObserveChallenge(const detail::net::HttpResponseInfo& resp) const {
        if (!options_.observe_response_challenge) return;
        std::string www = LastHeader(resp, "www-authenticate");
        std::string nonce = LastHeader(resp, "dpop-nonce");
        detail::HttpResponseChallengeContext context;
        context.status_code = resp.status_code;
        context.www_authenticate = www;
        context.dpop_nonce = nonce;
        options_.observe_response_challenge(context);
    }

    bool ApplyAuthChallenge(int status_code, std::string_view www_authenticate) {
        if (!options_.auth_challenge_handler) return false;
        if (status_code != 401 && status_code != 403) return false;
        std::string lower;
        for (char c : www_authenticate)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (status_code == 403 && lower.find("insufficient_scope") == std::string::npos)
            return false;
        std::string next;
        try {
            next = options_.auth_challenge_handler(www_authenticate);
        } catch (...) {
            return false;
        }
        if (next.empty()) return false;
        std::lock_guard<std::mutex> lock(auth_mutex_);
        auth_value_ = std::move(next);
        return true;
    }

    void NotifyEstablish(bool ok) {
        if (!options_.on_establish_result) return;
        bool expected = false;
        if (!establish_notified_.compare_exchange_strong(expected, true)) return;
        options_.on_establish_result(ok);
    }

    static std::string LastHeader(const detail::net::HttpResponseInfo& resp,
                                  std::string_view name) {
        std::string lower;
        for (char c : name)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto it = resp.headers.find(lower);
        return it == resp.headers.end() ? std::string{} : it->second;
    }

    std::size_t SseEventLimit() const {
        return options_.max_sse_event_bytes.value_or(
            (std::numeric_limits<std::size_t>::max)());
    }

    std::unique_ptr<detail::net::HttpClient> http_client_;
    std::unique_ptr<detail::net::HttpClient> post_client_;
    SseClientTransportOptions options_;
    std::string endpoint_url_;
    detail::SseIncrementalParser sse_parser_{
        (std::numeric_limits<std::size_t>::max)()};
    std::string last_event_id_;
    std::optional<uint64_t> retry_ms_;
    int reconnect_attempts_ = 0;
    mutable std::mutex auth_mutex_;
    std::string auth_value_;
    std::atomic<bool> establish_notified_{false};

    std::thread sse_thread_;
    std::thread send_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> close_entered_{false};

    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    std::queue<std::string> send_queue_;
};

} // namespace

namespace sse_client_impl {

std::shared_ptr<ITransport> CreateSession(
    SseClientTransportOptions options,
    std::shared_ptr<detail::SessionLifecycle> lifecycle,
    uint64_t generation,
    std::shared_ptr<MessageChannel> channel) {
    auto session = std::make_shared<SseClientSessionTransport>(
        std::move(options), std::move(lifecycle), generation, std::move(channel));
    session->Start();
    return session;
}

std::chrono::milliseconds ReconnectDelayForAttempt(int attempt, std::optional<uint64_t> retry_ms) {
    std::chrono::milliseconds wait = kBackoffBase;
    for (int i = 1; i < attempt; ++i)
        wait = (std::min)(wait * 3 / 2, kBackoffCap);
    if (retry_ms.has_value()) {
        uint64_t raw = *retry_ms;
        if (raw <= static_cast<uint64_t>(kReconnectWaitFloor.count()))
            wait = kReconnectWaitFloor;
        else if (raw >= static_cast<uint64_t>(kReconnectWaitCeiling.count()))
            wait = kReconnectWaitCeiling;
        else
            wait = std::chrono::milliseconds(raw);
    }
    if (wait < kReconnectWaitFloor)
        wait = kReconnectWaitFloor;
    if (wait > kReconnectWaitCeiling)
        wait = kReconnectWaitCeiling;
    return wait;
}

} // namespace sse_client_impl

SseClientTransport::SseClientTransport(std::string_view server_url, std::string_view name)
    : options_()
{
    options_.server_url = std::string(server_url);
    options_.name = name.empty() ? std::string("sse") : std::string(name);
}

SseClientTransport::SseClientTransport(SseClientTransportOptions options)
    : options_(std::move(options))
{
    if (options_.name.empty()) options_.name = "sse";
}

SseClientTransport::~SseClientTransport() = default;

std::string_view SseClientTransport::Name() const { return options_.name; }

std::shared_ptr<ITransport> SseClientTransport::Connect() {
    std::lock_guard<std::mutex> lock(connect_mutex_);
    auto previous = std::move(current_);
    uint64_t previous_generation = current_generation_;
    if (previous) {
        previous->Close();
        lifecycle_->WaitClosed(previous_generation, kSseCloseWait);
    }
    uint64_t generation = lifecycle_->BeginConnect(kSseConnectWait);
    auto session = sse_client_impl::CreateSession(options_, lifecycle_, generation, nullptr);
    current_generation_ = generation;
    current_ = session;
    return session;
}

} // namespace mcp
