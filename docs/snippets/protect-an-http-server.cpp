#include <mcp/Implementation.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/Transport.hpp>
#include <mcp/http/EventStore.hpp>
#include <mcp/http/SessionStore.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/storage/FileEventStore.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace mcp;

namespace protect_an_http_server {

const std::set<std::string>& KnownTokens() {
    static const std::set<std::string> tokens = {"tok-read-0001"};
    return tokens;
}

// region bearer-auth
StreamableHttpServerOptions WithBearerAuth() {
    StreamableHttpServerOptions options;
    options.port = 3010;
    options.endpoint = "/mcp";
    options.stateless = false;
    options.session_store = std::make_shared<InMemorySessionStore>();

    StreamableHttpServerOptions::BearerAuthConfig auth;
    auth.verify = [](const std::string& token) {
        AuthResult result;
        result.ok = KnownTokens().count(token) != 0;
        if (result.ok) result.scopes = {"mcp:tools"};
        return result;
    };
    auth.resource_metadata_url = "http://localhost:3010/.well-known/oauth-protected-resource";
    auth.scopes_supported = {"mcp:tools", "mcp:resources"};
    auth.required_scopes = {"mcp:tools"};
    auth.expected_resource = "http://localhost:3010/mcp";
    auth.validate_token_resource = true;
    auth.serve_metadata_endpoint = true;
    options.bearer_auth = auth;
    return options;
}

std::unique_ptr<McpServer> BuildSecuredServer(
    std::shared_ptr<StreamableHttpServerTransport>& out_transport) {
    auto transport = std::make_shared<StreamableHttpServerTransport>(WithBearerAuth());
    ServerOptions server_options;
    server_options.server_info = Implementation{"guarded-echo", "1.0.0"};
    auto server = McpServer::Create(transport, server_options);
    out_transport = transport;
    return server;
}
// endregion

// region dpop-server
StreamableHttpServerOptions WithDPoP() {
    StreamableHttpServerOptions options = WithBearerAuth();
    options.bearer_auth->enable_dpop = true;
    options.bearer_auth->authorization_servers = {"https://auth.example.internal"};
    return options;
}
// endregion

// region host-and-origin
StreamableHttpServerOptions PinHostAndOrigin() {
    StreamableHttpServerOptions options;
    options.host = "0.0.0.0";
    options.allowed_hosts = {"mcp.example.internal", "localhost:3010"};
    options.allowed_origins = {"https://console.example.internal"};
    return options;
}
// endregion

// region size-limits
StreamableHttpServerOptions TightenLimits() {
    StreamableHttpServerOptions options;
    options.max_request_body_bytes = std::optional<std::size_t>(262144);
    options.max_sse_event_bytes = std::optional<std::size_t>(65536);
    options.sse_keep_alive_ms = 10000;
    return options;
}
// endregion

// region stateless-mode
StreamableHttpServerOptions StatelessWithResume() {
    StreamableHttpServerOptions options;
    options.stateless = true;
    options.listen_hold_open = true;
    options.enable_legacy_sse = true;
    options.event_store = std::make_shared<FileEventStore>(
        std::filesystem::path("var/mcp-events"));
    options.session_store = nullptr;
    return options;
}
// endregion

// region param-headers
std::unique_ptr<McpServer> BuildWithParamAnnotations(
    std::shared_ptr<StreamableHttpServerTransport>& out_transport) {
    StreamableHttpServerOptions options = WithBearerAuth();
    McpServer* server_ptr = nullptr;
    options.resolve_param_annotations =
        [&server_ptr](const std::string& method, const std::string& name) {
            if (!server_ptr) return std::vector<McpParamAnnotationInfo>{};
            return server_ptr->ResolveToolParamAnnotations(method, name);
        };

    auto transport = std::make_shared<StreamableHttpServerTransport>(options);
    ServerOptions server_options;
    server_options.server_info = Implementation{"guarded-echo", "1.0.0"};
    auto server = McpServer::Create(transport, server_options);
    server_ptr = server.get();
    out_transport = transport;
    return server;
}
// endregion

// region full
std::unique_ptr<McpServer> BuildHardenedServer(
    std::shared_ptr<StreamableHttpServerTransport>& out_transport) {
    StreamableHttpServerOptions options = WithBearerAuth();
    options.allowed_hosts = {"localhost:3010"};
    options.allowed_origins = {"https://console.example.internal"};
    options.max_request_body_bytes = std::optional<std::size_t>(262144);
    options.enable_legacy_sse = true;

    McpServer* server_ptr = nullptr;
    options.resolve_param_annotations =
        [&server_ptr](const std::string& method, const std::string& name) {
            if (!server_ptr) return std::vector<McpParamAnnotationInfo>{};
            return server_ptr->ResolveToolParamAnnotations(method, name);
        };

    auto transport = std::make_shared<StreamableHttpServerTransport>(options);
    ServerOptions server_options;
    server_options.server_info = Implementation{"guarded-echo", "1.0.0"};
    server_options.declare_logging = true;
    auto server = McpServer::Create(transport, server_options);
    server_ptr = server.get();
    out_transport = transport;
    return server;
}
// endregion

} // namespace protect_an_http_server
