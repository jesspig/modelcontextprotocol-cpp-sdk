#include <mcp/JsonValue.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/ServerRequestContext.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/client/SkillVerify.hpp>
#include <mcp/client/auth/OAuthClientProvider.hpp>
#include <mcp/client/auth/TokenCache.hpp>
#include <mcp/ext/DPoP.hpp>
#include <mcp/ext/ServerCardClient.hpp>
#include <mcp/http/HttpServer.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/transport/HttpTransportMode.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mcp_docs {
using namespace mcp;
namespace {

// region oauth-strict
OAuthClientOptions StrictOAuthOptions() {
    OAuthClientOptions options;
    options.server_url = "https://as.example.com/mcp";
    options.resource = "https://as.example.com/mcp";
    options.require_iss_when_advertised = true;
    options.require_pkce_metadata = true;
    options.allow_legacy_metadata_fallback = false;

    auto metadata = OAuthMetadata::Discover(
        options.server_url, false, std::string_view("https://as.example.com/mcp"));
    (void)metadata;
    return options;
}

OAuthClientOptions LegacyOAuthOptionsForMigration() {
    OAuthClientOptions options;
    options.server_url = "https://as.example.com/mcp";
    options.require_iss_when_advertised = false;
    options.require_pkce_metadata = false;
    options.allow_legacy_metadata_fallback = true;

    auto metadata = OAuthMetadata::Discover(options.server_url, true);
    (void)metadata;
    return options;
}
// endregion

// region oauth-network
void ConfigureOutboundOrigins(OAuthClientOptions& options) {
    options.allowed_authorization_server_origins = {"https://as.example.com"};
    options.outbound_origin_allowlist =
        std::vector<std::string>{"https://auth.example.com"};
    options.loopback_port_exceptions = {{"127.0.0.1", 3001}};
    options.allow_cross_origin_redirect = false;
}
// endregion

// region tasks-modern
void BindPrincipalAndSubscribe(McpServer& server, McpClient& client,
                               const std::string& task_id) {
    server.SetDefaultPrincipal("alice");

    SubscriptionsListenRequestParams params;
    params.notifications.task_ids.push_back(task_id);
    client.SubscribeAsync(params);
}
// endregion

// region skills-yaml
SkillReadResult ReadSkillWithParts(McpClient& client) {
    CacheableRequestOptions options;
    const std::string uri = "https://example.com/skills/demo/SKILL.md";

    SkillReadResult frontmatter =
        client.ReadSkill(uri, options, SkillReadParts::FrontmatterOnly);
    (void)frontmatter;
    return client.ReadSkill(uri, options, SkillReadParts::All);
}

bool ResolveLocalSkillPath(const std::string& root, std::string& resolved) {
    std::string error;
    return ResolveSkillLocalPath(root, "notes/SKILL.md", resolved, error);
}
// endregion

// region session-context
void ConfigureAuthenticatedSessions(StreamableHttpServerOptions& http_options,
                                    McpServer& server) {
    http_options.bearer_auth = StreamableHttpServerOptions::BearerAuthConfig{};
    http_options.bearer_auth->verify = [](const std::string& token) -> AuthResult {
        AuthResult result;
        result.ok = !token.empty();
        if (result.ok) {
            result.principal = "alice";
            result.scopes = {"tools:read"};
        }
        return result;
    };
    http_options.on_authenticated = [&server](const ServerRequestContext& context) {
        if (context.IsSessionful()) {
            server.SetSessionPrincipal(context.SessionId(), context.Principal());
        }
    };
}
// endregion

// region http-bind-autodetect
HttpServerOptions BindLoopbackOnly() {
    HttpServerOptions options;
    options.bind_host = "127.0.0.1";
    return options;
}

HttpServerOptions BindAllInterfaces() {
    HttpServerOptions options;
    options.bind_host = "0.0.0.0";
    return options;
}

HttpClientTransportOptions UseExplicitTransport(std::string endpoint) {
    HttpClientTransportOptions options;
    options.endpoint = std::move(endpoint);
    options.transport_mode = HttpTransportMode::StreamableHttp;
    return options;
}
// endregion

// region dpop-cache-snapshot
void ObserveDpopAndSnapshot(OAuthClientProvider& provider, McpClient& client) {
    provider.SetDPoPNonceContext("https://as.example.com/token", "DPoP");
    auto nonce = provider.GetDPoPNonce("https://as.example.com/token", "DPoP");
    (void)nonce;

    auto snapshot = client.CurrentNegotiationSnapshot();
    (void)snapshot;
}
// endregion

// region file-input-and-server-card
ServerOptions ConfigureFileInputAllowlist() {
    ServerOptions options;
    options.enable_file_input = true;
    ServerOptions::FileInputConfig config;
    config.allowed_roots = {"./allowed"};
    config.max_refs = 16;
    config.max_file_bytes = 8 * 1024 * 1024;
    config.max_total_bytes = 8 * 1024 * 1024;
    config.max_inline_bytes = 256 * 1024;
    options.file_input_config = std::move(config);
    return options;
}

ext::ServerCard ParseCardDocument(const std::string& body) {
    return ext::ParseServerCard(body);
}
// endregion

// region jsonrpc-request
RequestHandle SendWellFormedRequest(McpClient& client, std::string method,
                                    JsonValue params) {
    RequestOptions options;
    options.timeout = std::chrono::milliseconds(5000);
    return client.SendRequest(method, std::move(params), options);
}
// endregion

} // namespace
} // namespace mcp_docs
