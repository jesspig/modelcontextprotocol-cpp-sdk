// WebSocketClientTransport.cpp — WebSocket client transport implementation

#include <mcp/transport/WebSocketClientTransport.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/Log.hpp>

#include <hv/WebSocketClient.h>

#include <atomic>
#include <memory>
#include <string>

namespace mcp {

namespace {

class WebSocketSessionTransport : public TransportBase {
public:
    explicit WebSocketSessionTransport(std::string url);
    ~WebSocketSessionTransport() override;

    void Start();
    void Close() override;
    void SendMessageAsync(JsonRpcMessage message) override;

private:
    std::string url_;
    hv::WebSocketClient ws_;
    std::atomic<bool> running_{false};
};

WebSocketSessionTransport::WebSocketSessionTransport(std::string url)
    : url_(std::move(url)) {}

WebSocketSessionTransport::~WebSocketSessionTransport() { Close(); }

void WebSocketSessionTransport::Start() {
    auto weak_self = std::weak_ptr<WebSocketSessionTransport>(std::static_pointer_cast<WebSocketSessionTransport>(shared_from_this()));
    ws_.onopen = [weak_self]() {
        auto self = weak_self.lock();
        if (!self) return;
        self->running_ = true;
        self->SetConnected();
    };
    ws_.onclose = [weak_self]() {
        auto self = weak_self.lock();
        if (!self) return;
        self->running_ = false;
        self->SetDisconnected();
    };
    ws_.onmessage = [weak_self](const std::string& msg) {
        auto self = weak_self.lock();
        if (!self) return;
        try {
            auto parsed = DeserializeMessage(msg);
            self->WriteMessage(std::move(parsed));
        } catch (const std::exception& e) {
            MCP_LOG(Error, std::string("WebSocket parse error: ") + e.what());
            self->NotifyError(std::string("WebSocket parse error: ") + e.what());
        }
    };

    ws_.open(url_.c_str());
}

void WebSocketSessionTransport::Close() {
    running_ = false;
    ws_.onopen = nullptr;
    ws_.onclose = nullptr;
    ws_.onmessage = nullptr;
    ws_.close();
    if (channel_)
        channel_->Close();
    SetDisconnected();
}

void WebSocketSessionTransport::SendMessageAsync(JsonRpcMessage message) {
    if (!running_)
        return;
    auto json_str = SerializeMessage(std::move(message));
    ws_.send(json_str);
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
