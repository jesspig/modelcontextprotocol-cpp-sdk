#pragma once

#include <mcp/Transport.hpp>

namespace mcp {

class StreamableHttpServerTransport : public Transport {
public:
    StreamableHttpServerTransport();
    ~StreamableHttpServerTransport() override;

    void Start() override;
    void Close() override;
    void SendMessageAsync(JsonRpcMessage message) override;
    MessageChannel& GetMessageChannel() override;
    asio::io_context& IoContext() override;
};

} // namespace mcp
