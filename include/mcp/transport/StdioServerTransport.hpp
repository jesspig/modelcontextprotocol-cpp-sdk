#pragma once

#include <mcp/Export.hpp>

#include <mcp/Transport.hpp>
#include <mcp/transport/detail/PlatformIO.hpp>
#include <atomic>
#include <memory>
#include <thread>

namespace mcp {

class MCP_API StdioServerTransport : public TransportBase {
public:
    StdioServerTransport();
    ~StdioServerTransport() override;

    void Start();
    void Close() override;
    void SendMessageAsync(JsonRpcMessage message) override;

private:
    void ReadLoop();

    std::thread read_thread_;
    std::atomic<bool> running_{false};
    std::unique_ptr<detail::PipeHandle> stdin_pipe_;
    std::unique_ptr<detail::PipeHandle> stdout_pipe_;
};

} // namespace mcp
