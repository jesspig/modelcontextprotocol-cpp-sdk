// SseClientTransport.cpp — SSE client transport implementation

#include <mcp/detail/ThreadUtils.hpp>
#include <mcp/transport/SseClientTransport.hpp>
#include <mcp/transport/detail/Url.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <mcp/JsonRpc.hpp>

#include <transport/detail/net/HttpClient.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>

namespace mcp {

namespace {

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

struct SseEvent {
    std::string event_type;
    std::string data;
    std::string id;   // from the "id:" line
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

        if (line.size() > 6 && line.compare(0, 6, "event:") == 0) {
            auto val = line.substr(6);
            auto n = val.find_first_not_of(" \t");
            if (n != std::string::npos) val = val.substr(n);
            else val.clear();
            evt.event_type = std::move(val);
        }
        else if (line.size() > 5 && line.compare(0, 5, "data:") == 0) {
            auto val = line.substr(5);
            auto n = val.find_first_not_of(" \t");
            if (n != std::string::npos) val = val.substr(n);
            else val.clear();
            if (!evt.data.empty()) evt.data += "\n";
            evt.data += val;
        }
        else if (line.size() > 3 && line.compare(0, 3, "id:") == 0) {
            auto val = line.substr(3);
            auto n = val.find_first_not_of(" \t");
            if (n != std::string::npos) val = val.substr(n);
            else val.clear();
            evt.id = std::move(val);
        }
    }
    return evt;
}

class SseClientSessionTransport final : public TransportBase {
public:
    SseClientSessionTransport(std::string server_url, std::string name)
        : TransportBase()
        , http_client_(std::make_unique<detail::net::HttpClient>())
        , post_client_(std::make_unique<detail::net::HttpClient>())
        , server_url_(std::move(server_url))
        , name_(std::move(name))
    {
    }

    ~SseClientSessionTransport() override {
        Close();
    }

    void Start() {
        if (running_.exchange(true))
            return;

        sse_thread_ = std::thread([this] { SseReadLoop(); });
        send_thread_ = std::thread([this] { SendLoop(); });
        SetConnected();
    }

    void Close() override {
        if (running_.exchange(false)) {
            {
                std::lock_guard<std::mutex> lock(send_mutex_);
                send_cv_.notify_one();
            }

            // Closing both clients unblocks the SSE read and POST sends
            if (post_client_)
                post_client_->Close();
            if (http_client_)
                http_client_->Close();
        }

        // Join unconditionally: the SSE read thread may have exited on its own
        // (reconnect exhaustion / oversize), so Close() must not be short-circuited
        detail::JoinThreadSafely(send_thread_);
        detail::JoinThreadSafely(sse_thread_);

        if (channel_) channel_->Close();
        SetDisconnected();
    }

    void SendMessageAsync(JsonRpcMessage message) override {
        if (!running_)
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
            req.url = server_url_;
            req.headers["Accept"] = "text/event-stream";
            if (!last_event_id_.empty()) {
                req.headers["Last-Event-ID"] = last_event_id_;
            }

            auto body_cb = [this](std::string_view chunk) {
                if (!chunk.empty()) {
                    sse_buffer_.append(chunk.data(), chunk.size());

                    if (sse_buffer_.size() > detail::kMaxMessageSize) {
                        sse_buffer_.clear();
                        NotifyError("message size exceeds maximum allowed size");
                        running_.store(false);
                        // Closing the client aborts the blocking recv in Request()
                        http_client_->Close();
                        return;
                    }

                    size_t pos;
                    while ((pos = sse_buffer_.find("\n\n")) != std::string::npos) {
                        auto block = sse_buffer_.substr(0, pos);
                        sse_buffer_.erase(0, pos + 2);
                        DispatchSseEvent(block);
                    }
                }
            };

            try {
                http_client_->Request(req, body_cb);
            } catch (...) {
                if (!running_) break;
            }

            // The stream ended. Exit on explicit Close, otherwise back off
            // and reconnect (the server replays missed events via
            // Last-Event-ID).
            if (!running_) break;
            if (!WaitForReconnect()) break;
        }

        // The SSE read thread exited on its own (reconnect exhaustion/oversize);
        // leave running_ untouched so Close() performs the full teardown.
        // Close the channel first, then wake SendLoop, whose wait predicate
        // also checks the channel state.
        if (channel_) channel_->Close();
        {
            std::lock_guard<std::mutex> lk(send_mutex_);
            send_cv_.notify_one();
        }
        SetDisconnected();
    }

    bool WaitForReconnect() {
        constexpr std::chrono::milliseconds kBackoffBase(1000);
        constexpr int kMaxBackoffSteps = 5;
        for (int step = 0; step < kMaxBackoffSteps; ++step) {
            auto deadline = std::chrono::steady_clock::now()
                + kBackoffBase * (1 << step);
            while (std::chrono::steady_clock::now() < deadline) {
                if (!running_.load()) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        return false;
    }

    void DispatchSseEvent(const std::string& block) {
        auto evt = ParseSseEvent(block);
        if (evt.id.empty()) {
            last_event_id_.clear();
        } else {
            last_event_id_ = std::move(evt.id);
        }

        if (evt.event_type == "endpoint") {
            try {
                std::lock_guard<std::mutex> lock(send_mutex_);
                endpoint_url_ = ResolveEndpoint(server_url_, evt.data);
            } catch (const std::exception& e) {
                NotifyError(std::string("invalid endpoint URL: ") + e.what());
            }
        } else if (evt.event_type == "message" && !evt.data.empty()) {
            if (evt.data.size() > detail::kMaxMessageSize) {
                NotifyError("message size exceeds maximum allowed size");
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

    void SendLoop() {
        while (running_) {
            std::string body;
            {
                std::unique_lock<std::mutex> lock(send_mutex_);
                send_cv_.wait(lock, [this] {
                    return !send_queue_.empty() || !running_ ||
                           (channel_ != nullptr && !channel_->IsOpen());
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
        try {
            post_client_->Request(req);
        } catch (...) {
        }
    }

    std::unique_ptr<detail::net::HttpClient> http_client_;
    std::unique_ptr<detail::net::HttpClient> post_client_;
    std::string server_url_;
    std::string name_;
    std::string endpoint_url_;
    std::string sse_buffer_;
    std::string last_event_id_;

    std::thread sse_thread_;
    std::thread send_thread_;
    std::atomic<bool> running_{false};

    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    std::queue<std::string> send_queue_;
};

} // namespace

SseClientTransport::SseClientTransport(std::string_view server_url, std::string_view name)
    : server_url_(server_url)
    , name_(name.empty() ? std::string("sse") : std::string(name))
{
}

SseClientTransport::~SseClientTransport() = default;

std::string_view SseClientTransport::Name() const { return name_; }

std::shared_ptr<ITransport> SseClientTransport::Connect() {
    auto session = std::make_shared<SseClientSessionTransport>(server_url_, name_);
    session->Start();
    return session;
}

} // namespace mcp
