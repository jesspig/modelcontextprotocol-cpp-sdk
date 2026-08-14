// OAuthTests — unit tests for PKCE, token cache, and OAuth client provider

#include <mcp/JsonRpc.hpp>
#include <mcp/client/auth/OAuthClientProvider.hpp>
#include <mcp/client/auth/TokenCache.hpp>
#include <mcp/http/HttpServer.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>
#include "TestServerUtil.hpp"

#include <atomic>
#include <chrono>
#include <mcp/test/McpTest.hpp>

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
    EXPECT_GE(v1.size(), 43U);
    EXPECT_LE(v1.size(), 128U);
}

TEST(OAuthTest, PkceComputeCodeChallenge) {
    // RFC 7636 Appendix B test vector
    std::string fixed_verifier =
        "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    auto challenge = pkce::ComputeCodeChallenge(fixed_verifier);
    EXPECT_EQ(challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
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

// ── client_credentials grant (RFC 6749 §4.4, M2M) ──
TEST(OAuthTest, ClientCredentialsGrant) {
    auto port = PickFreePort(kTestBasePort + 700);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/token",
        [issuer](const mcp::HttpRequest& req, mcp::HttpResponse& resp) {
            EXPECT_NE(req.body.find("grant_type=client_credentials"), std::string::npos);
            EXPECT_NE(req.body.find("client_id=test-client"), std::string::npos);
            resp.body = "{\"access_token\":\"m2m-token\",\"token_type\":\"Bearer\","
                        "\"expires_in\":3600,\"iss\":\"" + issuer + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "test-client";
    opts.client_secret = "test-secret";
    OAuthClientProvider provider(opts);

    ASSERT_TRUE(provider.AuthenticateClientCredentials());
    EXPECT_EQ(provider.GetAccessToken(), "m2m-token");
    server.Stop();
}

// ── 401 challenge → auth_challenge_handler → retry with Authorization ──
TEST(OAuthTest, AuthChallengeTriggersRetryWithAuthorization) {
    auto port = PickFreePort(kTestBasePort + 800);
    std::atomic<int> calls{0};
    std::string second_auth;

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/mcp",
        [&calls, &second_auth](const mcp::HttpRequest& req, mcp::HttpResponse& resp) {
            if (calls.fetch_add(1) == 0) {
                resp.status_code = 401;
                resp.status_text = "Unauthorized";
                resp.headers["WWW-Authenticate"] =
                    "Bearer resource_metadata=\"http://127.0.0.1:1/meta\", error=\"insufficient_scope\"";
                return;
            }
            auto it = req.headers.find("authorization");
            if (it != req.headers.end()) second_auth = it->second;
            resp.body = R"({"jsonrpc":"2.0","id":1,"result":{"resultType":"complete"}})";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    mcp::HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    opts.auth_challenge_handler = [](std::string_view www_auth) -> std::string {
        EXPECT_NE(www_auth.find("resource_metadata="), std::string::npos);
        return "Bearer retried-token";
    };
    auto transport = std::make_shared<mcp::StreamableHttpClientTransport>(opts);
    auto session = transport->Connect();

    mcp::JsonRpcRequest rpc;
    rpc.id = mcp::RequestId{int64_t(1)};
    rpc.method = "ping";
    session->SendMessageAsync(mcp::JsonRpcMessage{rpc});

    std::error_code ec;
    mcp::JsonRpcMessage resp_msg;
    session->GetMessageChannel().AsyncReceive(
        [&ec, &resp_msg](std::error_code e, mcp::JsonRpcMessage m) {
            ec = e; resp_msg = std::move(m);
        });
    ASSERT_FALSE(ec);
    EXPECT_EQ(calls.load(), 2);
    EXPECT_EQ(second_auth, "Bearer retried-token");

    session->Close();
    server.Stop();
}
