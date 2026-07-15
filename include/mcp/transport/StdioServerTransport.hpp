#pragma once

#include <mcp/Export.hpp>

#include <mcp/Transport.hpp>
#include <asio/io_context.hpp>
#include <atomic>
#include <memory>
#include <thread>

namespace mcp {

class MCP_API StdioServerTransport : public TransportBase {
public:
    explicit StdioServerTransport(asio::io_context& io_ctx);
    ~StdioServerTransport() override;

    void Start();
    void Close() override;
    void SendMessageAsync(JsonRpcMessage message) override;

private:
    void ReadLoop();

    std::thread read_thread_;
    std::atomic<bool> running_{false};
};

} // namespace mcp
