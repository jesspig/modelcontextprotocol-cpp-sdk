#pragma once

#include <mcp/Transport.hpp>

namespace mcp {

class StdioServerTransport : public Transport {
public:
    StdioServerTransport();
    ~StdioServerTransport() override;

    void Start() override;
    void Close() override;
    void SendMessageAsync(JsonRpcMessage message) override;
    MessageChannel& GetMessageChannel() override;
    asio::io_context& IoContext() override;
};

} // namespace mcp
