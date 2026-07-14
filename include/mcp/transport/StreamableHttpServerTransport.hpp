#pragma once

#include <mcp/Transport.hpp>
#include <mcp/http/HttpServer.hpp>
#include <mcp/http/EventStore.hpp>

#include <nlohmann/json.hpp>

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
class StreamableHttpServerTransport : public TransportBase {
public:
    StreamableHttpServerTransport(
        asio::io_context& io_ctx,
        StreamableHttpServerOptions options = {});

    ~StreamableHttpServerTransport() override;

    void Start();
    void Close() override;
    void SendMessageAsync(JsonRpcMessage message) override;
    bool IsStateless() const override { return options_.stateless; }

    // Validate Mcp-Method and Mcp-Name headers match body (SEP-2243)
    static bool ValidateMcpHeaders(
        const std::string& method_header,
        const std::string& name_header,
        const nlohmann::json& body,
        std::string& error_out);

private:
    // HTTP request handlers
    void HandlePost(const HttpRequest& req, HttpResponse& resp);
    void HandleGet(const HttpRequest& req, HttpResponse& resp);

    // Build SSE event data from a JSON-RPC message
    std::string BuildSseEvent(const JsonRpcMessage& msg);

    // Parse MCP headers from request
    std::string GetMcpHeader(const HttpRequest& req,
                             std::string_view header_name) const;

    StreamableHttpServerOptions options_;
    std::unique_ptr<HttpServer> http_server_;
    std::shared_ptr<EventStore> event_store_;
};

} // namespace mcp
