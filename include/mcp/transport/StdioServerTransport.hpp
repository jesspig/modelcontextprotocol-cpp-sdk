#pragma once

#include <mcp/Transport.hpp>
#include <asio/io_context.hpp>
#include <atomic>
#include <memory>
#include <thread>

namespace mcp {

class StdioServerTransport : public Transport {
public:
    explicit StdioServerTransport(asio::io_context& io_ctx);
    ~StdioServerTransport() override;

    void Start() override;
    void Close() override;
    void SendMessageAsync(JsonRpcMessage message) override;
    MessageChannel& GetMessageChannel() override;
    asio::io_context& IoContext() override;

private:
    void ReadLoop();

    asio::io_context& io_ctx_;
    MessageChannel channel_;
    std::thread read_thread_;
    std::atomic<bool> running_{false};
};

} // namespace mcp
