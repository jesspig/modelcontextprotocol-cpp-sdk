#include <mcp/JsonRpc.hpp>
#include <mcp/McpError.hpp>
#include <mcp/ErrorCodes.hpp>
#include <mcp/Log.hpp>
#include <mcp/client/auth/OAuthClientProvider.hpp>
#include <mcp/client/auth/TokenCache.hpp>
#include <mcp/detail/Base64Url.hpp>
#include <mcp/ext/DPoP.hpp>
#include <mcp/ext/Triggers.hpp>
#include <mcp/http/HttpServer.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>
#include "TestServerUtil.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include <mcp/test/McpTimeout.hpp>

using namespace mcp;

static const char* kTestEs256PrivateKeyPem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQghQsGvPGMif2reGCQ\n"
    "wHJKINXk9g1ky3LLtvxpU4UyxFehRANCAAQeMXIdwkqCzhCdxoIHxLjNmhr0ktzK\n"
    "8yEDCPaBCgpxhUlak78RuSXGlHp/elZywhDYrSDJsYd5uu5eDW4/I18y\n"
    "-----END PRIVATE KEY-----\n";

TEST(OAuthTest, PkceBase64UrlEncode) {
    std::string input = "test";
    auto encoded = pkce::Base64UrlEncode(input);
    EXPECT_FALSE(encoded.empty());
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
    std::string fixed_verifier =
        "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    auto challenge = pkce::ComputeCodeChallenge(fixed_verifier);
    EXPECT_EQ(challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}

TEST(OAuthTest, TokenContainerNotExpiredInitially) {
    TokenContainer tokens;
    tokens.access_token = "abc";
    tokens.expires_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()
        + 3600000;
    EXPECT_FALSE(tokens.IsExpired());
}

TEST(OAuthTest, TokenContainerExpired) {
    TokenContainer tokens;
    tokens.expires_at = 0;
    EXPECT_TRUE(tokens.IsExpired());
}

TEST(OAuthTest, TokenContainerWillExpireSoon) {
    TokenContainer tokens;
    tokens.expires_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()
        + 30000;
    EXPECT_TRUE(tokens.WillExpireSoon(60000));
}

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

TEST(OAuthTest, OAuthClientOptionsHasDefaults) {
    OAuthClientOptions opts;
    opts.server_url = "http://localhost:8080";
    opts.redirect_uri = "http://localhost:8080/callback";
    EXPECT_EQ(opts.server_url, "http://localhost:8080");
}

TEST(OAuthTest, OAuthClientProviderCreate) {
    OAuthClientOptions opts;
    opts.server_url = "http://localhost:8080";
    opts.redirect_uri = "http://localhost:8080/callback";
    opts.scopes = {"user"};

    OAuthClientProvider provider(opts);
    EXPECT_FALSE(provider.IsAuthenticated());
    EXPECT_FALSE(provider.HasToken());
}

TEST(OAuthTest, OAuthClientProviderWithPrepopulatedCache) {
    auto cache = std::make_shared<InMemoryTokenCache>();

    OAuthClientOptions opts;
    opts.server_url = "http://localhost:8080";
    opts.redirect_uri = "http://localhost:8080/callback";
    opts.token_cache = cache;

    TokenContainer tokens;
    tokens.access_token = "preloaded_token";
    tokens.expires_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 3600000;
    cache->StoreTokens(tokens);

    OAuthClientProvider provider(opts);
    EXPECT_TRUE(provider.HasToken());
    EXPECT_EQ(provider.GetAuthorizationHeader(), "Bearer preloaded_token");
}

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
    opts.allow_legacy_metadata_fallback = true;
    OAuthClientProvider provider(opts);

    ASSERT_TRUE(provider.AuthenticateClientCredentials());
    EXPECT_EQ(provider.GetAccessToken(), "m2m-token");
    server.Stop();
}

TEST(OAuthTest, SignEs256JwtWithPemProducesCompactAssertion) {
    try {
        auto jwt = mcp::ext::dpop::detail::SignEs256JwtWithPem(
            R"({"alg":"ES256","typ":"JWT"})",
            R"({"sub":"jwt-client"})",
            kTestEs256PrivateKeyPem);
        EXPECT_EQ(std::count(jwt.begin(), jwt.end(), '.'), 2);
        auto dot = jwt.rfind('.');
        ASSERT_TRUE(dot != std::string::npos);
        auto signature = mcp::detail::Base64UrlDecode(jwt.substr(dot + 1));
        ASSERT_TRUE(signature.has_value());
        EXPECT_EQ(signature->size(), 64U);
    } catch (const McpError& e) {
        EXPECT_TRUE(e.Code() == McpErrorCode::InternalError);
    }
}

TEST(OAuthTest, SignEs256JwtWithPemRejectsUnparsableKey) {
    bool threw = false;
    try {
        mcp::ext::dpop::detail::SignEs256JwtWithPem(
            R"({"alg":"ES256","typ":"JWT"})",
            R"({"sub":"jwt-client"})",
            "-----BEGIN PRIVATE KEY-----\nnot-a-key\n-----END PRIVATE KEY-----\n");
    } catch (const McpError& e) {
        threw = true;
        EXPECT_TRUE(e.Code() == McpErrorCode::InvalidParams ||
                    e.Code() == McpErrorCode::InternalError);
    }
    EXPECT_TRUE(threw);
}

