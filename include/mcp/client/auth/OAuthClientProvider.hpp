#pragma once
#include <mcp/JsonValue.hpp>
#include <mcp/client/auth/TokenCache.hpp>
#include <mcp/McpTypes.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace mcp {

struct AuthorizationCodeResult {
    std::string code;
    std::string state;
    std::optional<std::string> iss;
};

struct OAuthClientOptions {
    std::string server_url;
    std::string redirect_uri;
    std::optional<std::string> client_id;
    std::optional<std::string> client_secret;
    std::vector<std::string> scopes;
    std::shared_ptr<ITokenCache> token_cache;
    std::optional<std::string> resource;

    std::function<void(std::string_view url)> authorization_redirect_handler;
    std::function<std::optional<AuthorizationCodeResult>()> authorization_code_callback;
};

class OAuthClientProvider {
public:
    explicit OAuthClientProvider(OAuthClientOptions options);
    ~OAuthClientProvider();

    bool Authenticate();

    bool AuthenticateClientCredentials();

    std::string GetAccessToken();

    bool RefreshTokens();

    void Revoke();

    bool IsAuthenticated() const;
    bool HasToken() const;

    std::string GetAuthorizationHeader() const;

    bool StepUpAuthorization(const std::vector<std::string>& additional_scopes);

    bool HandleAuthChallenge(std::string_view www_authenticate);

private:
    bool DiscoverMetadata();
    bool RegisterClient();
    bool StartAuthorizationFlow();
    bool ExchangeCodeForToken(std::string_view code, std::string_view code_verifier);
    bool ParseAndStoreTokenResponse(const JsonValue& json);

    bool ValidateTokenIssuer(const JsonValue& response) const;

    std::string ResolveResourceIndicator() const;

    bool FetchClientMetadataDocument();

    JsonValue HttpPost(
        std::string_view url,
        const std::map<std::string, std::string>& form_data);

    OAuthClientOptions options_;
    std::shared_ptr<ITokenCache> token_cache_;

    std::optional<OAuthMetadata> metadata_;
    std::optional<ClientRegistrationInfo> registration_;

    std::string code_verifier_;
    std::string code_challenge_;

    std::string state_;

    std::mutex refresh_mutex_;
};

namespace pkce {

std::string GenerateCodeVerifier();

std::string ComputeCodeChallenge(std::string_view code_verifier);

std::string Base64UrlEncode(std::string_view input);

} // namespace pkce

} // namespace mcp
