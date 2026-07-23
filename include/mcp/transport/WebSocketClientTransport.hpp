#pragma once

#include <mcp/Transport.hpp>
#include <mcp/JsonRpc.hpp>

#include <memory>
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
};

} // namespace mcp