TEST(OAuthTest, PrivateKeyJwtClientCredentialsSendsAssertion) {
    auto port = PickFreePort(kTestBasePort + 1200);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::string seen_body;

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\","
                        "\"token_endpoint_auth_methods_supported\":"
                        "[\"private_key_jwt\",\"none\"]}";
        });
    server.SetHandler("POST", "/token",
        [&seen_body, issuer](const mcp::HttpRequest& req, mcp::HttpResponse& resp) {
            seen_body = req.body;
            resp.body = "{\"access_token\":\"jwt-token\",\"token_type\":\"Bearer\","
                        "\"expires_in\":3600,\"iss\":\"" + issuer + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "jwt-client";
    opts.private_key_pem = kTestEs256PrivateKeyPem;
    opts.signing_algorithm = "ES256";
    opts.client_assertion_clock_seconds = [] { return int64_t{1700000000}; };
    opts.allow_legacy_metadata_fallback = true;
    OAuthClientProvider provider(opts);

    try {
        ASSERT_TRUE(provider.AuthenticateClientCredentials());
        EXPECT_NE(seen_body.find(
            "client_assertion_type=urn%3Aietf%3Aparams%3Aoauth%3A"
            "client-assertion-type%3Ajwt-bearer"), std::string::npos);
        EXPECT_EQ(seen_body.find("client_secret"), std::string::npos);

        const std::string marker = "client_assertion=";
        auto value_begin = seen_body.find(marker);
        ASSERT_TRUE(value_begin != std::string::npos);
        value_begin += marker.size();
        auto value_end = seen_body.find('&', value_begin);
        std::string jwt = seen_body.substr(
            value_begin,
            value_end == std::string::npos ? std::string::npos
                                           : value_end - value_begin);

        auto dot1 = jwt.find('.');
        ASSERT_TRUE(dot1 != std::string::npos && dot1 > 0);
        auto dot2 = jwt.find('.', dot1 + 1);
        ASSERT_TRUE(dot2 != std::string::npos && dot2 > dot1 + 1);
        EXPECT_EQ(jwt.find('.', dot2 + 1), std::string::npos);

        auto header = mcp::detail::Base64UrlDecode(jwt.substr(0, dot1));
        auto claims_raw = mcp::detail::Base64UrlDecode(
            jwt.substr(dot1 + 1, dot2 - dot1 - 1));
        auto signature = mcp::detail::Base64UrlDecode(jwt.substr(dot2 + 1));
        ASSERT_TRUE(header.has_value() && claims_raw.has_value() &&
                    signature.has_value());
        EXPECT_EQ(signature->size(), 64U);

        auto header_json = JsonValue::Parse(*header);
        ASSERT_TRUE(header_json.IsObject());
        ASSERT_TRUE(header_json.Find("alg"));
        EXPECT_EQ(header_json.Find("alg")->GetString(), "ES256");
        ASSERT_TRUE(header_json.Find("typ"));
        EXPECT_EQ(header_json.Find("typ")->GetString(), "JWT");

        auto claims = JsonValue::Parse(*claims_raw);
        ASSERT_TRUE(claims.IsObject());
        ASSERT_TRUE(claims.Find("iss") && claims.Find("sub") &&
                    claims.Find("aud") && claims.Find("jti") &&
                    claims.Find("iat") && claims.Find("exp"));
        EXPECT_EQ(claims.Find("iss")->GetString(), "jwt-client");
        EXPECT_EQ(claims.Find("sub")->GetString(), "jwt-client");
        EXPECT_EQ(claims.Find("sub")->GetString(), claims.Find("iss")->GetString());
        EXPECT_EQ(claims.Find("aud")->GetString(), issuer);
        EXPECT_EQ(claims.Find("iat")->GetInt(), 1700000000);
        EXPECT_EQ(claims.Find("exp")->GetInt(), 1700000060);
        EXPECT_FALSE(claims.Find("jti")->GetString().empty());
    } catch (const McpError& e) {
        EXPECT_TRUE(e.Code() == McpErrorCode::InternalError);
    }
    server.Stop();
}

TEST(OAuthTest, PrivateKeyJwtUnadvertisedKeepsClientSecretPost) {
    auto port = PickFreePort(kTestBasePort + 1210);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::string seen_body;

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\","
                        "\"token_endpoint_auth_methods_supported\":"
                        "[\"client_secret_post\",\"none\"]}";
        });
    server.SetHandler("POST", "/token",
        [&seen_body, issuer](const mcp::HttpRequest& req, mcp::HttpResponse& resp) {
            seen_body = req.body;
            resp.body = "{\"access_token\":\"secret-token\",\"token_type\":\"Bearer\","
                        "\"expires_in\":3600,\"iss\":\"" + issuer + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "jwt-client";
    opts.client_secret = "test-secret";
    opts.private_key_pem = kTestEs256PrivateKeyPem;
    opts.allow_legacy_metadata_fallback = true;
    OAuthClientProvider provider(opts);

    ASSERT_TRUE(provider.AuthenticateClientCredentials());
    EXPECT_NE(seen_body.find("client_secret=test-secret"), std::string::npos);
    EXPECT_EQ(seen_body.find("client_assertion"), std::string::npos);
    EXPECT_EQ(provider.GetAccessToken(), "secret-token");
    server.Stop();
}

TEST(OAuthTest, ClientSecretSentOnCodeExchange) {
    auto port = PickFreePort(kTestBasePort + 1300);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::string seen_body;

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/token",
        [&seen_body, issuer](const mcp::HttpRequest& req, mcp::HttpResponse& resp) {
            seen_body = req.body;
            resp.body = "{\"access_token\":\"code-token\",\"token_type\":\"Bearer\","
                        "\"expires_in\":3600,\"iss\":\"" + issuer + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string seen_url;
    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.redirect_uri = "http://localhost/callback";
    opts.client_id = "confidential-client";
    opts.client_secret = "confidential-secret";
    opts.allow_legacy_metadata_fallback = true;
    opts.require_pkce_metadata = false;
    opts.authorization_redirect_handler =
        [&seen_url](std::string_view url) { seen_url = std::string(url); };
    opts.authorization_code_callback =
        [&seen_url]() -> std::optional<AuthorizationCodeResult> {
            AuthorizationCodeResult result;
            result.code = "auth-code";
            auto pos = seen_url.find("state=");
            if (pos != std::string::npos) {
                auto end = seen_url.find('&', pos);
                auto len = (end == std::string::npos)
                    ? std::string::npos : end - pos - 6;
                result.state = seen_url.substr(pos + 6, len);
            }
            return result;
        };
    OAuthClientProvider provider(opts);

    ASSERT_TRUE(provider.Authenticate());
    EXPECT_NE(seen_body.find("grant_type=authorization_code"), std::string::npos);
    EXPECT_NE(seen_body.find("client_id=confidential-client"), std::string::npos);
    EXPECT_NE(seen_body.find("client_secret=confidential-secret"), std::string::npos);
    EXPECT_EQ(provider.GetAccessToken(), "code-token");
    server.Stop();
}

TEST(OAuthTest, ClientSecretSentOnRefresh) {
    auto port = PickFreePort(kTestBasePort + 1310);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::string seen_body;

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/token",
        [&seen_body, issuer](const mcp::HttpRequest& req, mcp::HttpResponse& resp) {
            seen_body = req.body;
            resp.body = "{\"access_token\":\"refreshed-secret-token\","
                        "\"token_type\":\"Bearer\",\"expires_in\":3600,"
                        "\"iss\":\"" + issuer + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto cache = std::make_shared<InMemoryTokenCache>();
    TokenContainer stale;
    stale.access_token = "stale-token";
    stale.refresh_token = "refresh-secret";
    stale.expires_at = 0;
    cache->StoreTokens(stale);

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "confidential-client";
    opts.client_secret = "confidential-secret";
    opts.token_cache = cache;
    opts.allow_legacy_metadata_fallback = true;
    OAuthClientProvider provider(opts);

    ASSERT_TRUE(provider.RefreshTokens());
    EXPECT_NE(seen_body.find("grant_type=refresh_token"), std::string::npos);
    EXPECT_NE(seen_body.find("client_secret=confidential-secret"), std::string::npos);
    EXPECT_EQ(provider.GetAccessToken(), "refreshed-secret-token");
    server.Stop();
}

TEST(OAuthTest, ClientSecretBasicUsedWhenOnlyBasicAdvertised) {
    auto port = PickFreePort(kTestBasePort + 1340);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::string seen_body;
    std::string seen_authorization;

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\","
                        "\"token_endpoint_auth_methods_supported\":[\"client_secret_basic\"]}";
        });
    server.SetHandler("POST", "/token",
        [&seen_body, &seen_authorization, issuer](
            const mcp::HttpRequest& req, mcp::HttpResponse& resp) {
            seen_body = req.body;
            auto it = req.headers.find("authorization");
            if (it != req.headers.end()) seen_authorization = it->second;
            resp.body = "{\"access_token\":\"basic-token\",\"token_type\":\"Bearer\","
                        "\"expires_in\":3600,\"iss\":\"" + issuer + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "confidential-client";
    opts.client_secret = "confidential-secret";
    opts.allow_legacy_metadata_fallback = true;
    OAuthClientProvider provider(opts);

    ASSERT_TRUE(provider.AuthenticateClientCredentials());
    EXPECT_EQ(seen_body.find("client_secret"), std::string::npos);
    EXPECT_EQ(seen_authorization,
        "Basic Y29uZmlkZW50aWFsLWNsaWVudDpjb25maWRlbnRpYWwtc2VjcmV0");
    EXPECT_EQ(provider.GetAccessToken(), "basic-token");
    server.Stop();
}

TEST(OAuthTest, PrivateKeyJwtUnsupportedAlgorithmRejectsBeforeTokenRequest) {
    auto port = PickFreePort(kTestBasePort + 1220);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<int> token_hits{0};

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\","
                        "\"token_endpoint_auth_methods_supported\":"
                        "[\"private_key_jwt\"]}";
        });
    server.SetHandler("POST", "/token",
        [&token_hits](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            token_hits.fetch_add(1);
            resp.body = R"({"access_token":"should-not-issue"})";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "jwt-client";
    opts.private_key_pem = kTestEs256PrivateKeyPem;
    opts.signing_algorithm = "RS256";
    opts.allow_legacy_metadata_fallback = true;
    OAuthClientProvider provider(opts);

    bool threw = false;
    try {
        (void)provider.AuthenticateClientCredentials();
    } catch (const McpError& e) {
        threw = true;
        EXPECT_TRUE(e.Code() == McpErrorCode::InvalidParams);
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(token_hits.load(), 0);
    server.Stop();
}

TEST(OAuthTest, RegisterSendsApplicationType) {
    auto port = PickFreePort(kTestBasePort + 900);
    std::string seen_body;

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/register",
        [&seen_body](const mcp::HttpRequest& req, mcp::HttpResponse& resp) {
            seen_body = req.body;
            resp.body = R"({"client_id":"dyn-id","token_endpoint_auth_method":"none"})";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string endpoint = "http://127.0.0.1:" + std::to_string(port) + "/register";
    auto info = ClientRegistrationInfo::Register(
        endpoint, "http://localhost/callback", "test-client");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->client_id, "dyn-id");
    EXPECT_NE(seen_body.find("\"application_type\""), std::string::npos);
    EXPECT_NE(seen_body.find("\"native\""), std::string::npos);
    ASSERT_TRUE(info->token_endpoint_auth_method.has_value());
    EXPECT_EQ(*info->token_endpoint_auth_method, "none");

    auto web_info = ClientRegistrationInfo::Register(
        endpoint, "http://localhost/callback", "test-client", "web");
    ASSERT_TRUE(web_info.has_value());
    EXPECT_NE(seen_body.find("\"web\""), std::string::npos);
    server.Stop();
}

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

TEST(OAuthTest, DPoPDisabledByDefaultSkipsProof) {
    OAuthClientOptions opts;
    opts.server_url = "http://localhost:8080";
    opts.redirect_uri = "http://localhost:8080/callback";
    EXPECT_FALSE(opts.enable_dpop);

    auto cache = std::make_shared<InMemoryTokenCache>();
    opts.token_cache = cache;
    TokenContainer tokens;
    tokens.access_token = "plain-token";
    tokens.expires_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 3600000;
    cache->StoreTokens(tokens);

    OAuthClientProvider provider(opts);
    EXPECT_EQ(provider.GetAuthorizationHeader(), "Bearer plain-token");
    EXPECT_TRUE(provider.BuildDPoPProof("GET", "http://localhost:8080/mcp").empty());
}

TEST(OAuthTest, DPoPEnabledBindsSchemeAndProof) {
    auto cache = std::make_shared<InMemoryTokenCache>();
    TokenContainer tokens;
    tokens.access_token = "bound-token";
    tokens.expires_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 3600000;
    cache->StoreTokens(tokens);

    OAuthClientOptions opts;
    opts.server_url = "http://localhost:8080";
    opts.redirect_uri = "http://localhost:8080/callback";
    opts.token_cache = cache;
    opts.enable_dpop = true;

    OAuthClientProvider provider(opts);
    EXPECT_EQ(provider.GetAuthorizationHeader(), "DPoP bound-token");

    try {
        std::string proof = provider.BuildDPoPProof("POST", "https://resource.example/token");
        EXPECT_FALSE(proof.empty());
        auto first = proof.find('.');
        ASSERT_TRUE(first != std::string::npos);
        auto second = proof.find('.', first + 1);
        EXPECT_TRUE(second != std::string::npos);
        auto decoded = mcp::detail::Base64UrlDecode(proof.substr(0, first));
        ASSERT_TRUE(decoded.has_value());
        EXPECT_NE(decoded->find("dpop+jwt"), std::string::npos);
        EXPECT_NE(decoded->find("ES256"), std::string::npos);
    } catch (const McpError& e) {
        EXPECT_TRUE(e.Code() == McpErrorCode::InternalError);
    }

    std::string first_print = mcp::ext::dpop::ComputeThumbprint("x-value", "y-value");
    EXPECT_FALSE(first_print.empty());
    EXPECT_EQ(first_print.find('='), std::string::npos);
    EXPECT_EQ(mcp::ext::dpop::ComputeThumbprint("x-value", "y-value"), first_print);
}

TEST(OAuthTest, DPoPProofHeaderAdapterSkipsWhenDisabled) {
    OAuthClientOptions opts;
    opts.server_url = "http://localhost:8080";
    opts.redirect_uri = "http://localhost:8080/callback";

    OAuthClientProvider provider(opts);
    std::map<std::string, std::string> headers;
    provider.ApplyDPoPProofHeader(
        HttpRequestHeaderContext{"POST", "https://resource.example/mcp?x=1",
                                 HttpRequestPurpose::MessagePost,
                                 RequestId{int64_t(7)}, 0},
        headers);
    EXPECT_TRUE(headers.find("DPoP") == headers.end());

    provider.ObserveDPoPNonce("ignored-nonce");
    EXPECT_TRUE(provider.BuildDPoPProof("POST", "https://resource.example/mcp").empty());
}

TEST(OAuthTest, DPoPProofHeaderAdapterStripsQueryAndVariesPerCall) {
    auto cache = std::make_shared<InMemoryTokenCache>();
    TokenContainer tokens;
    tokens.access_token = "adapter-token";
    tokens.expires_at = 9999999999999LL;
    cache->StoreTokens(tokens);

    OAuthClientOptions opts;
    opts.server_url = "http://localhost:8080";
    opts.redirect_uri = "http://localhost:8080/callback";
    opts.token_cache = cache;
    opts.enable_dpop = true;

    OAuthClientProvider provider(opts);
    HttpRequestHeaderContext context{"POST", "https://resource.example/mcp?code=secret",
                                     HttpRequestPurpose::MessagePost,
                                     RequestId{int64_t(9)}, 0};

    std::map<std::string, std::string> first;
    std::map<std::string, std::string> second;
    try {
        provider.ApplyDPoPProofHeader(context, first);
        provider.ApplyDPoPProofHeader(context, second);
    } catch (const McpError& e) {
        EXPECT_TRUE(e.Code() == McpErrorCode::InternalError);
        return;
    }

    auto first_proof = first.find("DPoP");
    auto second_proof = second.find("DPoP");
    ASSERT_TRUE(first_proof != first.end());
    ASSERT_TRUE(second_proof != second.end());
    EXPECT_FALSE(first_proof->second.empty());
    EXPECT_NE(first_proof->second, second_proof->second);

    auto first_dot = first_proof->second.find('.');
    auto second_dot = first_proof->second.find('.', first_dot + 1);
    ASSERT_TRUE(first_dot != std::string::npos);
    ASSERT_TRUE(second_dot != std::string::npos);
    auto payload = mcp::detail::Base64UrlDecode(
        first_proof->second.substr(first_dot + 1, second_dot - first_dot - 1));
    ASSERT_TRUE(payload.has_value());
    EXPECT_NE(payload->find("\"htu\":\"https://resource.example/mcp\""),
              std::string::npos);
    EXPECT_EQ(payload->find("code=secret"), std::string::npos);
    EXPECT_NE(payload->find("\"ath\":"), std::string::npos);
    EXPECT_NE(payload->find("\"jti\":\""), std::string::npos);
}

TEST(OAuthTest, DPoPNonceObservationFeedsNextProof) {
    auto cache = std::make_shared<InMemoryTokenCache>();
    TokenContainer tokens;
    tokens.access_token = "nonce-token";
    tokens.expires_at = 9999999999999LL;
    cache->StoreTokens(tokens);

    OAuthClientOptions opts;
    opts.server_url = "http://localhost:8080";
    opts.redirect_uri = "http://localhost:8080/callback";
    opts.token_cache = cache;
    opts.enable_dpop = true;

    OAuthClientProvider provider(opts);
    provider.ObserveDPoPNonce("https://resource.example/mcp", "Bearer", "");
    provider.ObserveDPoPNonce("https://resource.example/mcp", "Bearer",
        std::string(2048, 'n'));
    provider.ObserveDPoPNonce("https://resource.example/mcp", "Bearer", "nonce-abc");

    std::map<std::string, std::string> headers;
    try {
        provider.ApplyDPoPProofHeader(
            HttpRequestHeaderContext{"GET", "https://resource.example/mcp",
                                     HttpRequestPurpose::ListenStream,
                                     std::nullopt, 0},
            headers);
    } catch (const McpError& e) {
        EXPECT_TRUE(e.Code() == McpErrorCode::InternalError);
        return;
    }

    auto proof = headers.find("DPoP");
    ASSERT_TRUE(proof != headers.end());
    auto first_dot = proof->second.find('.');
    auto second_dot = proof->second.find('.', first_dot + 1);
    ASSERT_TRUE(first_dot != std::string::npos);
    ASSERT_TRUE(second_dot != std::string::npos);
    auto payload = mcp::detail::Base64UrlDecode(
        proof->second.substr(first_dot + 1, second_dot - first_dot - 1));
    ASSERT_TRUE(payload.has_value());
    EXPECT_NE(payload->find("\"nonce\":\"nonce-abc\""), std::string::npos);
    EXPECT_NE(payload->find("\"htm\":\"GET\""), std::string::npos);
}

TEST(OAuthTest, DpopNonceBucketsIsolatedByOriginAndTokenType) {
    OAuthClientOptions opts;
    opts.server_url = "http://localhost:8080";
    opts.redirect_uri = "http://localhost:8080/callback";
    opts.enable_dpop = true;
    OAuthClientProvider provider(opts);

    provider.ObserveDPoPNonce("https://a.example/mcp", "Bearer", "nonce-a-bearer");
    provider.ObserveDPoPNonce("https://a.example/mcp", "DPoP", "nonce-a-dpop");
    provider.ObserveDPoPNonce("https://b.example/mcp", "Bearer", "nonce-b-bearer");

    ASSERT_TRUE(provider.GetDPoPNonce("https://a.example/mcp", "Bearer").has_value());
    EXPECT_EQ(*provider.GetDPoPNonce("https://a.example/mcp", "Bearer"), "nonce-a-bearer");
    ASSERT_TRUE(provider.GetDPoPNonce("https://a.example/mcp", "DPoP").has_value());
    EXPECT_EQ(*provider.GetDPoPNonce("https://a.example/mcp", "DPoP"), "nonce-a-dpop");
    ASSERT_TRUE(provider.GetDPoPNonce("https://b.example/mcp", "Bearer").has_value());
    EXPECT_EQ(*provider.GetDPoPNonce("https://b.example/mcp", "Bearer"), "nonce-b-bearer");
    EXPECT_FALSE(provider.GetDPoPNonce("https://a.example/mcp", "other").has_value());
    EXPECT_FALSE(provider.GetDPoPNonce("https://c.example/mcp", "Bearer").has_value());
    ASSERT_TRUE(provider.GetDPoPNonce("https://A.EXAMPLE:443/mcp", "bearer").has_value());
    EXPECT_EQ(*provider.GetDPoPNonce("https://A.EXAMPLE:443/mcp", "bearer"),
        "nonce-a-bearer");

    provider.SetDPoPNonceContext("https://c.example/mcp", "Bearer");
    provider.ObserveDPoPNonce("legacy-nonce");
    ASSERT_TRUE(provider.GetDPoPNonce("https://c.example/mcp", "Bearer").has_value());
    EXPECT_EQ(*provider.GetDPoPNonce("https://c.example/mcp", "Bearer"), "legacy-nonce");
    EXPECT_EQ(*provider.GetDPoPNonce("https://a.example/mcp", "Bearer"), "nonce-a-bearer");
}

TEST(OAuthTest, DpopKeyRestoredFromCacheAcrossProviderRestart) {
    auto cache = std::make_shared<InMemoryTokenCache>();
    ext::dpop::EcP256Jwk key{
        "axfR8uEsQkf4vOblY6RA8ncDfYEt6zOg9KE5RdiYwpY",
        "T-NC4v4af5uO5-tKfA-eFivOM1drMV7Oy7ZAaDe_UfU",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAE"};
    ASSERT_TRUE(ext::dpop::ValidateKey(key));
    cache->StoreDpopKey(key);

    OAuthClientOptions opts;
    opts.server_url = "http://localhost:8080";
    opts.redirect_uri = "http://localhost:8080/callback";
    opts.token_cache = cache;
    opts.enable_dpop = true;
    OAuthClientProvider provider(opts);

    try {
        auto proof = provider.BuildDPoPProof("GET", "https://resource.example/mcp");
        ASSERT_FALSE(proof.empty());
        auto dot = proof.find('.');
        ASSERT_NE(dot, std::string::npos);
        auto header = mcp::detail::Base64UrlDecode(proof.substr(0, dot));
        ASSERT_TRUE(header.has_value());
        EXPECT_NE(header->find(key.x), std::string::npos);
        EXPECT_NE(header->find(key.y), std::string::npos);
    } catch (const McpError&) {
    }
}

TEST(OAuthTest, DpopLegacyNonceSetterDelegatesToCurrentBucket) {
    OAuthClientOptions opts;
    opts.server_url = "http://localhost:8080";
    opts.redirect_uri = "http://localhost:8080/callback";
    opts.enable_dpop = true;
    OAuthClientProvider provider(opts);

    provider.SetDPoPNonceContext("https://legacy.example/mcp", "DPoP");
    provider.SetDPoPNonce("legacy-set-nonce");
    ASSERT_TRUE(provider.GetDPoPNonce("https://legacy.example/mcp", "DPoP").has_value());
    EXPECT_EQ(*provider.GetDPoPNonce("https://legacy.example/mcp", "DPoP"),
        "legacy-set-nonce");
    EXPECT_FALSE(provider.GetDPoPNonce("https://legacy.example/mcp", "Bearer").has_value());
}

TEST(OAuthTest, DPoPStaleProofRejectedByWindow) {
    int64_t now = mcp::ext::dpop::NowSeconds();
    EXPECT_TRUE(mcp::ext::dpop::IsNonceFresh(now, now));
    EXPECT_TRUE(mcp::ext::dpop::IsNonceFresh(now - 299, now));
    EXPECT_FALSE(mcp::ext::dpop::IsNonceFresh(now - 301, now));
    EXPECT_FALSE(mcp::ext::dpop::IsNonceFresh(now - 3600, now));    EXPECT_FALSE(mcp::ext::dpop::IsNonceFresh(now + 60, now));
    EXPECT_FALSE(mcp::ext::dpop::IsNonceFresh(now, now, -1));
}

TEST(OAuthTest, PkceS256UnsupportedAdvertisedRefusesFlow) {
    auto port = PickFreePort(kTestBasePort + 1100);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\","
                        "\"code_challenge_methods_supported\":[\"plain\"]}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    bool callback_called = false;
    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.redirect_uri = "http://localhost/callback";
    opts.authorization_code_callback =
        [&callback_called]() -> std::optional<AuthorizationCodeResult> {
            callback_called = true;
            return std::nullopt;
        };
    OAuthClientProvider provider(opts);
    EXPECT_FALSE(provider.Authenticate());
    EXPECT_FALSE(callback_called);
    server.Stop();
}

TEST(OAuthTest, EmptyAuthCodeRejectedWithoutTokenRequest) {
    auto port = PickFreePort(kTestBasePort + 1110);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<int> token_hits{0};

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\"}";
        });
    server.SetHandler("POST", "/token",
        [&token_hits](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            token_hits.fetch_add(1);
            resp.body = "{\"access_token\":\"should-not-issue\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string seen_url;
    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.redirect_uri = "http://localhost/callback";
    opts.allow_legacy_metadata_fallback = true;
    opts.require_pkce_metadata = false;
    opts.authorization_redirect_handler =
        [&seen_url](std::string_view url) { seen_url = std::string(url); };
    opts.authorization_code_callback =
        [&seen_url]() -> std::optional<AuthorizationCodeResult> {
            AuthorizationCodeResult result;
            result.code = "";
            auto pos = seen_url.find("state=");
            if (pos != std::string::npos) {
                auto end = seen_url.find('&', pos);
                auto len = (end == std::string::npos)
                    ? std::string::npos : end - pos - 6;
                result.state = seen_url.substr(pos + 6, len);
            }
            return result;
        };
    OAuthClientProvider provider(opts);
    EXPECT_FALSE(provider.Authenticate());
    EXPECT_NE(seen_url.find("code_challenge_method=S256"), std::string::npos);
    EXPECT_EQ(token_hits.load(), 0);
    server.Stop();
}

TEST(OAuthTest, TokenResponseMissingIssAccepted) {
    auto port = PickFreePort(kTestBasePort + 1120);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/token",
        [](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"access_token\":\"no-iss-token\",\"token_type\":\"Bearer\","
                        "\"expires_in\":3600}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "test-client";
    opts.client_secret = "test-secret";
    opts.allow_legacy_metadata_fallback = true;
    OAuthClientProvider provider(opts);
    EXPECT_TRUE(provider.AuthenticateClientCredentials());
    EXPECT_TRUE(provider.HasToken());
    EXPECT_EQ(provider.GetAccessToken(), "no-iss-token");
    server.Stop();
}

TEST(OAuthTest, MissingIssWarnsWhenNotRequired) {
    auto port = PickFreePort(kTestBasePort + 1320);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\","
                        "\"authorization_response_iss_parameter_supported\":true}";
        });
    server.SetHandler("POST", "/token",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"access_token\":\"flow-token\",\"token_type\":\"Bearer\","
                        "\"expires_in\":3600,\"iss\":\"" + issuer + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string seen_url;
    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.redirect_uri = "http://localhost/callback";
    opts.client_id = "test-client";
    opts.require_iss_when_advertised = false;
    opts.allow_legacy_metadata_fallback = true;
    opts.require_pkce_metadata = false;
    opts.authorization_redirect_handler =
        [&seen_url](std::string_view url) { seen_url = std::string(url); };
    opts.authorization_code_callback =
        [&seen_url]() -> std::optional<AuthorizationCodeResult> {
            AuthorizationCodeResult result;
            result.code = "auth-code";
            auto pos = seen_url.find("state=");
            if (pos != std::string::npos) {
                auto end = seen_url.find('&', pos);
                auto len = (end == std::string::npos)
                    ? std::string::npos : end - pos - 6;
                result.state = seen_url.substr(pos + 6, len);
            }
            return result;
        };
    OAuthClientProvider provider(opts);

    EXPECT_TRUE(provider.Authenticate());
    EXPECT_EQ(provider.GetAccessToken(), "flow-token");
    server.Stop();
}

TEST(OAuthTest, MissingIssRejectedWhenRequired) {
    auto port = PickFreePort(kTestBasePort + 1330);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<int> token_hits{0};

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\","
                        "\"authorization_response_iss_parameter_supported\":true}";
        });
    server.SetHandler("POST", "/token",
        [&token_hits, issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            token_hits.fetch_add(1);
            resp.body = "{\"access_token\":\"should-not-issue\",\"token_type\":\"Bearer\","
                        "\"expires_in\":3600,\"iss\":\"" + issuer + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string seen_url;
    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.redirect_uri = "http://localhost/callback";
    opts.client_id = "test-client";
    opts.require_iss_when_advertised = true;
    opts.allow_legacy_metadata_fallback = true;
    opts.require_pkce_metadata = false;
    opts.authorization_redirect_handler =
        [&seen_url](std::string_view url) { seen_url = std::string(url); };
    opts.authorization_code_callback =
        [&seen_url]() -> std::optional<AuthorizationCodeResult> {
            AuthorizationCodeResult result;
            result.code = "auth-code";
            auto pos = seen_url.find("state=");
            if (pos != std::string::npos) {
                auto end = seen_url.find('&', pos);
                auto len = (end == std::string::npos)
                    ? std::string::npos : end - pos - 6;
                result.state = seen_url.substr(pos + 6, len);
            }
            return result;
        };
    OAuthClientProvider provider(opts);

    EXPECT_FALSE(provider.Authenticate());
    EXPECT_EQ(token_hits.load(), 0);
    EXPECT_FALSE(provider.HasToken());
    server.Stop();
}

TEST(OAuthTest, TokenResponseMatchingIssAccepted) {
    auto port = PickFreePort(kTestBasePort + 1122);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/token",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"access_token\":\"matching-iss-token\","
                        "\"token_type\":\"Bearer\",\"expires_in\":3600,"
                        "\"iss\":\"" + issuer + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "test-client";
    opts.client_secret = "test-secret";
    opts.allow_legacy_metadata_fallback = true;
    OAuthClientProvider provider(opts);
    EXPECT_TRUE(provider.AuthenticateClientCredentials());
    EXPECT_TRUE(provider.HasToken());
    EXPECT_EQ(provider.GetAccessToken(), "matching-iss-token");
    server.Stop();
}

TEST(OAuthTest, TokenResponseIssuerMismatchRejected) {
    auto port = PickFreePort(kTestBasePort + 1130);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/token",
        [](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"access_token\":\"forged-token\",\"token_type\":\"Bearer\","
                        "\"expires_in\":3600,\"iss\":\"http://127.0.0.1:9/evil\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "test-client";
    opts.client_secret = "test-secret";
    OAuthClientProvider provider(opts);
    EXPECT_FALSE(provider.AuthenticateClientCredentials());
    EXPECT_FALSE(provider.HasToken());
    server.Stop();
}

TEST(OAuthTest, TokenResponseNonStringIssRejected) {
    auto port = PickFreePort(kTestBasePort + 1135);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/token",
        [](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"access_token\":\"typed-iss-token\","
                        "\"token_type\":\"Bearer\",\"expires_in\":3600,"
                        "\"iss\":{\"value\":\"forged\"}}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "test-client";
    opts.client_secret = "test-secret";
    OAuthClientProvider provider(opts);
    EXPECT_FALSE(provider.AuthenticateClientCredentials());
    EXPECT_FALSE(provider.HasToken());
    server.Stop();
}

TEST(OAuthTest, TokenRefreshWithoutIssAccepted) {
    auto port = PickFreePort(kTestBasePort + 1138);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/token",
        [](const mcp::HttpRequest& req, mcp::HttpResponse& resp) {
            if (req.body.find("grant_type=refresh_token") != std::string::npos) {
                resp.body = "{\"access_token\":\"refreshed-no-iss\","
                            "\"token_type\":\"Bearer\",\"expires_in\":3600}";
            } else {
                resp.body = "{\"access_token\":\"issued-no-iss\","
                            "\"token_type\":\"Bearer\",\"expires_in\":0,"
                            "\"refresh_token\":\"rt-one\"}";
            }
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "test-client";
    opts.client_secret = "test-secret";
    opts.allow_legacy_metadata_fallback = true;
    OAuthClientProvider provider(opts);
    ASSERT_TRUE(provider.AuthenticateClientCredentials());
    EXPECT_EQ(provider.GetAccessToken(), "refreshed-no-iss");
    server.Stop();
}

TEST(OAuthTest, ApplicationTypeEmptyStringDefaultsNative) {
    auto port = PickFreePort(kTestBasePort + 1140);
    std::string seen_body;

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/register",
        [&seen_body](const mcp::HttpRequest& req, mcp::HttpResponse& resp) {
            seen_body = req.body;
            resp.body = R"({"client_id":"dyn-id"})";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string endpoint = "http://127.0.0.1:" + std::to_string(port) + "/register";
    auto info = ClientRegistrationInfo::Register(
        endpoint, "http://localhost/callback", "test-client", "");
    ASSERT_TRUE(info.has_value());
    EXPECT_NE(seen_body.find("\"native\""), std::string::npos);
    server.Stop();
}

TEST(OAuthTest, TokenResourcePersistedAndMismatchGate) {
    auto port = PickFreePort(kTestBasePort + 1150);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/token",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"access_token\":\"res-token\",\"token_type\":\"Bearer\","
                        "\"expires_in\":3600,\"iss\":\"" + issuer + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto cache = std::make_shared<InMemoryTokenCache>();
    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "test-client";
    opts.client_secret = "test-secret";
    opts.token_cache = cache;
    opts.allow_legacy_metadata_fallback = true;
    OAuthClientProvider provider(opts);
    ASSERT_TRUE(provider.AuthenticateClientCredentials());

    auto cached = cache->GetTokens();
    ASSERT_TRUE(cached.has_value());
    ASSERT_TRUE(cached->resource.has_value());
    EXPECT_EQ(*cached->resource, issuer);

    OAuthClientOptions foreign_opts;
    foreign_opts.server_url = "http://127.0.0.1:9/other";
    foreign_opts.token_cache = cache;
    OAuthClientProvider foreign(foreign_opts);
    EXPECT_FALSE(foreign.HasToken());
    EXPECT_TRUE(foreign.GetAuthorizationHeader().empty());
    EXPECT_TRUE(foreign.GetAccessToken().empty());
    server.Stop();
}

TEST(OAuthTest, IssuerChangeRejectedAndTokensRetained) {
    auto port = PickFreePort(kTestBasePort + 1160);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/meta1",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\"}";
        });
    server.SetHandler("GET", "/meta2",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "/tenant-b\","
                        "\"authorization_endpoint\":"
                        "\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\"}";
        });
    server.SetHandler("GET", "/meta3",
        [](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"http://127.0.0.1:9/tenant-b\","
                        "\"authorization_endpoint\":"
                        "\"http://127.0.0.1:9/tenant-b/authorize\","
                        "\"token_endpoint\":\"http://127.0.0.1:9/tenant-b/token\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto cache = std::make_shared<InMemoryTokenCache>();
    TokenContainer tokens;
    tokens.access_token = "sealed-token";
    tokens.expires_at = 9999999999999LL;
    cache->StoreTokens(tokens);

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.redirect_uri = "http://localhost/callback";
    opts.token_cache = cache;
    OAuthClientProvider provider(opts);

    EXPECT_TRUE(provider.HandleAuthChallenge(
        "Bearer resource_metadata=\"" + issuer + "/meta1\""));
    EXPECT_TRUE(provider.HasToken());
    EXPECT_FALSE(provider.HandleAuthChallenge(
        "Bearer resource_metadata=\"" + issuer + "/meta2\""));
    EXPECT_TRUE(provider.HasToken());
    EXPECT_FALSE(provider.HandleAuthChallenge(
        "Bearer resource_metadata=\"" + issuer + "/meta3\""));
    EXPECT_TRUE(provider.HasToken());
    auto retained = cache->GetTokens();
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained->access_token, "sealed-token");
    server.Stop();
}

TEST(OAuthTest, ChallengeOversizeRejectedWithoutThrow) {
    OAuthClientOptions opts;
    opts.server_url = "http://localhost:8080";
    opts.redirect_uri = "http://localhost:8080/callback";
    OAuthClientProvider provider(opts);

    std::string huge(5000, 'x');
    EXPECT_NO_THROW((void)provider.HandleAuthChallenge(huge));
    EXPECT_FALSE(provider.HandleAuthChallenge(huge));
}

TEST(OAuthTest, ChallengeIllegalUrlRejectedWithoutThrow) {
    OAuthClientOptions opts;
    opts.server_url = "http://localhost:8080";
    opts.redirect_uri = "http://localhost:8080/callback";
    OAuthClientProvider provider(opts);

    EXPECT_FALSE(provider.HandleAuthChallenge(
        "Bearer resource_metadata=\"ftp://evil.example/meta\""));
    EXPECT_FALSE(provider.HandleAuthChallenge(
        "Bearer resource_metadata=\"http://exa mple.com/x\""));
    EXPECT_FALSE(provider.HandleAuthChallenge("Bearer error=\"invalid_token\""));
    EXPECT_TRUE(provider.GetAuthorizationHeader().empty());
}

TEST(OAuthTest, TokenCacheConcurrentStoreGet) {
    InMemoryTokenCache cache;
    std::atomic<int> reads{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back([&cache, &reads, i]() {
            for (int n = 0; n < 200; ++n) {
                TokenContainer item;
                item.access_token = "token-" + std::to_string(i) + "-"
                    + std::to_string(n);
                item.expires_at = 9999999999999LL;
                cache.StoreTokens(item);
                auto got = cache.GetTokens();
                if (got && !got->access_token.empty()) reads.fetch_add(1);
                if (n % 50 == 0) cache.ClearTokens();
            }
        });
    }
    for (auto& worker : workers) worker.join();

    TokenContainer fin;
    fin.access_token = "final";
    fin.expires_at = 9999999999999LL;
    cache.StoreTokens(fin);
    auto got = cache.GetTokens();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->access_token, "final");
    EXPECT_TRUE(reads.load() > 0);
}

TEST(OAuthTest, TokenRefreshNotifiesHandler) {
    auto port = PickFreePort(kTestBasePort + 1170);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/token",
        [issuer](const mcp::HttpRequest& req, mcp::HttpResponse& resp) {
            if (req.body.find("grant_type=refresh_token") != std::string::npos) {
                resp.body = "{\"access_token\":\"rotated-token\","
                            "\"token_type\":\"Bearer\",\"expires_in\":3600,"
                            "\"refresh_token\":\"rt-two\",\"iss\":\"" + issuer + "\"}";
            } else {
                resp.body = "{\"access_token\":\"short-token\","
                            "\"token_type\":\"Bearer\",\"expires_in\":0,"
                            "\"refresh_token\":\"rt-one\",\"iss\":\"" + issuer + "\"}";
            }
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::mutex seen_mutex;
    std::vector<std::string> seen;
    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "test-client";
    opts.client_secret = "test-secret";
    opts.allow_legacy_metadata_fallback = true;
    opts.token_update_handler = [&seen_mutex, &seen](const TokenContainer& t) {
        std::lock_guard<std::mutex> lock(seen_mutex);
        seen.push_back(t.access_token);
    };
    OAuthClientProvider provider(opts);
    ASSERT_TRUE(provider.AuthenticateClientCredentials());
    EXPECT_EQ(provider.GetAccessToken(), "rotated-token");

    std::lock_guard<std::mutex> lock(seen_mutex);
    ASSERT_TRUE(seen.size() == 2);
    EXPECT_EQ(seen[0], "short-token");
    EXPECT_EQ(seen[1], "rotated-token");
    server.Stop();
}

TEST(OAuthTest, ConcurrentGetAccessTokenRefreshesOnce) {
    auto port = PickFreePort(kTestBasePort + 1172);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<int> refresh_calls{0};

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/token",
        [&](const mcp::HttpRequest& req, mcp::HttpResponse& resp) {
            if (req.body.find("grant_type=refresh_token") != std::string::npos) {
                refresh_calls.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                resp.body = "{\"access_token\":\"refreshed-token\","
                            "\"token_type\":\"Bearer\",\"expires_in\":3600,"
                            "\"refresh_token\":\"rt-two\",\"iss\":\"" + issuer + "\"}";
            } else {
                resp.body = "{\"access_token\":\"short-token\","
                            "\"token_type\":\"Bearer\",\"expires_in\":0,"
                            "\"refresh_token\":\"rt-one\",\"iss\":\"" + issuer + "\"}";
            }
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "test-client";
    opts.client_secret = "test-secret";
    opts.allow_legacy_metadata_fallback = true;
    OAuthClientProvider provider(opts);
    ASSERT_TRUE(provider.AuthenticateClientCredentials());

    std::vector<std::thread> callers;
    std::vector<std::string> results(4);
    for (int i = 0; i < 4; ++i) {
        callers.emplace_back([&provider, &results, i] {
            results[static_cast<size_t>(i)] = provider.GetAccessToken();
        });
    }
    for (auto& caller : callers) caller.join();

    EXPECT_EQ(refresh_calls.load(), 1);
    for (const auto& value : results) {
        EXPECT_EQ(value, "refreshed-token");
    }

    server.Stop();
}

TEST(OAuthTest, TokenUpdateHandlerReentrantGetAccessTokenDoesNotDeadlock) {
    auto port = PickFreePort(kTestBasePort + 1174);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/token",
        [issuer](const mcp::HttpRequest& req, mcp::HttpResponse& resp) {
            if (req.body.find("grant_type=refresh_token") != std::string::npos) {
                resp.body = "{\"access_token\":\"rotated-token\","
                            "\"token_type\":\"Bearer\",\"expires_in\":0,"
                            "\"refresh_token\":\"rt-two\",\"iss\":\"" + issuer + "\"}";
            } else {
                resp.body = "{\"access_token\":\"short-token\","
                            "\"token_type\":\"Bearer\",\"expires_in\":0,"
                            "\"refresh_token\":\"rt-one\",\"iss\":\"" + issuer + "\"}";
            }
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::mutex seen_mutex;
    std::vector<std::string> seen;
    OAuthClientProvider* provider_ptr = nullptr;
    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "test-client";
    opts.client_secret = "test-secret";
    opts.allow_legacy_metadata_fallback = true;
    opts.token_update_handler =
        [&provider_ptr, &seen_mutex, &seen](const TokenContainer& t) {
            std::lock_guard<std::mutex> lock(seen_mutex);
            seen.push_back(t.access_token);
            if (seen.size() < 2 || provider_ptr == nullptr) return;
            seen.push_back(provider_ptr->GetAccessToken());
        };
    OAuthClientProvider provider(opts);
    provider_ptr = &provider;

    MCP_RUN_WITH_TIMEOUT(std::chrono::seconds(20), [&] {
        EXPECT_TRUE(provider.AuthenticateClientCredentials());
        EXPECT_EQ(provider.GetAccessToken(), "rotated-token");
    });

    std::lock_guard<std::mutex> lock(seen_mutex);
    ASSERT_TRUE(seen.size() == 3);
    EXPECT_EQ(seen[0], "short-token");
    EXPECT_EQ(seen[1], "rotated-token");
    EXPECT_EQ(seen[2], "rotated-token");
    server.Stop();
}

TEST(OAuthTest, ConcurrentWaiterSurvivesRefreshOwnerReentry) {
    auto port = PickFreePort(kTestBasePort + 1176);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<int> refresh_calls{0};
    std::atomic<bool> refresh_started{false};

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/token",
        [&](const mcp::HttpRequest& req, mcp::HttpResponse& resp) {
            if (req.body.find("grant_type=refresh_token") != std::string::npos) {
                refresh_calls.fetch_add(1);
                refresh_started.store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                resp.body = "{\"access_token\":\"rotated-token\","
                            "\"token_type\":\"Bearer\",\"expires_in\":3600,"
                            "\"refresh_token\":\"rt-two\",\"iss\":\"" + issuer + "\"}";
            } else {
                resp.body = "{\"access_token\":\"short-token\","
                            "\"token_type\":\"Bearer\",\"expires_in\":0,"
                            "\"refresh_token\":\"rt-one\",\"iss\":\"" + issuer + "\"}";
            }
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::mutex seen_mutex;
    std::vector<std::string> seen;
    OAuthClientProvider* provider_ptr = nullptr;
    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "test-client";
    opts.client_secret = "test-secret";
    opts.allow_legacy_metadata_fallback = true;
    opts.token_update_handler =
        [&provider_ptr, &seen_mutex, &seen](const TokenContainer& t) {
            std::lock_guard<std::mutex> lock(seen_mutex);
            seen.push_back(t.access_token);
            if (seen.size() < 2 || provider_ptr == nullptr) return;
            seen.push_back(provider_ptr->GetAccessToken());
        };
    OAuthClientProvider provider(opts);
    provider_ptr = &provider;

    std::string waiter_token;
    std::string owner_token;
    std::atomic<bool> refresh_failed{false};
    MCP_RUN_WITH_TIMEOUT(std::chrono::seconds(20), [&] {
        EXPECT_TRUE(provider.AuthenticateClientCredentials());
        std::thread waiter([&] {
            try {
                while (!refresh_started.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                waiter_token = provider.GetAccessToken();
            } catch (...) {
                refresh_failed.store(true);
            }
        });
        try {
            owner_token = provider.GetAccessToken();
        } catch (...) {
            refresh_failed.store(true);
        }
        waiter.join();
    });

    EXPECT_FALSE(refresh_failed.load());
    EXPECT_EQ(refresh_calls.load(), 1);
    EXPECT_EQ(owner_token, "rotated-token");
    EXPECT_EQ(waiter_token, "rotated-token");
    std::lock_guard<std::mutex> lock(seen_mutex);
    ASSERT_TRUE(seen.size() == 3);
    EXPECT_EQ(seen[0], "short-token");
    EXPECT_EQ(seen[1], "rotated-token");
    EXPECT_EQ(seen[2], "rotated-token");
    server.Stop();
}

TEST(OAuthTest, AuthClosedLoopBearerChallengeSuccess) {
    auto port = PickFreePort(kTestBasePort + 1180);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<int> calls{0};
    std::string second_auth;

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/meta",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\"}";
        });
    server.SetHandler("POST", "/mcp",
        [&calls, &second_auth, issuer](const mcp::HttpRequest& req,
                                       mcp::HttpResponse& resp) {
            if (calls.fetch_add(1) == 0) {
                resp.status_code = 401;
                resp.status_text = "Unauthorized";
                resp.headers["WWW-Authenticate"] =
                    "Bearer resource_metadata=\"" + issuer + "/meta\","
                    " error=\"invalid_token\"";
                return;
            }
            auto it = req.headers.find("authorization");
            if (it != req.headers.end()) second_auth = it->second;
            resp.body = R"({"jsonrpc":"2.0","id":1,"result":{"resultType":"complete"}})";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto cache = std::make_shared<InMemoryTokenCache>();
    TokenContainer tokens;
    tokens.access_token = "loop-token";
    tokens.expires_at = 9999999999999LL;
    cache->StoreTokens(tokens);

    OAuthClientOptions oauth_opts;
    oauth_opts.server_url = issuer;
    oauth_opts.redirect_uri = "http://localhost/callback";
    oauth_opts.token_cache = cache;
    OAuthClientProvider provider(oauth_opts);

    mcp::HttpClientTransportOptions opts;
    opts.endpoint = issuer + "/mcp";
    opts.auth_challenge_handler =
        [&provider](std::string_view www_auth) -> std::string {
            if (!provider.HandleAuthChallenge(www_auth)) return {};
            return provider.GetAuthorizationHeader();
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
    EXPECT_EQ(second_auth, "Bearer loop-token");

    session->Close();
    server.Stop();
}

TEST(OAuthTest, AuthClosedLoopScopeInsufficientRetry) {
    auto port = PickFreePort(kTestBasePort + 1190);
    std::atomic<int> calls{0};
    std::atomic<bool> handler_called{false};
    std::string second_auth;

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/mcp",
        [&calls, &second_auth](const mcp::HttpRequest& req,
                               mcp::HttpResponse& resp) {
            if (calls.fetch_add(1) == 0) {
                resp.status_code = 403;
                resp.status_text = "Forbidden";
                resp.headers["WWW-Authenticate"] =
                    "Bearer error=\"insufficient_scope\", scope=\"extra-scope\"";
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
    opts.auth_challenge_handler =
        [&handler_called](std::string_view www_auth) -> std::string {
            handler_called.store(
                www_auth.find("insufficient_scope") != std::string::npos);
            return "Bearer upgraded-token";
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
    EXPECT_TRUE(handler_called.load());
    EXPECT_EQ(second_auth, "Bearer upgraded-token");

    session->Close();
    server.Stop();
}

TEST(OAuthTest, CimdUnreachableDocumentFallsBackToAuthorizationFlow) {
    auto port = PickFreePort(kTestBasePort + 1210);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\","
                        "\"code_challenge_methods_supported\":[\"S256\"]}";
        });
    server.SetHandler("POST", "/token",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"access_token\":\"cimd-fallback-token\","
                        "\"token_type\":\"Bearer\",\"expires_in\":3600,"
                        "\"iss\":\"" + issuer + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string seen_url;
    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.redirect_uri = "http://localhost/callback";
    opts.client_id = "https://127.0.0.1:9/unreachable-client";
    opts.allow_legacy_metadata_fallback = true;
    opts.authorization_redirect_handler =
        [&seen_url](std::string_view url) { seen_url = std::string(url); };
    opts.authorization_code_callback =
        [&seen_url]() -> std::optional<AuthorizationCodeResult> {
            AuthorizationCodeResult result;
            result.code = "cimd-code";
            auto pos = seen_url.find("state=");
            if (pos != std::string::npos) {
                auto end = seen_url.find('&', pos);
                auto len = (end == std::string::npos)
                    ? std::string::npos : end - pos - 6;
                result.state = seen_url.substr(pos + 6, len);
            }
            return result;
        };
    OAuthClientProvider provider(opts);
    EXPECT_TRUE(provider.Authenticate());
    EXPECT_EQ(provider.GetAccessToken(), "cimd-fallback-token");
    server.Stop();
}

TEST(OAuthTest, CimdHttpDocumentUrlRejectedBeforeDiscovery) {
    OAuthClientOptions opts;
    opts.server_url = "https://mcp.example/mcp";
    opts.redirect_uri = "http://localhost/callback";
    opts.client_id = "http://client.example/client";
    OAuthClientProvider provider(opts);
    EXPECT_THROW((void)provider.Authenticate(), McpError);
    EXPECT_THROW((void)provider.AuthenticateClientCredentials(), McpError);
}

TEST(OAuthTest, CimdRootPathDocumentUrlRejectedBeforeDiscovery) {
    OAuthClientOptions opts;
    opts.server_url = "https://mcp.example/mcp";
    opts.redirect_uri = "http://localhost/callback";
    opts.client_id = "https://client.example/";
    OAuthClientProvider provider(opts);
    EXPECT_THROW((void)provider.Authenticate(), McpError);
}

TEST(OAuthTest, CimdUserinfoFragmentAndMissingPathRejectedBeforeDiscovery) {
    const std::vector<std::string> invalid = {
        "https://user@client.example/client",
        "https://client.example/client#section",
        "https://client.example?query=1",
        "https:///client"
    };
    for (const auto& client_id : invalid) {
        OAuthClientOptions opts;
        opts.server_url = "https://mcp.example/mcp";
        opts.redirect_uri = "http://localhost/callback";
        opts.client_id = client_id;
        OAuthClientProvider provider(opts);
        EXPECT_THROW((void)provider.Authenticate(), McpError);
    }
}

TEST(OAuthTest, AuthClosedLoopScopeInsufficientRetryCaseInsensitive) {
    auto port = PickFreePort(kTestBasePort + 1240);
    std::atomic<int> calls{0};
    std::atomic<bool> handler_called{false};
    std::string second_auth;

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/mcp",
        [&calls, &second_auth](const mcp::HttpRequest& req,
                               mcp::HttpResponse& resp) {
            if (calls.fetch_add(1) == 0) {
                resp.status_code = 403;
                resp.status_text = "Forbidden";
                resp.headers["WWW-Authenticate"] =
                    "Bearer error=\"INSUFFICIENT_SCOPE\", scope=\"extra-scope\"";
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
    opts.auth_challenge_handler =
        [&handler_called](std::string_view www_auth) -> std::string {
            handler_called.store(
                www_auth.find("INSUFFICIENT_SCOPE") != std::string::npos);
            return "Bearer upgraded-token";
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
    EXPECT_TRUE(handler_called.load());
    EXPECT_EQ(second_auth, "Bearer upgraded-token");

    session->Close();
    server.Stop();
}

TEST(OAuthTest, AuthChallengeForbiddenWithoutScopeSkipsRetry) {
    auto port = PickFreePort(kTestBasePort + 1200);
    std::atomic<int> calls{0};
    std::atomic<bool> handler_called{false};

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/mcp",
        [&calls](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            calls.fetch_add(1);
            resp.status_code = 403;
            resp.status_text = "Forbidden";
            resp.headers["WWW-Authenticate"] = "Bearer error=\"invalid_token\"";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    mcp::HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    opts.auth_challenge_handler =
        [&handler_called](std::string_view) -> std::string {
            handler_called.store(true);
            return "Bearer unused-token";
        };
    auto transport = std::make_shared<mcp::StreamableHttpClientTransport>(opts);
    auto session = transport->Connect();

    mcp::JsonRpcRequest rpc;
    rpc.id = mcp::RequestId{int64_t(1)};
    rpc.method = "ping";
    session->SendMessageAsync(mcp::JsonRpcMessage{rpc});
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    EXPECT_EQ(calls.load(), 1);
    EXPECT_FALSE(handler_called.load());

    session->Close();
    server.Stop();
}

TEST(OAuthTest, PkceVerifierIsThirtyTwoBytesAndUnique) {
    std::set<std::string> seen;
    for (int i = 0; i < 64; ++i) {
        auto verifier = pkce::GenerateCodeVerifier();
        EXPECT_EQ(verifier.size(), 43U);
        EXPECT_EQ(verifier.find('='), std::string::npos);
        EXPECT_TRUE(seen.insert(verifier).second);
    }
}

TEST(OAuthTest, SecureEndpointPolicyRequiresHttpsExceptLoopback) {
    EXPECT_FALSE(OAuthClientProvider::IsSecureEndpoint("http://auth.example.com/token"));
    EXPECT_FALSE(OAuthClientProvider::IsSecureEndpoint("http://10.0.0.5/token"));
    EXPECT_FALSE(OAuthClientProvider::IsSecureEndpoint("http://example.com"));
    EXPECT_FALSE(OAuthClientProvider::IsSecureEndpoint("ftp://example.com/token"));
    EXPECT_TRUE(OAuthClientProvider::IsSecureEndpoint("https://auth.example.com/token"));
    EXPECT_TRUE(OAuthClientProvider::IsSecureEndpoint("http://localhost:8080/token"));
    EXPECT_TRUE(OAuthClientProvider::IsSecureEndpoint("http://127.0.0.1:8080/token"));
    EXPECT_TRUE(OAuthClientProvider::IsSecureEndpoint("http://[::1]:8080/token"));
}

TEST(OAuthTest, SameOriginComparisonNormalizesDefaultPorts) {
    EXPECT_TRUE(OAuthClientProvider::IsSameOrigin(
        "https://example.com/token", "https://example.com/authorize"));
    EXPECT_TRUE(OAuthClientProvider::IsSameOrigin(
        "http://127.0.0.1:9000/a", "http://127.0.0.1:9000/b"));
    EXPECT_FALSE(OAuthClientProvider::IsSameOrigin(
        "https://example.com/token", "https://example.com:8443/token"));
    EXPECT_FALSE(OAuthClientProvider::IsSameOrigin(
        "https://example.com/token", "http://example.com/token"));
}

TEST(OAuthTest, DiscoveredPlaintextTokenEndpointRejected) {
    auto port = PickFreePort(kTestBasePort + 1230);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = R"json({"issuer":"http://127.0.0.1:1",)"
                        R"json("authorization_endpoint":"http://127.0.0.1:1/authorize",)"
                        R"json("token_endpoint":"http://evil.example.com/token"})json";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.redirect_uri = "http://localhost/callback";
    opts.allow_legacy_metadata_fallback = true;
    OAuthClientProvider provider(opts);
    EXPECT_THROW((void)provider.Authenticate(), McpError);
    server.Stop();
}

TEST(OAuthTest, InvalidGrantFallsBackToAuthorizationFlow) {
    auto port = PickFreePort(kTestBasePort + 1240);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<int> token_calls{0};

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\"}";
        });
    server.SetHandler("POST", "/token",
        [&token_calls](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            token_calls.fetch_add(1);
            resp.status_code = 400;
            resp.status_text = "Bad Request";
            resp.body = R"json({"error":"invalid_grant","error_description":"revoked"})json";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto cache = std::make_shared<InMemoryTokenCache>();
    TokenContainer tokens;
    tokens.access_token = "stale-access";
    tokens.refresh_token = "revoked-refresh";
    tokens.expires_at = 1;
    cache->StoreTokens(tokens);

    std::atomic<bool> redirect_called{false};
    std::string redirect_url;
    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.redirect_uri = "http://localhost/callback";
    opts.token_cache = cache;
    opts.allow_legacy_metadata_fallback = true;
    opts.require_pkce_metadata = false;
    opts.authorization_redirect_handler = [&](std::string_view url) {
        redirect_called.store(true);
        redirect_url = std::string(url);
    };
    opts.authorization_code_callback = []() -> std::optional<AuthorizationCodeResult> {
        return std::nullopt;
    };

    OAuthClientProvider provider(opts);
    EXPECT_FALSE(provider.Authenticate());
    EXPECT_EQ(token_calls.load(), 1);
    EXPECT_TRUE(redirect_called.load());
    EXPECT_NE(redirect_url.find("code_challenge="), std::string::npos);
    EXPECT_NE(redirect_url.find("code_challenge_method=S256"), std::string::npos);
    EXPECT_FALSE(cache->GetTokens().has_value());
    server.Stop();
}

TEST(OAuthTest, AuthenticateAndPublicRefreshCoalesceToOneRequest) {
    auto port = PickFreePort(kTestBasePort + 1178);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<int> refresh_calls{0};

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/token",
        [&](const mcp::HttpRequest& req, mcp::HttpResponse& resp) {
            if (req.body.find("grant_type=refresh_token") != std::string::npos) {
                refresh_calls.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                resp.body = "{\"access_token\":\"coalesced-token\","
                            "\"token_type\":\"Bearer\",\"expires_in\":3600,"
                            "\"refresh_token\":\"rt-two\",\"iss\":\"" + issuer + "\"}";
            } else {
                resp.body = "{\"access_token\":\"short-token\","
                            "\"token_type\":\"Bearer\",\"expires_in\":0,"
                            "\"refresh_token\":\"rt-one\",\"iss\":\"" + issuer + "\"}";
            }
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "test-client";
    opts.client_secret = "test-secret";
    opts.allow_legacy_metadata_fallback = true;
    OAuthClientProvider provider(opts);
    ASSERT_TRUE(provider.AuthenticateClientCredentials());

    std::atomic<int> successes{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < 4; ++i) {
        callers.emplace_back([&provider, &successes, i] {
            bool ok = i < 2 ? provider.RefreshTokens() : provider.Authenticate();
            if (ok) successes.fetch_add(1);
        });
    }
    for (auto& caller : callers) caller.join();

    EXPECT_EQ(refresh_calls.load(), 1);
    EXPECT_EQ(successes.load(), 4);
    EXPECT_EQ(provider.GetAccessToken(), "coalesced-token");
    server.Stop();
}

TEST(OAuthTest, RefreshSingleFlightReleasesWaitersAfterOwnerException) {
    auto port = PickFreePort(kTestBasePort + 1179);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<int> refresh_calls{0};
    std::atomic<bool> refresh_started{false};

    mcp::HttpServer server(port);
    server.SetHandler("POST", "/token",
        [&](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            refresh_calls.fetch_add(1);
            refresh_started.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            resp.body = "{\"access_token\":\"after-fault-token\","
                        "\"token_type\":\"Bearer\",\"expires_in\":3600,"
                        "\"refresh_token\":\"rt-two\",\"iss\":\"" + issuer + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    struct FaultCache : InMemoryTokenCache {
        std::atomic<bool> fail_once{true};
        void StoreTokens(const TokenContainer& tokens) override {
            if (fail_once.exchange(false)) {
                throw McpError(McpErrorCode::InternalError, "injected cache fault");
            }
            InMemoryTokenCache::StoreTokens(tokens);
        }
    };
    auto cache = std::make_shared<FaultCache>();
    TokenContainer short_token;
    short_token.access_token = "short-token";
    short_token.refresh_token = "rt-one";
    short_token.token_type = "Bearer";
    short_token.expires_at = 1;
    cache->InMemoryTokenCache::StoreTokens(short_token);

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "test-client";
    opts.client_secret = "test-secret";
    opts.token_cache = cache;
    opts.allow_legacy_metadata_fallback = true;
    OAuthClientProvider provider(opts);

    std::atomic<bool> owner_fault{false};
    bool waiter_result = true;
    MCP_RUN_WITH_TIMEOUT(std::chrono::seconds(20), [&] {
        std::thread waiter([&] {
            while (!refresh_started.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            try {
                waiter_result = provider.RefreshTokens();
            } catch (...) {
                waiter_result = true;
            }
        });
        try {
            (void)provider.RefreshTokens();
        } catch (const McpError&) {
            owner_fault.store(true);
        }
        waiter.join();
    });

    EXPECT_TRUE(owner_fault.load());
    EXPECT_FALSE(waiter_result);
    EXPECT_EQ(refresh_calls.load(), 1);
    EXPECT_TRUE(provider.RefreshTokens());
    EXPECT_EQ(provider.GetAccessToken(), "after-fault-token");
    server.Stop();
}

TEST(OAuthTest, ServerErrorDuringRefreshKeepsCachedTokens) {
    auto port = PickFreePort(kTestBasePort + 1250);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\"}";
        });
    server.SetHandler("POST", "/token",
        [](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.status_code = 503;
            resp.status_text = "Service Unavailable";
            resp.body = R"json({"error":"temporarily_unavailable"})json";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto cache = std::make_shared<InMemoryTokenCache>();
    TokenContainer tokens;
    tokens.access_token = "still-valid";
    tokens.refresh_token = "refresh-token";
    tokens.expires_at = 1;
    cache->StoreTokens(tokens);

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.redirect_uri = "http://localhost/callback";
    opts.token_cache = cache;
    OAuthClientProvider provider(opts);
    EXPECT_FALSE(provider.Authenticate());
    auto retained = cache->GetTokens();
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained->access_token, "still-valid");
    server.Stop();
}

TEST(OAuthTest, MissingMetadataFailsWithoutCrash) {
    auto cache = std::make_shared<InMemoryTokenCache>();
    TokenContainer tokens;
    tokens.access_token = "stale";
    tokens.refresh_token = "refresh";
    tokens.expires_at = 1;
    cache->StoreTokens(tokens);

    OAuthClientOptions opts;
    opts.server_url = "";
    opts.redirect_uri = "http://localhost/callback";
    opts.token_cache = cache;
    OAuthClientProvider provider(opts);

    EXPECT_NO_THROW({
        EXPECT_FALSE(provider.RefreshTokens());
        EXPECT_FALSE(provider.StepUpAuthorization({"extra"}));
        EXPECT_FALSE(provider.Authenticate());
    });
    EXPECT_TRUE(provider.GetAuthorizationHeader().empty());
}

TEST(OAuthTest, IssuerAnchorPersistsInTokenCacheAcrossProviders) {
    auto port = PickFreePort(kTestBasePort + 1260);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/meta",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\"}";
        });
    server.SetHandler("GET", "/other",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "/tenant-x\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto cache = std::make_shared<InMemoryTokenCache>();
    TokenContainer tokens;
    tokens.access_token = "kept-token";
    tokens.expires_at = 9999999999999LL;
    cache->StoreTokens(tokens);

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.redirect_uri = "http://localhost/callback";
    opts.token_cache = cache;

    OAuthClientProvider first(opts);
    EXPECT_TRUE(first.HandleAuthChallenge(
        "Bearer resource_metadata=\"" + issuer + "/meta\""));
    auto anchor = cache->GetBoundIssuer();
    ASSERT_TRUE(anchor.has_value());
    EXPECT_EQ(*anchor, issuer);

    OAuthClientProvider second(opts);
    EXPECT_FALSE(second.HandleAuthChallenge(
        "Bearer resource_metadata=\"" + issuer + "/other\""));
    auto retained = cache->GetTokens();
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained->access_token, "kept-token");
    server.Stop();
}

TEST(OAuthTest, IssuerChangeThroughDiscoveryRejectedAndTokensRetained) {
    auto first_port = PickFreePort(kTestBasePort + 1270);
    auto second_port = PickFreePort(kTestBasePort + 1280);
    std::string first_issuer = "http://127.0.0.1:" + std::to_string(first_port);
    std::string second_issuer = "http://127.0.0.1:" + std::to_string(second_port);

    mcp::HttpServer first_server(first_port);
    first_server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [first_issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + first_issuer + "\","
                        "\"authorization_endpoint\":\"" + first_issuer + "/authorize\","
                        "\"token_endpoint\":\"" + first_issuer + "/token\"}";
        });
    mcp::HttpServer second_server(second_port);
    second_server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [second_issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + second_issuer + "\","
                        "\"authorization_endpoint\":\"" + second_issuer + "/authorize\","
                        "\"token_endpoint\":\"" + second_issuer + "/token\"}";
        });
    first_server.Start();
    second_server.Start();
    ASSERT_TRUE(WaitUntilReady(first_port));
    ASSERT_TRUE(WaitUntilReady(second_port));

    auto cache = std::make_shared<InMemoryTokenCache>();
    TokenContainer tokens;
    tokens.access_token = "kept-token";
    tokens.expires_at = 9999999999999LL;
    cache->StoreTokens(tokens);

    OAuthClientOptions first_opts;
    first_opts.server_url = first_issuer;
    first_opts.redirect_uri = "http://localhost/callback";
    first_opts.token_cache = cache;
    OAuthClientProvider first(first_opts);
    EXPECT_TRUE(first.HandleAuthChallenge(
        "Bearer resource_metadata=\"" + first_issuer +
        "/.well-known/oauth-authorization-server\""));

    OAuthClientOptions second_opts;
    second_opts.server_url = second_issuer;
    second_opts.redirect_uri = "http://localhost/callback";
    second_opts.token_cache = cache;
    OAuthClientProvider second(second_opts);
    EXPECT_FALSE(second.Authenticate());
    auto retained = cache->GetTokens();
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained->access_token, "kept-token");

    first_server.Stop();
    second_server.Stop();
}

TEST(OAuthTest, ConcurrentAuthorizationFlowsUseDistinctState) {
    auto port = PickFreePort(kTestBasePort + 2160);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\","
                        "\"code_challenge_methods_supported\":[\"S256\"]}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto cache = std::make_shared<InMemoryTokenCache>();
    std::mutex states_mutex;
    std::vector<std::string> states;

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.redirect_uri = "http://localhost/callback";
    opts.token_cache = cache;
    opts.allow_legacy_metadata_fallback = true;
    opts.authorization_redirect_handler = [&](std::string_view url) {
        std::string text(url);
        auto pos = text.find("state=");
        if (pos == std::string::npos) return;
        auto end = text.find('&', pos);
        std::lock_guard<std::mutex> lock(states_mutex);
        states.push_back(text.substr(pos + 6, end == std::string::npos
            ? std::string::npos : end - pos - 6));
    };
    opts.authorization_code_callback = []() -> std::optional<AuthorizationCodeResult> {
        return std::nullopt;
    };

    OAuthClientProvider provider(opts);
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&provider]() {
            for (int n = 0; n < 3; ++n) {
                EXPECT_FALSE(provider.StepUpAuthorization({"scope"}));
            }
        });
    }
    for (auto& worker : workers) worker.join();

    std::lock_guard<std::mutex> lock(states_mutex);
    EXPECT_EQ(states.size(), 12U);
    std::set<std::string> unique(states.begin(), states.end());
    EXPECT_EQ(unique.size(), states.size());
    server.Stop();
}

TEST(OAuthTest, TriggerWebhookLogDoesNotLeakWebhookUrl) {
    std::string webhook = "http://127.0.0.1:1/services/T00000000/SUPERSECRETTOKEN";
    std::string captured;
    auto previous_handler = mcp::GetLogHandler();
    auto previous_level = mcp::GetLogLevel();

    mcp::SetLogLevel(mcp::LogLevel::Warning);
    mcp::SetLogHandler([&captured](const mcp::LogRecord& record) {
        captured += std::string(record.message);
        captured += "\n";
    });
    mcp::ext::PostTriggerWebhook(webhook, "{}", std::chrono::milliseconds(200));
    mcp::SetLogHandler(previous_handler);
    mcp::SetLogLevel(previous_level);

    EXPECT_FALSE(captured.empty());
    EXPECT_EQ(captured.find(webhook), std::string::npos);
    EXPECT_EQ(captured.find("SUPERSECRETTOKEN"), std::string::npos);
    EXPECT_NE(captured.find("127.0.0.1"), std::string::npos);
}

TEST(OAuthFallbackTest, StrictDefaults) {
    OAuthClientOptions opts;
    EXPECT_TRUE(opts.require_iss_when_advertised);
    EXPECT_TRUE(opts.require_pkce_metadata);
    EXPECT_FALSE(opts.allow_legacy_metadata_fallback);
}

TEST(OAuthFallbackTest, PrmReachableWithValidAsMetadata) {
    auto resource_port = PickFreePort(kTestBasePort + 2100);
    auto as_port = PickFreePort(kTestBasePort + 2110);
    std::string resource = "http://127.0.0.1:" + std::to_string(resource_port);
    std::string as = "http://127.0.0.1:" + std::to_string(as_port);

    mcp::HttpServer resource_server(resource_port);
    resource_server.SetHandler("GET", "/.well-known/oauth-protected-resource",
        [as, resource](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"resource\":\"" + resource + "\","
                        "\"authorization_servers\":[\"" + as + "\"]}";
        });
    mcp::HttpServer as_server(as_port);
    as_server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [as](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + as + "\","
                        "\"authorization_endpoint\":\"" + as + "/authorize\","
                        "\"token_endpoint\":\"" + as + "/token\"}";
        });
    resource_server.Start();
    as_server.Start();
    ASSERT_TRUE(WaitUntilReady(resource_port));
    ASSERT_TRUE(WaitUntilReady(as_port));

    auto enabled = OAuthMetadata::Discover(resource);
    ASSERT_TRUE(enabled.has_value());
    EXPECT_EQ(enabled->issuer, as);

    auto disabled = OAuthMetadata::Discover(resource, false);
    ASSERT_TRUE(disabled.has_value());
    EXPECT_EQ(disabled->issuer, as);

    resource_server.Stop();
    as_server.Stop();
}

TEST(OAuthFallbackTest, PrmUnreachable) {
    auto port = PickFreePort(kTestBasePort + 2120);
    std::string origin = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [origin](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + origin + "\","
                        "\"authorization_endpoint\":\"" + origin + "/authorize\","
                        "\"token_endpoint\":\"" + origin + "/token\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto enabled = OAuthMetadata::Discover(origin, true);
    ASSERT_TRUE(enabled.has_value());
    EXPECT_EQ(enabled->issuer, origin);

    auto disabled = OAuthMetadata::Discover(origin, false);
    EXPECT_FALSE(disabled.has_value());

    server.Stop();
}

TEST(OAuthFallbackTest, EmptyAsMetadata) {
    auto resource_port = PickFreePort(kTestBasePort + 2130);
    auto as_port = PickFreePort(kTestBasePort + 2140);
    std::string resource = "http://127.0.0.1:" + std::to_string(resource_port);
    std::string as = "http://127.0.0.1:" + std::to_string(as_port);

    mcp::HttpServer resource_server(resource_port);
    resource_server.SetHandler("GET", "/.well-known/oauth-protected-resource",
        [as, resource](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"resource\":\"" + resource + "\","
                        "\"authorization_servers\":[\"" + as + "\"]}";
        });
    resource_server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [resource](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + resource + "\","
                        "\"authorization_endpoint\":\"" + resource + "/authorize\","
                        "\"token_endpoint\":\"" + resource + "/token\"}";
        });
    mcp::HttpServer as_server(as_port);
    as_server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{}";
        });
    resource_server.Start();
    as_server.Start();
    ASSERT_TRUE(WaitUntilReady(resource_port));
    ASSERT_TRUE(WaitUntilReady(as_port));

    auto enabled = OAuthMetadata::Discover(resource, true);
    ASSERT_TRUE(enabled.has_value());
    EXPECT_EQ(enabled->issuer, resource);

    auto disabled = OAuthMetadata::Discover(resource, false);
    EXPECT_FALSE(disabled.has_value());

    resource_server.Stop();
    as_server.Stop();
}

TEST(OAuthFallbackTest, ProviderHonorsFallbackOption) {
    auto port = PickFreePort(kTestBasePort + 2150);
    std::string origin = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<int> rfc8414_requests{0};

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [origin, &rfc8414_requests](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            ++rfc8414_requests;
            resp.body = "{\"issuer\":\"" + origin + "\","
                        "\"authorization_endpoint\":\"" + origin + "/authorize\","
                        "\"token_endpoint\":\"" + origin + "/token\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    {
        OAuthClientOptions opts;
        opts.server_url = origin;
        opts.client_id = "client";
        opts.client_secret = "secret";
        opts.allow_legacy_metadata_fallback = false;
        OAuthClientProvider provider(opts);
        (void)provider.AuthenticateClientCredentials();
    }
    const int after_disabled = rfc8414_requests.load();
    EXPECT_EQ(after_disabled, 0);

    {
        OAuthClientOptions opts;
        opts.server_url = origin;
        opts.client_id = "client";
        opts.client_secret = "secret";
        opts.allow_legacy_metadata_fallback = true;
        OAuthClientProvider provider(opts);
        (void)provider.AuthenticateClientCredentials();
    }
    EXPECT_GT(rfc8414_requests.load(), after_disabled);

    server.Stop();
}

TEST(OAuthTest, AuthorizationResponseIssFourStatesByDefault) {
    auto port = PickFreePort(kTestBasePort + 2170);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<int> token_hits{0};

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-protected-resource",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"resource\":\"" + issuer + "\","
                        "\"authorization_servers\":[\"" + issuer + "\"]}";
        });
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\","
                        "\"authorization_response_iss_parameter_supported\":true,"
                        "\"code_challenge_methods_supported\":[\"S256\"]}";
        });
    server.SetHandler("POST", "/token",
        [&token_hits, issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            token_hits.fetch_add(1);
            resp.body = "{\"access_token\":\"iss-token\",\"token_type\":\"Bearer\","
                        "\"expires_in\":3600,\"iss\":\"" + issuer + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto run = [&](std::optional<std::string> iss) {
        std::string seen_url;
        OAuthClientOptions opts;
        opts.server_url = issuer;
        opts.redirect_uri = "http://localhost/callback";
        opts.authorization_redirect_handler =
            [&seen_url](std::string_view url) { seen_url = std::string(url); };
        opts.authorization_code_callback =
            [&seen_url, iss]() -> std::optional<AuthorizationCodeResult> {
                AuthorizationCodeResult result;
                result.code = "code";
                auto pos = seen_url.find("state=");
                if (pos != std::string::npos) {
                    auto end = seen_url.find('&', pos);
                    auto len = end == std::string::npos
                        ? std::string::npos : end - pos - 6;
                    result.state = seen_url.substr(pos + 6, len);
                }
                result.iss = iss;
                return result;
            };
        OAuthClientProvider provider(opts);
        return provider.Authenticate();
    };

    EXPECT_TRUE(run(issuer));
    EXPECT_EQ(token_hits.load(), 1);
    EXPECT_FALSE(run(std::nullopt));
    EXPECT_FALSE(run(std::string{}));
    EXPECT_FALSE(run(issuer + "/evil"));
    EXPECT_EQ(token_hits.load(), 1);
    server.Stop();
}

TEST(OAuthTest, MissingIssAcceptedWithExplicitLegacyOptOut) {
    auto port = PickFreePort(kTestBasePort + 2180);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<int> token_hits{0};

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-protected-resource",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"resource\":\"" + issuer + "\","
                        "\"authorization_servers\":[\"" + issuer + "\"]}";
        });
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\","
                        "\"authorization_response_iss_parameter_supported\":true,"
                        "\"code_challenge_methods_supported\":[\"S256\"]}";
        });
    server.SetHandler("POST", "/token",
        [&token_hits, issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            token_hits.fetch_add(1);
            resp.body = "{\"access_token\":\"legacy-iss-token\",\"token_type\":\"Bearer\","
                        "\"expires_in\":3600,\"iss\":\"" + issuer + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string seen_url;
    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.redirect_uri = "http://localhost/callback";
    opts.require_iss_when_advertised = false;
    opts.authorization_redirect_handler =
        [&seen_url](std::string_view url) { seen_url = std::string(url); };
    opts.authorization_code_callback =
        [&seen_url]() -> std::optional<AuthorizationCodeResult> {
            AuthorizationCodeResult result;
            result.code = "code";
            auto pos = seen_url.find("state=");
            if (pos != std::string::npos) {
                auto end = seen_url.find('&', pos);
                auto len = end == std::string::npos
                    ? std::string::npos : end - pos - 6;
                result.state = seen_url.substr(pos + 6, len);
            }
            return result;
        };
    OAuthClientProvider provider(opts);
    EXPECT_TRUE(provider.Authenticate());
    EXPECT_EQ(token_hits.load(), 1);
    server.Stop();
}

TEST(OAuthTest, PkceMetadataMissingRefusesFlowByDefault) {
    auto port = PickFreePort(kTestBasePort + 2190);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<int> token_hits{0};
    bool callback_called = false;

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-protected-resource",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"resource\":\"" + issuer + "\","
                        "\"authorization_servers\":[\"" + issuer + "\"]}";
        });
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\"}";
        });
    server.SetHandler("POST", "/token",
        [&token_hits](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            token_hits.fetch_add(1);
            resp.body = R"({"access_token":"should-not-issue"})";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.redirect_uri = "http://localhost/callback";
    opts.authorization_redirect_handler = [](std::string_view) {};
    opts.authorization_code_callback =
        [&callback_called]() -> std::optional<AuthorizationCodeResult> {
            callback_called = true;
            return std::nullopt;
        };
    OAuthClientProvider provider(opts);
    EXPECT_FALSE(provider.Authenticate());
    EXPECT_FALSE(callback_called);
    EXPECT_EQ(token_hits.load(), 0);
    server.Stop();
}

TEST(OAuthTest, PrmResourceRequiresExactMatch) {
    auto resource_port = PickFreePort(kTestBasePort + 2200);
    auto as_port = PickFreePort(kTestBasePort + 2210);
    std::string origin = "http://127.0.0.1:" + std::to_string(resource_port);
    std::string resource = origin + "/mcp";
    std::string as = "http://127.0.0.1:" + std::to_string(as_port);
    std::atomic<int> mode{0};

    mcp::HttpServer resource_server(resource_port);
    auto prm = [as, origin, resource, &mode](
        const mcp::HttpRequest&, mcp::HttpResponse& resp) {
        std::string value = resource;
        switch (mode.load()) {
        case 1: value = origin; break;
        case 2: value = resource + "/"; break;
        case 3: value = "*"; break;
        case 4: value = "evil" + resource; break;
        default: break;
        }
        resp.body = "{\"resource\":\"" + value + "\","
                    "\"authorization_servers\":[\"" + as + "\"]}";
    };
    resource_server.SetHandler("GET", "/mcp/.well-known/oauth-protected-resource", prm);
    resource_server.SetHandler("GET", "/.well-known/oauth-protected-resource", prm);
    mcp::HttpServer as_server(as_port);
    as_server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [as](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + as + "\","
                        "\"authorization_endpoint\":\"" + as + "/authorize\","
                        "\"token_endpoint\":\"" + as + "/token\"}";
        });
    resource_server.Start();
    as_server.Start();
    ASSERT_TRUE(WaitUntilReady(resource_port));
    ASSERT_TRUE(WaitUntilReady(as_port));

    for (int value = 1; value <= 4; ++value) {
        mode.store(value);
        EXPECT_FALSE(OAuthMetadata::Discover(resource, false).has_value());
    }
    mode.store(0);
    auto exact = OAuthMetadata::Discover(resource, false);
    ASSERT_TRUE(exact.has_value());
    EXPECT_TRUE(exact->resource.has_value());
    EXPECT_EQ(*exact->resource, resource);

    auto overridden = OAuthMetadata::Discover(origin, false, std::string_view(resource));
    ASSERT_TRUE(overridden.has_value());
    EXPECT_EQ(*overridden->resource, resource);

    resource_server.Stop();
    as_server.Stop();
}

TEST(OAuthTest, ChallengeResourcePrefixRejected) {
    auto port = PickFreePort(kTestBasePort + 2220);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<int> mode{0};

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/meta",
        [issuer, &mode](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            std::string value = mode.load() == 0 ? issuer : issuer + "/tenant";
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\","
                        "\"resource\":\"" + value + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.redirect_uri = "http://localhost/callback";
    OAuthClientProvider provider(opts);
    const std::string challenge = "Bearer resource_metadata=\"" + issuer + "/meta\"";
    EXPECT_TRUE(provider.HandleAuthChallenge(challenge));
    mode.store(1);
    EXPECT_FALSE(provider.HandleAuthChallenge(challenge));
    server.Stop();
}

TEST(OAuthTest, Sha256FallbackWorksWithoutOpenSsl) {
#ifndef MCP_HAVE_OPENSSL
    EXPECT_EQ(pkce::ComputeCodeChallenge(
        "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"),
        "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    EXPECT_GE(pkce::GenerateCodeVerifier().size(), 43U);
#endif
}

TEST(OAuthTest, PrivateKeyJwtFailsClosedWithoutOpenSsl) {
#ifndef MCP_HAVE_OPENSSL
    auto port = PickFreePort(kTestBasePort + 2230);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-protected-resource",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"resource\":\"" + issuer + "\","
                        "\"authorization_servers\":[\"" + issuer + "\"]}";
        });
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\","
                        "\"token_endpoint_auth_methods_supported\":[\"private_key_jwt\"]}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.client_id = "jwt-client";
    opts.private_key_pem = kTestEs256PrivateKeyPem;
    opts.client_assertion_clock_seconds = [] { return int64_t{1700000000}; };
    OAuthClientProvider provider(opts);
    bool threw = false;
    try {
        (void)provider.AuthenticateClientCredentials();
    } catch (const McpError& e) {
        threw = true;
        EXPECT_EQ(e.Code(), McpErrorCode::InternalError);
    }
    EXPECT_TRUE(threw);
    server.Stop();
#endif
}

namespace {

class FakeUrlResolver final : public mcp::detail::net::IUrlSecurityResolver {
public:
    explicit FakeUrlResolver(std::vector<mcp::detail::net::UrlSecurityAddress> answers)
        : answers_(std::move(answers)) {}

    std::vector<mcp::detail::net::UrlSecurityAddress> Resolve(std::string_view) override {
        ++calls_;
        return answers_;
    }

    [[nodiscard]] int Calls() const { return calls_; }

private:
    std::vector<mcp::detail::net::UrlSecurityAddress> answers_;
    int calls_{0};
};

mcp::detail::net::UrlSecurityAddress V4(unsigned char a, unsigned char b,
                                        unsigned char c, unsigned char d) {
    mcp::detail::net::UrlSecurityAddress address;
    address.family = 4;
    address.length = 4;
    address.bytes[0] = a;
    address.bytes[1] = b;
    address.bytes[2] = c;
    address.bytes[3] = d;
    return address;
}

mcp::detail::net::UrlSecurityAddress V6(std::initializer_list<unsigned char> values) {
    mcp::detail::net::UrlSecurityAddress address;
    address.family = 6;
    address.length = 16;
    std::size_t index = 0;
    for (unsigned char value : values) {
        if (index >= address.bytes.size()) break;
        address.bytes[index++] = value;
    }
    return address;
}

class LogCapture final {
public:
    LogCapture()
        : previous_handler_(GetLogHandler())
        , previous_level_(GetLogLevel())
    {
        SetLogLevel(LogLevel::Error);
        SetLogHandler([this](const LogRecord& record) {
            std::lock_guard<std::mutex> lock(mutex_);
            records_.emplace_back(record.message);
        });
    }

    ~LogCapture() {
        SetLogHandler(previous_handler_);
        SetLogLevel(previous_level_);
    }

    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    [[nodiscard]] bool Contains(std::string_view needle) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& record : records_)
            if (record.find(needle) != std::string::npos) return true;
        return false;
    }

private:
    LogHandler previous_handler_;
    LogLevel previous_level_;
    mutable std::mutex mutex_;
    std::vector<std::string> records_;
};

std::string DcrMetadataBody(std::string_view issuer, bool with_registration) {
    JsonValue object(JsonValue::object_tag);
    object["issuer"] = JsonValue(std::string(issuer));
    object["authorization_endpoint"] = JsonValue(std::string(issuer) + "/authorize");
    object["token_endpoint"] = JsonValue(std::string(issuer) + "/token");
    if (with_registration)
        object["registration_endpoint"] = JsonValue(std::string(issuer) + "/register");
    return JsonValue(std::move(object)).Dump();
}

} // namespace

TEST(OAuthSecurityTest, OriginAllowlistExplicitOnlyAndPortNormalization) {
    using namespace mcp::detail::net;
    UrlSecurityPolicyConfig config;
    config.allowed_origins = {"https://Good.Example:8443", "https://plain.example"};
    config.resolver = std::make_shared<FakeUrlResolver>(
        std::vector<UrlSecurityAddress>{V4(93, 184, 215, 14)});
    UrlSecurityPolicy policy(std::move(config));
    std::string error;
    std::vector<UrlSecurityAddress> addresses;

    EXPECT_TRUE(policy.Validate("https://good.example:8443/token", error, addresses));
    EXPECT_TRUE(policy.Validate("https://GOOD.EXAMPLE:8443/token", error, addresses));
    EXPECT_TRUE(policy.Validate("https://plain.example/token", error, addresses));
    EXPECT_FALSE(policy.Validate("https://good.example/token", error, addresses));
    EXPECT_FALSE(policy.Validate("http://good.example:8443/token", error, addresses));
    EXPECT_FALSE(policy.Validate("https://evil.example/token", error, addresses));
    EXPECT_FALSE(policy.Validate("https://user@good.example:8443/token", error, addresses));
    EXPECT_FALSE(policy.Validate("https://%67ood.example:8443/token", error, addresses));

    EXPECT_FALSE(ParseUrlSecurityOrigin("https://2130706433/token").has_value());
    EXPECT_FALSE(ParseUrlSecurityOrigin("http://0x7f000001/token").has_value());
    EXPECT_FALSE(ParseUrlSecurityOrigin("http://user@127.0.0.1/token").has_value());
    EXPECT_FALSE(ParseUrlSecurityOrigin("http://%31%32%37.0.0.1/token").has_value());
}

TEST(OAuthSecurityTest, DnsAddressPolicyRejectsForbiddenClasses) {
    using namespace mcp::detail::net;
    EXPECT_EQ(ClassifyAddress(V4(127, 0, 0, 1)), UrlAddressClass::Loopback);
    EXPECT_EQ(ClassifyAddress(V4(10, 0, 0, 5)), UrlAddressClass::PrivateAddress);
    EXPECT_EQ(ClassifyAddress(V4(172, 16, 0, 1)), UrlAddressClass::PrivateAddress);
    EXPECT_EQ(ClassifyAddress(V4(192, 168, 1, 1)), UrlAddressClass::PrivateAddress);
    EXPECT_EQ(ClassifyAddress(V4(169, 254, 169, 254)), UrlAddressClass::LinkLocal);
    EXPECT_EQ(ClassifyAddress(V4(224, 0, 0, 1)), UrlAddressClass::Multicast);
    EXPECT_EQ(ClassifyAddress(V4(0, 0, 0, 0)), UrlAddressClass::Unspecified);
    EXPECT_EQ(ClassifyAddress(V4(240, 0, 0, 1)), UrlAddressClass::Reserved);
    EXPECT_EQ(ClassifyAddress(V4(100, 64, 0, 1)), UrlAddressClass::Reserved);
    EXPECT_EQ(ClassifyAddress(V4(8, 8, 8, 8)), UrlAddressClass::Public);
    EXPECT_EQ(ClassifyAddress(V6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})),
              UrlAddressClass::Unspecified);
    EXPECT_EQ(ClassifyAddress(V6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1})),
              UrlAddressClass::Loopback);
    EXPECT_EQ(ClassifyAddress(V6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255,
                                  127, 0, 0, 1})), UrlAddressClass::Mapped);
    EXPECT_EQ(ClassifyAddress(V6({0xfe, 0x80, 0, 0, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0, 1})), UrlAddressClass::LinkLocal);
    EXPECT_EQ(ClassifyAddress(V6({0xfc, 0, 0, 0, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0, 1})), UrlAddressClass::PrivateAddress);
    EXPECT_EQ(ClassifyAddress(V6({0xff, 0x02, 0, 0, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0, 1})), UrlAddressClass::Multicast);
    EXPECT_EQ(ClassifyAddress(V6({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0, 1})), UrlAddressClass::Reserved);
}

