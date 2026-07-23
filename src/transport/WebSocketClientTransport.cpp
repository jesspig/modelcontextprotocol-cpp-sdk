#include <mcp/transport/WebSocketClientTransport.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/Log.hpp>

#include <hv/WebSocketClient.h>

#include <atomic>
#include <memory>
#include <string>

namespace mcp {

namespace {

class WebSocketSessionTransport : public TransportBase, public std::enable_shared_from_this<WebSocketSessionTransport> {
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
    ws_.onopen = [this]() { SetConnected(); };
    ws_.onclose = [this]() { running_ = false; SetDisconnected(); };
    ws_.onmessage = [this](const std::string& msg) {
        try {
            auto parsed = DeserializeMessage(msg);
            WriteMessage(std::move(parsed));
        } catch (const std::exception& e) {
            MCP_LOG(Error, std::string("WebSocket parse error: ") + e.what());
            NotifyError(std::string("WebSocket parse error: ") + e.what());
        }
    };

    ws_.open(url_.c_str());
    running_ = true;
}

void WebSocketSessionTransport::Close() {
    running_ = false;
    ws_.close();
    if (channel_)
        channel_->Close();
    SetDisconnected();
}

void WebSocketSessionTransport::SendMessageAsync(JsonRpcMessage message) {
    if (!running_)
        return;
    auto json_str = SerializeMessage(message);
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
