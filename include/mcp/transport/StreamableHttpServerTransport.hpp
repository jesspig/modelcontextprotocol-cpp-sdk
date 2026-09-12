#pragma once
// StreamableHttpServerTransport.hpp — HTTP server transport implementing the MCP Streamable HTTP spec

#include <mcp/Transport.hpp>
#include <mcp/http/HttpServer.hpp>
#include <mcp/http/EventStore.hpp>
#include <mcp/http/SessionStore.hpp>

#include <mcp/JsonValue.hpp>
#include <mcp/McpVersion.hpp>

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mcp {

// ── Defaults for StreamableHttpServerTransport ──
inline constexpr uint16_t kDefaultPort = 3001;

// ── RFC 6750 bearer token verification outcome ──
struct AuthResult {
    bool ok{false};
    std::vector<std::string> scopes;
};

// ── Options for StreamableHttpServerTransport ──
struct StreamableHttpServerOptions {
    uint16_t port{kDefaultPort};
    std::string endpoint{"/mcp"};

    // 2026-07-28: stateless mode (no sessions) is the default
    bool stateless{true};

    // Legacy SSE support (GET /mcp endpoint for SSE stream)
    bool enable_legacy_sse{true};

    // SSE keepalive comment-frame interval in ms (0 disables; default 15s)
    int sse_keep_alive_ms{15000};

    // Event store for resumption
    std::shared_ptr<EventStore> event_store;

    // External session store (stateful mode only): sessions become adoptable
    // by other instances sharing the same store
    std::shared_ptr<SessionStore> session_store;

    // Server info for discovery
    std::string server_name{"mcp-server"};
    std::string server_version{std::string(kSdkVersion)};

    // 监听绑定地址，透传至 HttpServerOptions::bind_host。
    // 空字符串 = 所有接口 (INADDR_ANY)。接受 IPv4/IPv6 字面量（如 "127.0.0.1" / "::1" / "::"）。
    std::string host;

    // Bearer auth (RFC 6750/9728): setting `bearer_auth` enables the 401/403
    // challenge flow at the top of POST/GET, before any session handling.
    struct BearerAuthConfig {
        // Required: ok=false rejects with 401 error="invalid_token"
        std::function<AuthResult(const std::string& token)> verify;
        // Full URL of the protected-resource metadata document; also referenced
        // by the WWW-Authenticate challenge
        std::string resource_metadata_url;
        std::vector<std::string> scopes_supported;
        // All must be covered by token scopes; empty disables scope checks
        std::vector<std::string> required_scopes;
        std::vector<std::string> authorization_servers;
        // Register GET /.well-known/oauth-protected-resource (no auth)
        bool serve_metadata_endpoint{true};
    };
    std::optional<BearerAuthConfig> bearer_auth;
};

// ── StreamableHttpServerTransport ──
// HTTP transport implementing the MCP Streamable HTTP spec.
// Supports both 2026-07-28 (stateless) and legacy modes.
class StreamableHttpServerTransport : public TransportBase {
public:
    StreamableHttpServerTransport(
        StreamableHttpServerOptions options = {});

    ~StreamableHttpServerTransport() override;

    void Start() override;
    void Close() override;
    void SendMessageAsync(JsonRpcMessage message) override;
    bool IsStateless() const override { return options_.stateless; }

    // Validate Mcp-Method and Mcp-Name headers match body (SEP-2243)
    static bool ValidateMcpHeaders(
        const std::string& method_header,
        const std::string& name_header,
        const JsonValue& body,
        std::string& error_out);

private:
    // HTTP request handlers
    void HandlePost(const HttpRequest& req, HttpResponse& resp);
    void HandleGet(const HttpRequest& req, HttpResponse& resp);

    // Bearer auth gate (RFC 6750): false means resp carries 401/403
    bool AuthorizeRequest(const HttpRequest& req, HttpResponse& resp);
    void HandleMetadataRequest(HttpResponse& resp);

    // Build SSE event data from a JSON-RPC message
    std::string BuildSseEvent(JsonRpcMessage msg);

    // Parse MCP headers from request
    std::optional<std::string> GetMcpHeader(const HttpRequest& req,
                                            std::string_view header_name) const;

    // Session adoption via external SessionStore (stateful mode only);
    // returns false when the response has been filled with a 404
    bool EnsureSession(const HttpRequest& req, HttpResponse& resp);
    std::string ActiveSessionId() const;
    void AdoptSession(const std::string& session_id);

    // Convert RequestId to string key for response correlation
    static std::string RequestIdToString(const RequestId& id);

    StreamableHttpServerOptions options_;
    std::unique_ptr<HttpServer> http_server_;
    std::shared_ptr<EventStore> event_store_;
    std::shared_ptr<SessionStore> session_store_;
    mutable std::mutex session_state_mutex_;

    // Stateless mode: in-flight synchronous response waiters (bounds worker occupancy)
    std::atomic<size_t> stateless_inflight_{0};
    // Stateless mode: pending request-response correlation
    std::unordered_map<std::string, std::shared_ptr<std::promise<JsonRpcMessage>>> pending_responses_;
    std::mutex pending_mutex_;
};

} // namespace mcp
