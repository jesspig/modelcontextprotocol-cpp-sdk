#include <mcp/JsonRpc.hpp>
#include <mcp/McpError.hpp>
#include <mcp/ErrorCodes.hpp>
#include <mcp/client/auth/OAuthClientProvider.hpp>
#include <mcp/client/auth/TokenCache.hpp>
#include <mcp/detail/Base64Url.hpp>
#include <mcp/ext/DPoP.hpp>
#include <mcp/http/HttpServer.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>
#include "TestServerUtil.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
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
    OAuthClientProvider provider(opts);

    ASSERT_TRUE(provider.AuthenticateClientCredentials());
    EXPECT_NE(seen_body.find("client_secret=test-secret"), std::string::npos);
    EXPECT_EQ(seen_body.find("client_assertion"), std::string::npos);
    EXPECT_EQ(provider.GetAccessToken(), "secret-token");
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
    OAuthClientProvider provider(opts);

    bool threw = false;
    try {
        provider.AuthenticateClientCredentials();
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
    provider.ObserveDPoPNonce("");
    provider.ObserveDPoPNonce(std::string(2048, 'n'));
    provider.ObserveDPoPNonce("nonce-abc");

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
    OAuthClientProvider provider(opts);
    EXPECT_TRUE(provider.AuthenticateClientCredentials());
    EXPECT_TRUE(provider.HasToken());
    EXPECT_EQ(provider.GetAccessToken(), "no-iss-token");
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

TEST(OAuthTest, IssuerChangeClearsTokensViaChallenge) {
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
    EXPECT_TRUE(provider.HandleAuthChallenge(
        "Bearer resource_metadata=\"" + issuer + "/meta2\""));
    EXPECT_FALSE(provider.HasToken());
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
    for (const auto& value : results) EXPECT_EQ(value, "refreshed-token");

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
                        "\"token_endpoint\":\"" + issuer + "/token\"}";
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
    opts.client_id = "http://127.0.0.1:9/unreachable-client";
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

TEST(OAuthTest, CimdDocumentClientIdMismatchFallsBack) {
    auto port = PickFreePort(kTestBasePort + 1220);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\"}";
        });
    server.SetHandler("GET", "/client",
        [](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = R"({"client_id":"https://other.example/client",)"
                        R"("token_endpoint_auth_method":"none",)"
                        R"("redirect_uris":["http://localhost/callback"]})";
        });
    server.SetHandler("POST", "/token",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"access_token\":\"cimd-mismatch-token\","
                        "\"token_type\":\"Bearer\",\"expires_in\":3600,"
                        "\"iss\":\"" + issuer + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string seen_url;
    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.redirect_uri = "http://localhost/callback";
    opts.client_id = issuer + "/client";
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
    EXPECT_EQ(provider.GetAccessToken(), "cimd-mismatch-token");
    server.Stop();
}

TEST(OAuthTest, CimdRedirectUriMismatchFallsBack) {
    auto port = PickFreePort(kTestBasePort + 1230);
    std::string issuer = "http://127.0.0.1:" + std::to_string(port);

    mcp::HttpServer server(port);
    server.SetHandler("GET", "/.well-known/oauth-authorization-server",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"issuer\":\"" + issuer + "\","
                        "\"authorization_endpoint\":\"" + issuer + "/authorize\","
                        "\"token_endpoint\":\"" + issuer + "/token\"}";
        });
    server.SetHandler("GET", "/client",
        [](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = R"({"redirect_uris":["http://localhost/other"]})";
        });
    server.SetHandler("POST", "/token",
        [issuer](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            resp.body = "{\"access_token\":\"cimd-redirect-token\","
                        "\"token_type\":\"Bearer\",\"expires_in\":3600,"
                        "\"iss\":\"" + issuer + "\"}";
        });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string seen_url;
    OAuthClientOptions opts;
    opts.server_url = issuer;
    opts.redirect_uri = "http://localhost/callback";
    opts.client_id = issuer + "/client";
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
    EXPECT_EQ(provider.GetAccessToken(), "cimd-redirect-token");
    server.Stop();
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
