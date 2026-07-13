#pragma once

#include <mcp/McpTypes.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace mcp {

// ── TokenContainer — access + refresh tokens ──
struct TokenContainer {
    std::string access_token;
    std::string refresh_token;
    std::vector<std::string> scopes;
    int64_t expires_at{0};  // unix timestamp ms
    std::string token_type{"Bearer"};

    bool IsExpired() const;
    bool WillExpireSoon(int64_t margin_ms = 60000) const;
};

// ── ITokenCache (对应 C# ITokenCache) ──
class ITokenCache {
public:
    virtual ~ITokenCache() = default;
    virtual void StoreTokens(const TokenContainer& tokens) = 0;
    virtual std::optional<TokenContainer> GetTokens() = 0;
    virtual void ClearTokens() = 0;
};

// ── InMemoryTokenCache (对应 C# InMemoryTokenCache) ──
class InMemoryTokenCache : public ITokenCache {
public:
    void StoreTokens(const TokenContainer& tokens) override;
    std::optional<TokenContainer> GetTokens() override;
    void ClearTokens() override;
private:
    std::optional<TokenContainer> tokens_;
    std::mutex mutex_;
};

// ── OAuthMetadata — authorization server metadata ──
struct OAuthMetadata {
    std::string issuer;
    std::string authorization_endpoint;
    std::string token_endpoint;
    std::optional<std::string> registration_endpoint;
    std::vector<std::string> scopes_supported;
    std::vector<std::string> grant_types_supported;
    std::vector<std::string> code_challenge_methods_supported;
    std::optional<std::string> jwks_uri;

    static std::optional<OAuthMetadata> Discover(std::string_view server_url);
};

// ── OAuthTokenResponse — token endpoint response ──
struct OAuthTokenResponse {
    std::string access_token;
    std::optional<std::string> refresh_token;
    int64_t expires_in{3600};
    std::string token_type{"Bearer"};
    std::optional<std::string> scope;
};

// ── ClientRegistrationInfo — DCR response ──
struct ClientRegistrationInfo {
    std::string client_id;
    std::optional<std::string> client_secret;
    std::vector<std::string> grant_types;
    std::vector<std::string> redirect_uris;

    static std::optional<ClientRegistrationInfo> Register(
        std::string_view registration_endpoint,
        std::string_view redirect_uri,
        std::string_view client_name);
};

} // namespace mcp
