#include <mcp/transport/WebSocketClientTransport.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/Log.hpp>

#include <transport/detail/net/WebSocketClient.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

namespace mcp {

namespace {

constexpr std::chrono::milliseconds kWebSocketConnectWait(5000);
constexpr std::chrono::milliseconds kWebSocketCloseWait(5000);

class WebSocketSessionTransport : public TransportBase {
public:
    WebSocketSessionTransport(std::string url,
                              std::shared_ptr<detail::SessionLifecycle> lifecycle,
                              uint64_t generation)
        : url_(std::move(url)) {
        AdoptLifecycle(std::move(lifecycle), generation);
    }
    ~WebSocketSessionTransport() override;

    void Start() override;
    void Close() override;
    void SendMessageAsync(JsonRpcMessage message) override;

private:
    std::string url_;
    detail::net::WebSocketClient ws_;
    std::atomic<bool> running_{false};
};

WebSocketSessionTransport::~WebSocketSessionTransport() { Close(); }

void WebSocketSessionTransport::Start() {
    uint64_t generation = Generation();
    auto weak_self = std::weak_ptr<WebSocketSessionTransport>(
        std::static_pointer_cast<WebSocketSessionTransport>(shared_from_this()));
    ws_.SetCallbacks(
        [weak_self, generation](std::string_view msg) {
            auto self = weak_self.lock();
            if (!self || !self->IsGenerationActive()) return;
            try {
                auto parsed = DeserializeMessage(msg);
                self->WriteMessage(std::move(parsed));
            } catch (const std::exception& e) {
                MCP_LOG(Error, std::string("WebSocket parse error: ") + e.what());
                self->NotifyError(std::string("WebSocket parse error: ") + e.what());
            }
        },
        [weak_self, generation]() {
            auto self = weak_self.lock();
            if (!self || !self->IsGenerationActive()) return;
            self->running_.store(false);
            self->CompleteShutdown();
        },
        [weak_self, generation](std::string_view message) {
            auto self = weak_self.lock();
            if (!self || !self->IsGenerationActive()) return;
            MCP_LOG(Error, std::string("WebSocket error: ") + std::string(message));
            self->NotifyError(message);
        });

    running_.store(true);
    SetConnected();
    ws_.Open(url_, std::chrono::seconds(30), true);
}

void WebSocketSessionTransport::Close() {
    if (!BeginShutdown()) return;
    running_.store(false);
    if (channel_) channel_->Close();
    ws_.Close();
    CompleteShutdown();
}

void WebSocketSessionTransport::SendMessageAsync(JsonRpcMessage message) {
    if (!running_.load() || !IsGenerationActive()) return;
    auto json_str = SerializeMessage(std::move(message));
    ws_.Send(json_str);
}

} // namespace

WebSocketClientTransport::WebSocketClientTransport(std::string url, std::string name)
    : url_(std::move(url)), name_(std::move(name)) {}

WebSocketClientTransport::~WebSocketClientTransport() = default;

std::string_view WebSocketClientTransport::Name() const { return name_; }

std::shared_ptr<ITransport> WebSocketClientTransport::Connect() {
    std::lock_guard<std::mutex> lock(connect_mutex_);
    auto previous = std::move(current_);
    uint64_t previous_generation = current_generation_;
    if (previous) {
        previous->Close();
        lifecycle_->WaitClosed(previous_generation, kWebSocketCloseWait);
    }
    uint64_t generation = lifecycle_->BeginConnect(kWebSocketConnectWait);
    auto session = std::make_shared<WebSocketSessionTransport>(url_, lifecycle_, generation);
    session->Start();
    current_generation_ = generation;
    current_ = session;
    return session;
}

} // namespace mcp