TEST(OAuthSecurityTest, DnsMultipleAnswersWithPrivateRejectedBeforeConnect) {
    using namespace mcp::detail::net;
    auto resolver = std::make_shared<FakeUrlResolver>(
        std::vector<UrlSecurityAddress>{V4(93, 184, 215, 14), V4(10, 0, 0, 5)});
    UrlSecurityPolicyConfig config;
    config.allowed_origins = {"https://multi.test"};
    config.resolver = resolver;
    UrlSecurityPolicy policy(std::move(config));
    std::string error;
    std::vector<UrlSecurityAddress> addresses;
    EXPECT_FALSE(policy.Validate("https://multi.test/token", error, addresses));
    EXPECT_EQ(error, "address_forbidden");
    EXPECT_EQ(resolver->Calls(), 1);

    auto provider_resolver = std::make_shared<FakeUrlResolver>(
        std::vector<UrlSecurityAddress>{V4(93, 184, 215, 14), V4(10, 0, 0, 5)});
    OAuthClientOptions options;
    options.server_url = "https://multi.test";
    options.client_id = "multi-client";
    options.client_secret = "multi-secret";
    options.dns_resolver = provider_resolver;
    OAuthClientProvider provider(options);
    EXPECT_FALSE(provider.AuthenticateClientCredentials());
    EXPECT_GE(provider_resolver->Calls(), 1);
}

