#pragma once

#include <mcp/Export.hpp>

#include <mcp/Transport.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <mcp/transport/detail/PlatformIO.hpp>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <thread>

namespace mcp {

class MCP_API StdioServerTransport : public TransportBase {
public:
    StdioServerTransport();
    explicit StdioServerTransport(std::optional<std::size_t> max_frame_bytes);
    ~StdioServerTransport() override;

    void Start() override;
    void Close() override;
    void SendMessageAsync(JsonRpcMessage message) override;

private:
    void ReadLoop();

    std::thread read_thread_;
    std::atomic<bool> running_{false};
    std::unique_ptr<detail::PipeHandle> stdin_pipe_;
    std::unique_ptr<detail::PipeHandle> stdout_pipe_;
    std::size_t max_frame_bytes_{detail::kMaxStdioFrameBytes};
};

} // namespace mcp
