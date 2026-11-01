#include <mcp/Implementation.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/Transport.hpp>
#include <mcp/client/ClientOptions.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/client/auth/OAuthClientProvider.hpp>
#include <mcp/client/auth/TokenCache.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace mcp;

namespace add_oauth_to_a_client {

std::shared_ptr<ITokenCache> MakeCache() {
    return std::make_shared<InMemoryTokenCache>();
}

// region provider-options
OAuthClientOptions BaseOptions() {
    OAuthClientOptions options;
    options.server_url = "https://mcp.example.internal/mcp";
    options.redirect_uri = "https://localhost:8443/callback";
    options.client_id = "release-bot";
    options.scopes = {"mcp:tools", "offline_access"};
    options.token_cache = MakeCache();
    options.resource = "https://mcp.example.internal/mcp";
    options.application_type = "native";
    return options;
}

AuthorizationCodeResult WaitForCode() {
    AuthorizationCodeResult result;
    result.code = "stub-authorization-code";
    result.state = "stub-state";
    return result;
}

OAuthClientOptions WithBrowserFlow() {
    OAuthClientOptions options = BaseOptions();
    options.authorization_redirect_handler = [](std::string_view url) {
        (void)url;
    };
    options.authorization_code_callback = &WaitForCode;
    options.token_update_handler = [](const TokenContainer& tokens) {
        const std::size_t scope_count = tokens.scopes.size();
        (void)scope_count;
    };
    return options;
}
// endregion

// region client-credentials
OAuthClientOptions WithClientCredentials() {
    OAuthClientOptions options = BaseOptions();
    options.client_secret = "rotated-from-secret-manager";
    options.application_type = "confidential";
    return options;
}

bool PrepareToken(OAuthClientProvider& provider) {
    if (provider.HasToken()) {
        return !provider.GetAccessToken().empty();
    }
    if (provider.RefreshTokens()) {
        return true;
    }
    return provider.AuthenticateClientCredentials();
}

std::string AuthorizationValue(OAuthClientProvider& provider) {
    return provider.GetAuthorizationHeader();
}
// endregion

// region wire-transport
std::unique_ptr<McpClient> ConnectWithBearer(
    const std::shared_ptr<OAuthClientProvider>& provider) {
    HttpClientTransportOptions transport_options;
    transport_options.endpoint = "https://mcp.example.internal/mcp";
    transport_options.auth_challenge_handler =
        [provider](std::string_view www_authenticate) -> std::string {
            if (!provider->HandleAuthChallenge(www_authenticate)) {
                return std::string{};
            }
            return provider->GetAuthorizationHeader();
        };

    auto transport = StreamableHttpClientTransport(transport_options).Connect();

    ClientOptions options;
    options.client_info = Implementation{"release-bot", "1.4.0"};
    options.max_total_timeout = std::chrono::seconds(30);
    return McpClient::Create(std::move(transport), options);
}
// endregion

// region dpop
OAuthClientOptions WithDPoP(const std::string& private_key_pem) {
    OAuthClientOptions options = BaseOptions();
    options.enable_dpop = true;
    options.private_key_pem = private_key_pem;
    options.signing_algorithm = "ES256";
    return options;
}

std::unique_ptr<McpClient> ConnectWithDPoP(
    const std::shared_ptr<OAuthClientProvider>& provider) {
    HttpClientTransportOptions transport_options;
    transport_options.endpoint = "https://mcp.example.internal/mcp";
    transport_options.additional_headers["Authorization"] =
        "DPoP " + provider->GetAccessToken();
    transport_options.apply_per_request_headers =
        [provider](const HttpRequestHeaderContext& context,
                   std::map<std::string, std::string>& headers) {
            provider->ApplyDPoPProofHeader(context, headers);
        };
    transport_options.observe_response_challenge =
        [provider](const HttpResponseChallengeContext& context) {
            provider->ObserveDPoPNonce(context.dpop_nonce);
        };

    auto transport = StreamableHttpClientTransport(transport_options).Connect();
    return McpClient::Create(std::move(transport));
}
// endregion

// region step-up
bool EscalateOnChallenge(
    const std::shared_ptr<OAuthClientProvider>& provider,
    std::string_view www_authenticate,
    const std::vector<std::string>& extra_scopes) {
    if (!provider->HandleAuthChallenge(www_authenticate)) {
        return false;
    }
    return provider->StepUpAuthorization(extra_scopes);
}

OAuthClientOptions WithScopeFilter() {
    OAuthClientOptions options = BaseOptions();
    options.scope_filter = [](const std::vector<std::string>& advertised) {
        std::vector<std::string> requested;
        for (const std::string& scope : advertised) {
            if (scope.rfind("mcp:", 0) == 0) requested.push_back(scope);
        }
        return requested;
    };
    return options;
}
// endregion

// region endpoint-hygiene
bool EndpointLooksSafe(std::string_view url) {
    return OAuthClientProvider::IsSecureEndpoint(url);
}

bool SameAuthorizationServer(std::string_view lhs, std::string_view rhs) {
    return OAuthClientProvider::IsSameOrigin(lhs, rhs);
}

std::optional<OAuthMetadata> DiscoverMetadata() {
    return OAuthMetadata::Discover("https://mcp.example.internal/mcp");
}
// endregion

// region full
std::unique_ptr<McpClient> BuildAuthorisedClient() {
    auto provider = std::make_shared<OAuthClientProvider>(WithBrowserFlow());
    if (!provider->Authenticate() && !provider->HasToken()) {
        return nullptr;
    }

    HttpClientTransportOptions transport_options;
    transport_options.endpoint = "https://mcp.example.internal/mcp";
    transport_options.auth_challenge_handler =
        [provider](std::string_view www_authenticate) -> std::string {
            if (!provider->HandleAuthChallenge(www_authenticate)) {
                return std::string{};
            }
            if (provider->RefreshTokens()) {
                return provider->GetAuthorizationHeader();
            }
            (void)provider->Authenticate();
            return provider->GetAuthorizationHeader();
        };

    ClientOptions client_options;
    client_options.client_info = Implementation{"release-bot", "1.4.0"};
    client_options.connect_mode = ConnectMode::Auto;

    auto transport = StreamableHttpClientTransport(transport_options).Connect();
    return McpClient::Create(std::move(transport), client_options);
}
// endregion

} // namespace add_oauth_to_a_client