TEST(OAuthSecurityTest, DnsAnswerSwappedBeforeConnectRejected) {
    auto port = PickFreePort(kTestBasePort + 1300);
    std::string issuer = "http://localhost:" + std::to_string(port);
    std::atomic<int> token_hits{0};
    mcp::HttpServer server(port);
    server.SetHandler("POST", "/token",
        [&token_hits](const mcp::HttpRequest&, mcp::HttpResponse& response) {
            token_hits.fetch_add(1);
            response.body = R"json({"access_token":"swapped"})json";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto resolver = std::make_shared<FakeUrlResolver>(
        std::vector<mcp::detail::net::UrlSecurityAddress>{V4(93, 184, 215, 14)});
    mcp::detail::net::UrlSecurityPolicyConfig config;
    config.allowed_origins = {issuer};
    config.resolver = resolver;
    auto policy = std::make_shared<const mcp::detail::net::UrlSecurityPolicy>(
        std::move(config));
    mcp::detail::net::HttpClient client;
    mcp::detail::net::HttpRequestSpec request;
    request.method = "POST";
    request.url = issuer + "/token";
    request.body = "grant_type=client_credentials";
    request.security_policy = policy;
    request.peer_address_override = []() -> std::optional<std::string> {
        return std::string("127.0.0.1");
    };
    EXPECT_THROW((void)client.Request(request), McpError);
    EXPECT_EQ(token_hits.load(), 0);
    EXPECT_EQ(resolver->Calls(), 1);
    server.Stop();
}

TEST(OAuthSecurityTest, MetadataCannotExpandOriginAllowlist) {
    auto port_a = PickFreePort(kTestBasePort + 1310);
    auto port_b = PickFreePort(kTestBasePort + 1320);
    std::string issuer_a = "http://127.0.0.1:" + std::to_string(port_a);
    std::atomic<int> server_b_hits{0};

    mcp::HttpServer server_a(port_a);
    server_a.SetHandler("GET", "/.well-known/oauth-protected-resource",
        [issuer_a, port_b](const mcp::HttpRequest&, mcp::HttpResponse& response) {
            response.body = "{\"resource\":\"" + issuer_a + "\","
                            "\"authorization_servers\":[\"http://127.0.0.1:" +
                            std::to_string(port_b) + "\"]}";
        });
    mcp::HttpServer server_b(port_b);
    server_b.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [&server_b_hits](const mcp::HttpRequest&, mcp::HttpResponse& response) {
            server_b_hits.fetch_add(1);
            response.body = R"json({"issuer":"http://127.0.0.1:1"})json";
        });
    server_a.Start();
    server_b.Start();
    ASSERT_TRUE(WaitUntilReady(port_a));
    ASSERT_TRUE(WaitUntilReady(port_b));

    OAuthClientOptions options;
    options.server_url = issuer_a;
    options.redirect_uri = issuer_a + "/callback";
    OAuthClientProvider provider(options);
    EXPECT_FALSE(provider.Authenticate());
    EXPECT_EQ(server_b_hits.load(), 0);
    server_a.Stop();
    server_b.Stop();
}

TEST(OAuthSecurityTest, EveryRedirectHopRevalidatedAndCrossOriginRejected) {
    auto port_a = PickFreePort(kTestBasePort + 1330);
    auto port_b = PickFreePort(kTestBasePort + 1340);
    std::atomic<int> server_b_hits{0};
    mcp::HttpServer server_a(port_a);
    server_a.SetHandler("GET", "/start",
        [port_b](const mcp::HttpRequest&, mcp::HttpResponse& response) {
            response.status_code = 302;
            response.headers["Location"] = "http://127.0.0.1:" +
                std::to_string(port_b) + "/landing";
        });
    mcp::HttpServer server_b(port_b);
    server_b.SetHandler("GET", "/landing",
        [&server_b_hits](const mcp::HttpRequest&, mcp::HttpResponse&) {
            server_b_hits.fetch_add(1);
        });
    server_a.Start();
    server_b.Start();
    ASSERT_TRUE(WaitUntilReady(port_a));
    ASSERT_TRUE(WaitUntilReady(port_b));

    mcp::detail::net::UrlSecurityPolicyConfig config;
    config.allowed_origins = {"http://127.0.0.1:" + std::to_string(port_a)};
    config.loopback_exceptions = {{"127.0.0.1", static_cast<std::uint16_t>(port_a)}};
    auto policy = std::make_shared<const mcp::detail::net::UrlSecurityPolicy>(
        std::move(config));
    mcp::detail::net::HttpClient client;
    mcp::detail::net::HttpRequestSpec request;
    request.method = "GET";
    request.url = "http://127.0.0.1:" + std::to_string(port_a) + "/start";
    request.security_policy = policy;
    EXPECT_THROW((void)client.Request(request), McpError);
    EXPECT_EQ(server_b_hits.load(), 0);
    server_a.Stop();
    server_b.Stop();
}

TEST(OAuthSecurityTest, RedirectBudgetEnforcedByPolicy) {
    auto port = PickFreePort(kTestBasePort + 1350);
    std::string origin = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<int> final_hits{0};
    mcp::HttpServer server(port);
    server.SetHandler("GET", "/a",
        [origin](const mcp::HttpRequest&, mcp::HttpResponse& response) {
            response.status_code = 302;
            response.headers["Location"] = origin + "/b";
        });
    server.SetHandler("GET", "/b",
        [origin](const mcp::HttpRequest&, mcp::HttpResponse& response) {
            response.status_code = 302;
            response.headers["Location"] = origin + "/c";
        });
    server.SetHandler("GET", "/c",
        [&final_hits](const mcp::HttpRequest&, mcp::HttpResponse&) {
            final_hits.fetch_add(1);
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    mcp::detail::net::UrlSecurityPolicyConfig config;
    config.allowed_origins = {origin};
    config.loopback_exceptions = {{"127.0.0.1", port}};
    config.max_redirects = 1;
    auto policy = std::make_shared<const mcp::detail::net::UrlSecurityPolicy>(
        std::move(config));
    mcp::detail::net::HttpClient client;
    mcp::detail::net::HttpRequestSpec request;
    request.method = "GET";
    request.url = origin + "/a";
    request.security_policy = policy;
    EXPECT_THROW((void)client.Request(request), McpError);
    EXPECT_EQ(final_hits.load(), 0);
    server.Stop();
}

TEST(OAuthSecurityTest, CrossOriginRedirectDropsSensitiveHeaders) {
    auto port_a = PickFreePort(kTestBasePort + 1360);
    auto port_b = PickFreePort(kTestBasePort + 1370);
    std::string origin_a = "http://127.0.0.1:" + std::to_string(port_a);
    std::string origin_b = "http://127.0.0.1:" + std::to_string(port_b);
    std::atomic<int> auth_hits{0};
    std::atomic<int> cookie_hits{0};
    std::atomic<int> secret_hits{0};
    std::atomic<int> keep_hits{0};

    mcp::HttpServer server_a(port_a);
    server_a.SetHandler("GET", "/start",
        [origin_b](const mcp::HttpRequest&, mcp::HttpResponse& response) {
            response.status_code = 302;
            response.headers["Location"] = origin_b + "/landing";
        });
    mcp::HttpServer server_b(port_b);
    server_b.SetHandler("GET", "/landing",
        [&](const mcp::HttpRequest& request, mcp::HttpResponse&) {
            if (request.headers.count("authorization")) auth_hits.fetch_add(1);
            if (request.headers.count("cookie")) cookie_hits.fetch_add(1);
            if (request.headers.count("x-client-secret")) secret_hits.fetch_add(1);
            if (request.headers.count("x-keep")) keep_hits.fetch_add(1);
        });
    server_a.Start();
    server_b.Start();
    ASSERT_TRUE(WaitUntilReady(port_a));
    ASSERT_TRUE(WaitUntilReady(port_b));

    mcp::detail::net::UrlSecurityPolicyConfig config;
    config.allowed_origins = {origin_a, origin_b};
    config.loopback_exceptions = {
        {"127.0.0.1", static_cast<std::uint16_t>(port_a)},
        {"127.0.0.1", static_cast<std::uint16_t>(port_b)}};
    config.allow_cross_origin_redirect = true;
    auto policy = std::make_shared<const mcp::detail::net::UrlSecurityPolicy>(
        std::move(config));
    mcp::detail::net::HttpClient client;
    mcp::detail::net::HttpRequestSpec request;
    request.method = "GET";
    request.url = origin_a + "/start";
    request.redirect_policy = mcp::detail::HttpRedirectPolicy::FollowAll;
    request.headers["Authorization"] = "Bearer leaked-token";
    request.headers["Cookie"] = "sid=leaked";
    request.headers["X-Client-Secret"] = "leaked-secret";
    request.headers["X-Keep"] = "ok";
    request.security_policy = policy;
    auto response = client.Request(request);
    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(auth_hits.load(), 0);
    EXPECT_EQ(cookie_hits.load(), 0);
    EXPECT_EQ(secret_hits.load(), 0);
    EXPECT_EQ(keep_hits.load(), 1);
    server_a.Stop();
    server_b.Stop();
}

TEST(OAuthSecurityTest, DcrSecretPriorityExplicitOverRegistered) {
    auto port = PickFreePort(kTestBasePort + 1380);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-protected-resource",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& response) {
            response.body = "{\"resource\":\"" + issuer + "\","
                            "\"authorization_servers\":[\"" + issuer + "\"]}";
        });
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& response) {
            response.body = DcrMetadataBody(issuer, true);
        });
    server.SetHandler("POST", "/register",
        [](const mcp::HttpRequest&, mcp::HttpResponse& response) {
            response.body = "{\"client_id\":\"dcr-client\","
                            "\"client_secret\":\"LEAKME_DCR_SECRET\","
                            "\"token_endpoint_auth_method\":\"client_secret_post\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto cache = std::make_shared<InMemoryTokenCache>();
    TokenContainer tokens;
    tokens.access_token = "cached-token";
    tokens.expires_at = 9999999999999LL;
    cache->StoreTokens(tokens);

    OAuthClientOptions options;
    options.server_url = issuer;
    options.redirect_uri = issuer + "/callback";
    options.client_secret = "EXPLICIT_SECRET";
    options.token_cache = cache;
    OAuthClientProvider explicit_provider(options);
    ASSERT_TRUE(explicit_provider.Authenticate());
    auto explicit_secret = explicit_provider.ResolveClientSecret();
    ASSERT_TRUE(explicit_secret.has_value());
    EXPECT_EQ(*explicit_secret, "EXPLICIT_SECRET");

    OAuthClientOptions second = options;
    second.client_secret.reset();
    OAuthClientProvider registered_provider(second);
    ASSERT_TRUE(registered_provider.Authenticate());
    auto registered = registered_provider.ResolveClientSecret();
    ASSERT_TRUE(registered.has_value());
    EXPECT_EQ(*registered, "LEAKME_DCR_SECRET");
    server.Stop();
}

TEST(OAuthSecurityTest, DcrSecretIsUsedForClientAuthentication) {
    auto port = PickFreePort(kTestBasePort + 1390);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    std::atomic<int> token_hits{0};
    std::string observed_secret;
    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-protected-resource",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& response) {
            response.body = "{\"resource\":\"" + issuer + "\","
                            "\"authorization_servers\":[\"" + issuer + "\"]}";
        });
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& response) {
            response.body = DcrMetadataBody(issuer, true);
        });
    server.SetHandler("POST", "/register",
        [](const mcp::HttpRequest&, mcp::HttpResponse& response) {
            response.body = "{\"client_id\":\"dcr-client\","
                            "\"client_secret\":\"LEAKME_DCR_SECRET\","
                            "\"token_endpoint_auth_method\":\"client_secret_post\"}";
        });
    server.SetHandler("POST", "/token",
        [&](const mcp::HttpRequest& request, mcp::HttpResponse& response) {
            token_hits.fetch_add(1);
            auto pos = request.body.find("client_secret=");
            if (pos != std::string::npos) {
                auto value = request.body.substr(pos + std::string("client_secret=").size());
                auto amp = value.find('&');
                observed_secret = amp == std::string::npos ? value : value.substr(0, amp);
            }
            response.body = "{\"access_token\":\"refreshed\",\"iss\":\"" + issuer + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto cache = std::make_shared<InMemoryTokenCache>();
    TokenContainer tokens;
    tokens.access_token = "old-token";
    tokens.refresh_token = "refresh-token";
    tokens.expires_at = 0;
    cache->StoreTokens(tokens);

    OAuthClientOptions options;
    options.server_url = issuer;
    options.redirect_uri = issuer + "/callback";
    options.token_cache = cache;
    OAuthClientProvider provider(options);
    ASSERT_TRUE(provider.Authenticate());
    EXPECT_EQ(token_hits.load(), 1);
    EXPECT_EQ(observed_secret, "LEAKME_DCR_SECRET");
    server.Stop();
}

TEST(OAuthSecurityTest, DcrInvalidSecretRejectedWithoutLeak) {
    auto port = PickFreePort(kTestBasePort + 1400);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);
    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-protected-resource",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& response) {
            response.body = "{\"resource\":\"" + issuer + "\","
                            "\"authorization_servers\":[\"" + issuer + "\"]}";
        });
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& response) {
            response.body = DcrMetadataBody(issuer, true);
        });
    server.SetHandler("POST", "/register",
        [](const mcp::HttpRequest&, mcp::HttpResponse& response) {
            response.body = "{\"client_id\":\"dcr-client\","
                            "\"client_secret\":\"LEAKME_INVALID_SECRET \"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    OAuthClientOptions options;
    options.server_url = issuer;
    options.redirect_uri = issuer + "/callback";
    OAuthClientProvider provider(options);
    LogCapture logs;
    std::string exception_text;
    try {
        (void)provider.Authenticate();
    } catch (const McpError& error) {
        exception_text = error.what();
        EXPECT_EQ(error.Code(), McpErrorCode::InvalidParams);
    }
    EXPECT_FALSE(exception_text.empty());
    EXPECT_EQ(exception_text.find("LEAKME_INVALID_SECRET"), std::string::npos);
    EXPECT_FALSE(logs.Contains("LEAKME_INVALID_SECRET"));
    server.Stop();
}

TEST(OAuthSecurityTest, SecretCacheKeyIsolatedByIssuerAndPrincipal) {
    using mcp::OAuthClientProvider;
    const std::string first = OAuthClientProvider::SecretCacheKey(
        "https://issuer-a", "client", "https://resource");
    EXPECT_NE(first, OAuthClientProvider::SecretCacheKey(
        "https://issuer-b", "client", "https://resource"));
    EXPECT_NE(first, OAuthClientProvider::SecretCacheKey(
        "https://issuer-a", "other-client", "https://resource"));
    EXPECT_NE(first, OAuthClientProvider::SecretCacheKey(
        "https://issuer-a", "client", "https://other-resource"));
}
