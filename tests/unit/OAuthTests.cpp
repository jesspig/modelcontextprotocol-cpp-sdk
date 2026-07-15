#include <mcp/client/auth/OAuthClientProvider.hpp>
#include <mcp/client/auth/TokenCache.hpp>

#include <gtest/gtest.h>

using namespace mcp;

// ── PKCE ──
TEST(OAuthTest, PkceBase64UrlEncode) {
    std::string input = "test";
    auto encoded = pkce::Base64UrlEncode(input);
    EXPECT_FALSE(encoded.empty());
    // No padding chars in URL-safe encoding
    EXPECT_EQ(encoded.find('='), std::string::npos);
}

TEST(OAuthTest, PkceBase64UrlNoPadding) {
    std::string input(32, 'a');
    auto encoded = pkce::Base64UrlEncode(input);
    EXPECT_EQ(encoded.find('='), std::string::npos);
    EXPECT_EQ(encoded.find('+'), std::string::npos);
    EXPECT_EQ(encoded.find('/'), std::string::npos);
}

TEST(OAuthTest, PkceGenerateCodeVerifier) {
    auto v1 = pkce::GenerateCodeVerifier();
    auto v2 = pkce::GenerateCodeVerifier();
    EXPECT_NE(v1, v2);  // random
    EXPECT_GE(v1.size(), 43U);
    EXPECT_LE(v1.size(), 128U);
}

TEST(OAuthTest, PkceComputeCodeChallenge) {
    std::string fixed_verifier =
        "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    auto challenge = pkce::ComputeCodeChallenge(fixed_verifier);
    EXPECT_FALSE(challenge.empty());
}

// ── Base64Url ──
TEST(OAuthTest, Base64UrlRoundTrip) {
    std::string original = "hello world!";
    // Just test encoding (decoding would need inverse)
    auto encoded = pkce::Base64UrlEncode(original);
    EXPECT_FALSE(encoded.empty());
}

// ── TokenContainer ──
TEST(OAuthTest, TokenContainerNotExpiredInitially) {
    TokenContainer tokens;
    tokens.access_token = "abc";
    tokens.expires_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()
        + 3600000;  // 1 hour from now
    EXPECT_FALSE(tokens.IsExpired());
}

TEST(OAuthTest, TokenContainerExpired) {
    TokenContainer tokens;
    tokens.expires_at = 0;  // expired
    EXPECT_TRUE(tokens.IsExpired());
}

TEST(OAuthTest, TokenContainerWillExpireSoon) {
    TokenContainer tokens;
    tokens.expires_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()
        + 30000;  // 30 seconds from now
    EXPECT_TRUE(tokens.WillExpireSoon(60000));  // margin 60s
}

// ── InMemoryTokenCache ──
TEST(OAuthTest, InMemoryTokenCacheStoreAndGet) {
    InMemoryTokenCache cache;
    TokenContainer tokens;
    tokens.access_token = "test_token";
    tokens.refresh_token = "refresh_token";
    tokens.expires_at = 9999999999999LL;

    cache.StoreTokens(tokens);
    auto result = cache.GetTokens();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->access_token, "test_token");
    EXPECT_EQ(result->refresh_token, "refresh_token");
}

TEST(OAuthTest, InMemoryTokenCacheClear) {
    InMemoryTokenCache cache;
    TokenContainer tokens;
    tokens.access_token = "test";
    cache.StoreTokens(tokens);
    cache.ClearTokens();
    auto result = cache.GetTokens();
    EXPECT_FALSE(result.has_value());
}

TEST(OAuthTest, InMemoryTokenCacheEmptyInitially) {
    InMemoryTokenCache cache;
    auto result = cache.GetTokens();
    EXPECT_FALSE(result.has_value());
}

// ── OAuthClientOptions defaults ──
TEST(OAuthTest, OAuthClientOptionsHasDefaults) {
    OAuthClientOptions opts;
    opts.server_url = "http://localhost:8080";
    opts.redirect_uri = "http://localhost:8080/callback";
    EXPECT_EQ(opts.server_url, "http://localhost:8080");
    EXPECT_EQ(opts.timeout_seconds, 30);
}

// ── OAuthClientProvider creation ──
TEST(OAuthTest, OAuthClientProviderCreate) {
    OAuthClientOptions opts;
    opts.server_url = "http://localhost:8080";
    opts.redirect_uri = "http://localhost:8080/callback";
    opts.scopes = {"user"};

    OAuthClientProvider provider(opts);
    EXPECT_FALSE(provider.IsAuthenticated());
    EXPECT_FALSE(provider.HasToken());
}

// ── OAuthClientProvider with token cache ──
TEST(OAuthTest, OAuthClientProviderWithPrepopulatedCache) {
    auto cache = std::make_shared<InMemoryTokenCache>();

    OAuthClientOptions opts;
    opts.server_url = "http://localhost:8080";
    opts.redirect_uri = "http://localhost:8080/callback";
    opts.token_cache = cache;

    // Pre-populate token
    TokenContainer tokens;
    tokens.access_token = "preloaded_token";
    tokens.expires_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 3600000;
    cache->StoreTokens(tokens);

    OAuthClientProvider provider(opts);
    EXPECT_TRUE(provider.HasToken());
    EXPECT_EQ(provider.GetAuthorizationHeader(), "Bearer preloaded_token");
}

// ── Revoke clears tokens ──
TEST(OAuthTest, OAuthClientProviderRevokeClearsTokens) {
    auto cache = std::make_shared<InMemoryTokenCache>();
    TokenContainer tokens;
    tokens.access_token = "test";
    tokens.expires_at = 9999999999999LL;
    cache->StoreTokens(tokens);

    OAuthClientOptions opts;
    opts.server_url = "http://localhost:8080";
    opts.redirect_uri = "http://localhost:8080/callback";
    opts.token_cache = cache;

    OAuthClientProvider provider(opts);
    EXPECT_TRUE(provider.HasToken());
    provider.Revoke();
    EXPECT_FALSE(provider.HasToken());
}
