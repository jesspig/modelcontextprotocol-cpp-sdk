#pragma once

#include <mcp/Transport.hpp>
#include <mcp/JsonRpc.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include <memory>
#include <queue>
#include <string>

namespace mcp {

// ── WebSocketClientTransport ──
// Implements IClientTransport for WebSocket connections.
// Performs TCP connect + WebSocket handshake in Connect().
class WebSocketClientTransport : public IClientTransport {
public:
    WebSocketClientTransport(
        asio::io_context& io_ctx,
        std::string host,
        std::string port,
        std::string path = "/",
        std::string name = "websocket");

    ~WebSocketClientTransport() override;

    // IClientTransport interface
    std::string_view Name() const override;
    std::shared_ptr<ITransport> Connect() override;

private:
    asio::io_context& io_ctx_;
    std::string host_;
    std::string port_;
    std::string path_;
    std::string name_;
};

// ── WebSocketTransport ──
// Implements ITransport for an active WebSocket connection.
// Reads/writes WebSocket frames over a connected TCP socket.
class WebSocketTransport : public TransportBase {
public:
    WebSocketTransport(
        std::shared_ptr<asio::io_context> io_ctx,
        asio::ip::tcp::socket socket);

    ~WebSocketTransport() override;

    // Starts read/write loops (must be called after construction)
    void Start();

    // ITransport interface
    void SendMessageAsync(JsonRpcMessage message) override;
    void Close() override;

private:
    void ReadLoop();
    void WriteLoop();

    std::shared_ptr<asio::io_context> io_ctx_;
    asio::ip::tcp::socket socket_;

    std::thread read_thread_;
    std::thread write_thread_;
    std::thread io_thread_;
    std::atomic<bool> running_{false};

    std::mutex write_mutex_;
    std::condition_variable write_cv_;
    std::queue<std::string> write_queue_;
};

} // namespace mcp
