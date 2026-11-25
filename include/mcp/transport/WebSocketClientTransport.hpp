#pragma once

#include <mcp/Transport.hpp>
#include <mcp/JsonRpc.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace mcp {

class WebSocketClientTransport : public IClientTransport {
public:
    WebSocketClientTransport(std::string url, std::string name = "websocket");
    ~WebSocketClientTransport() override;

    std::string_view Name() const override;
    std::shared_ptr<ITransport> Connect() override;

private:
    std::string url_;
    std::string name_;
    std::shared_ptr<detail::SessionLifecycle> lifecycle_ =
        std::make_shared<detail::SessionLifecycle>();
    std::shared_ptr<ITransport> current_;
    uint64_t current_generation_ = 0;
    std::mutex connect_mutex_;
};

} // namespace mcp
