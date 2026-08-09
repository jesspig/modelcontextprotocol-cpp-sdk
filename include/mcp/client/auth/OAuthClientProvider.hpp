#pragma once
// OAuthClientProvider.hpp
// OAuth 2.0 client provider with PKCE flow and token management
#include <mcp/JsonValue.hpp>
#include <mcp/client/auth/TokenCache.hpp>
#include <mcp/McpTypes.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace mcp {

// Result of the authorization redirect: the authorization code and the
// `state` value echoed back by the authorization server.
struct AuthorizationCodeResult {
    std::string code;
    std::string state;
};

// ── OAuthClientOptions (对应 C# ClientOAuthOptions) ──
// NOTE: OAuthClientProvider is NOT thread-safe — call its methods from a
// single thread only.
struct OAuthClientOptions {
    std::string server_url;
    std::string redirect_uri;
    std::optional<std::string> client_id;
    std::optional<std::string> client_secret;
    std::vector<std::string> scopes;
    std::shared_ptr<ITokenCache> token_cache;

    // Callbacks
    std::function<void(std::string_view url)> authorization_redirect_handler;
    // Returns the authorization code plus the `state` echoed back by the
    // authorization server (CSRF protection). Return nullopt on failure.
    std::function<std::optional<AuthorizationCodeResult>()> authorization_code_callback;
};

// ── OAuthClientProvider (对应 C# ClientOAuthProvider) ──
// Manages the full OAuth lifecycle:
//   1. Metadata discovery
//   2. Dynamic client registration
//   3. Authorization code + PKCE
//   4. Token exchange & refresh
//   5. Auto-attach Bearer token to requests
//
// Usage (matching C# pattern):
//   OAuthClientProvider auth(options);
//   auth.Authenticate();
//   auto token = auth.GetAccessToken();
class OAuthClientProvider {
public:
    explicit OAuthClientProvider(OAuthClientOptions options);
    ~OAuthClientProvider();

    // ── OAuth flow ──
    // Full authenticate: discover → register → authorize → token exchange
    // Returns false on failure.
    bool Authenticate();

    // ── Machine-to-machine (client credentials, RFC 6749 §4.4) ──
    // Obtains a token without user interaction. Requires client_id and
    // client_secret. Returns false on failure.
    bool AuthenticateClientCredentials();

    // ── Get valid access token (auto-refresh if needed) ──
    std::string GetAccessToken();

    // ── Token refresh ──
    bool RefreshTokens();

    // ── Revoke / logout ──
    void Revoke();

    // ── Check auth state ──
    bool IsAuthenticated() const;
    bool HasToken() const;

    // ── Apply Bearer auth to an HTTP request ──
    // Returns "Bearer {token}" string
    std::string GetAuthorizationHeader() const;

    // ── Step-up authorization (SEP-2350) ──
    // Some operations require additional scopes.
    bool StepUpAuthorization(const std::vector<std::string>& additional_scopes);

    // ── Protected Resource Metadata (RFC 9728) ──
    // Parses a WWW-Authenticate challenge advertising a resource_metadata
    // URI, fetches it, and merges any discovered endpoints into the known
    // metadata. Returns true when metadata was refreshed.
    bool HandleAuthChallenge(std::string_view www_authenticate);

private:
    // Internal helpers
    bool DiscoverMetadata();
    bool RegisterClient();
    bool StartAuthorizationFlow();
    bool ExchangeCodeForToken(std::string_view code, std::string_view code_verifier);
    // Parse a token endpoint response and persist it. Returns false when the
    // response lacks the access_token.
    bool ParseAndStoreTokenResponse(const JsonValue& json);

    // RFC 9207: validate issuer parameter in token response
    bool ValidateTokenIssuer(const JsonValue& response) const;

    // HTTP POST helper (minimal, for OAuth endpoints only)
    JsonValue HttpPost(
        std::string_view url,
        const std::map<std::string, std::string>& form_data);

    OAuthClientOptions options_;
    std::shared_ptr<ITokenCache> token_cache_;

    // Discovered metadata
    std::optional<OAuthMetadata> metadata_;
    std::optional<ClientRegistrationInfo> registration_;

    // PKCE state
    std::string code_verifier_;
    std::string code_challenge_;

    // CSRF state (RFC 6749 §10.12)
    std::string state_;
};

// ── PKCE helpers ──
namespace pkce {

// Generate a cryptographic random code verifier (43-128 chars)
std::string GenerateCodeVerifier();

// Compute S256 code challenge from verifier
std::string ComputeCodeChallenge(std::string_view code_verifier);

// Base64url encode (no padding)
std::string Base64UrlEncode(std::string_view input);

} // namespace pkce

} // namespace mcp
