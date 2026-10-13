#pragma once
#include <mcp/JsonValue.hpp>
#include <mcp/client/auth/TokenCache.hpp>
#include <mcp/ext/DPoP.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    std::function<std::vector<std::string>(const std::vector<std::string>&)> scope_filter;
    std::shared_ptr<ITokenCache> token_cache;
    std::optional<std::string> resource;
    std::string application_type{"native"};
    bool enable_dpop{false};
    std::optional<std::string> private_key_pem;
    std::string signing_algorithm{"ES256"};
    std::function<int64_t()> client_assertion_clock_seconds;

    std::function<void(std::string_view url)> authorization_redirect_handler;
    std::function<std::optional<AuthorizationCodeResult>()> authorization_code_callback;
    std::function<void(const TokenContainer&)> token_update_handler;
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

    std::string BuildDPoPProof(std::string_view htm,
                               std::string_view htu,
                               std::optional<std::string_view> access_token = std::nullopt);

    void ApplyDPoPProofHeader(const HttpRequestHeaderContext& context,
                              std::map<std::string, std::string>& headers);

    void ObserveDPoPNonce(std::string_view dpop_nonce);

    void SetDPoPNonce(std::string nonce);

    bool StepUpAuthorization(const std::vector<std::string>& additional_scopes);

    bool HandleAuthChallenge(std::string_view www_authenticate);

private:
    bool DiscoverMetadata();
    bool RegisterClient();
    bool StartAuthorizationFlow();
    bool ExchangeCodeForToken(std::string_view code, std::string_view code_verifier);
    bool ParseAndStoreTokenResponse(const JsonValue& json);

    void RefreshTokensCoalesced();

    bool ValidateTokenIssuer(const JsonValue& response) const;

    bool CodeChallengeS256Supported() const;

    void NotifyTokenUpdate(const TokenContainer& tokens) const;

    bool TokenResourceMatches(const TokenContainer& tokens) const;

    void EnforceIssuerBinding();

    std::string ResolveResourceIndicator() const;

    std::vector<std::string> ResolveScopes() const;

    bool FetchClientMetadataDocument();

    bool PrivateKeyJwtAdvertised() const;

    bool UsesClientAssertion() const;

    std::string CreateClientAssertion() const;

    void ApplyClientAuthentication(std::map<std::string, std::string>& form) const;

    JsonValue HttpPost(
        std::string_view url,
        const std::map<std::string, std::string>& form_data);

    OAuthClientOptions options_;
    std::shared_ptr<ITokenCache> token_cache_;

    std::optional<OAuthMetadata> metadata_;
    std::optional<ClientRegistrationInfo> registration_;
    std::optional<std::string> bound_issuer_;

    std::string code_verifier_;
    std::string code_challenge_;

    std::string state_;

    std::optional<ext::dpop::EcP256Jwk> dpop_key_;
    std::optional<std::string> dpop_nonce_;
    mutable std::mutex dpop_mutex_;

    std::mutex refresh_mutex_;
    std::condition_variable refresh_cv_;
    bool refresh_in_flight_ = false;
};

namespace pkce {

std::string GenerateCodeVerifier();

std::string ComputeCodeChallenge(std::string_view code_verifier);

std::string Base64UrlEncode(std::string_view input);

} // namespace pkce

} // namespace mcp
