// WebSocketClientTransport.cpp — WebSocket client transport implementation

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

class WebSocketSessionTransport : public TransportBase {
public:
    explicit WebSocketSessionTransport(std::string url);
    ~WebSocketSessionTransport() override;

    void Start() override;
    void Close() override;
    void SendMessageAsync(JsonRpcMessage message) override;

private:
    std::string url_;
    detail::net::WebSocketClient ws_;
    std::atomic<bool> running_{false};
};

WebSocketSessionTransport::WebSocketSessionTransport(std::string url)
    : url_(std::move(url)) {}

WebSocketSessionTransport::~WebSocketSessionTransport() { Close(); }

void WebSocketSessionTransport::Start() {
    auto weak_self = std::weak_ptr<WebSocketSessionTransport>(std::static_pointer_cast<WebSocketSessionTransport>(shared_from_this()));
    ws_.SetCallbacks(
        [weak_self](std::string_view msg) {
            auto self = weak_self.lock();
            if (!self) return;
            try {
                auto parsed = DeserializeMessage(msg);
                self->WriteMessage(std::move(parsed));
            } catch (const std::exception& e) {
                MCP_LOG(Error, std::string("WebSocket parse error: ") + e.what());
                self->NotifyError(std::string("WebSocket parse error: ") + e.what());
            }
        },
        [weak_self]() {
            auto self = weak_self.lock();
            if (!self) return;
            self->running_ = false;
            self->SetDisconnected();
        },
        [weak_self](std::string_view message) {
            auto self = weak_self.lock();
            if (!self) return;
            MCP_LOG(Error, std::string("WebSocket error: ") + std::string(message));
            self->NotifyError(message);
        });

    // 自研客户端无 onopen：握手成功即读循环；连接失败经 on_error/on_close 回退
    running_ = true;
    SetConnected();
    ws_.Open(url_, std::chrono::seconds(30), true);
}

void WebSocketSessionTransport::Close() {
    running_ = false;
    ws_.SetCallbacks(nullptr, nullptr, nullptr);
    ws_.Close();
    if (channel_)
        channel_->Close();
    SetDisconnected();
}

void WebSocketSessionTransport::SendMessageAsync(JsonRpcMessage message) {
    if (!running_)
        return;
    auto json_str = SerializeMessage(std::move(message));
    ws_.Send(json_str);
}

} // namespace

WebSocketClientTransport::WebSocketClientTransport(std::string url, std::string name)
    : url_(std::move(url)), name_(std::move(name)) {}

WebSocketClientTransport::~WebSocketClientTransport() = default;

std::string_view WebSocketClientTransport::Name() const { return name_; }

std::shared_ptr<ITransport> WebSocketClientTransport::Connect() {
    auto session = std::make_shared<WebSocketSessionTransport>(url_);
    session->Start();
    return session;
}

} // namespace mcp
