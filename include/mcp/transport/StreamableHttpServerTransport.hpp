#pragma once

#include <mcp/Transport.hpp>
#include <mcp/http/HttpServer.hpp>
#include <mcp/http/EventStore.hpp>

#include <memory>
#include <string>

namespace mcp {

// ── Options for StreamableHttpServerTransport ──
struct StreamableHttpServerOptions {
    uint16_t port{3001};
    std::string endpoint{"/mcp"};

    // 2026-07-28: stateless mode (no sessions)
    bool stateless{false};

    // Legacy SSE support (GET /mcp endpoint for SSE stream)
    bool enable_legacy_sse{true};

    // Event store for resumption
    std::shared_ptr<EventStore> event_store;

    // Server info for discovery
    std::string server_name{"mcp-server"};
    std::string server_version{"0.1.0"};
};

// ── StreamableHttpServerTransport ──
// HTTP transport implementing the MCP Streamable HTTP spec.
// Supports both 2026-07-28 (stateless) and legacy modes.
class StreamableHttpServerTransport : public Transport {
public:
    StreamableHttpServerTransport(
        asio::io_context& io_ctx,
        StreamableHttpServerOptions options = {});

    ~StreamableHttpServerTransport() override;

    void Start() override;
    void Close() override;
    void SendMessageAsync(JsonRpcMessage message) override;
    MessageChannel& GetMessageChannel() override;
    asio::io_context& IoContext() override;
    std::string_view SessionId() const override { return session_id_; }
    bool IsStateless() const override { return options_.stateless; }

private:
    // HTTP request handlers
    void HandlePost(const HttpRequest& req, HttpResponse& resp);
    void HandleGet(const HttpRequest& req, HttpResponse& resp);

    // Build SSE event data from a JSON-RPC message
    std::string BuildSseEvent(const JsonRpcMessage& msg);

    // Parse MCP headers from request
    std::string GetMcpHeader(const HttpRequest& req,
                             std::string_view header_name) const;

    asio::io_context& io_ctx_;
    StreamableHttpServerOptions options_;
    std::unique_ptr<HttpServer> http_server_;
    std::unique_ptr<MessageChannel> channel_;
    std::shared_ptr<EventStore> event_store_;
    std::string session_id_;
};

} // namespace mcp
