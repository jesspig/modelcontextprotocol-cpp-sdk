# OAuth Support

The client supports the MCP OAuth authorization flow for servers that require authentication.

## Flow

1. **Metadata discovery**: RFC 9728 protected-resource metadata (`/.well-known/oauth-protected-resource`) first — the authorization server is located via `authorization_servers[0]` before its RFC 8414 metadata is fetched (a missing `resource` is carried over from the protected-resource document); each failed stage logs a warning and falls back — first to direct RFC 8414 discovery, finally to hardcoded `server_url` defaults (`issuer=server_url`, `authorize`/`token` suffixes) — never failing hard
2. **Authorization Code + PKCE** with S256 code challenge
3. **Client identity**: when `client_id` is an `http(s)://` URL, the Client ID Metadata Document (CIMD) is fetched and parsed; **Dynamic Client Registration** (DCR, HTTP POST to the registration endpoint) runs only when no `client_id` is configured
4. **Resource indicator**: the authorization request and all token requests carry a `resource` parameter per RFC 8707
5. **Token refresh** via preemptive expiry check (not 401-driven)
6. **Token revocation** via manual `Revoke()` call (best-effort call to the RFC 7009 revocation endpoint; failure does not affect local token clearing)

## OAuthClientOptions

| Field | Type | Description |
|-------|------|-------------|
| `server_url` | `string` | Authorization server base URL |
| `redirect_uri` | `string` | OAuth redirect URI |
| `client_id` | `optional<string>` | Client identifier (auto-registered if absent) |
| `client_secret` | `optional<string>` | Client secret (optional) |
| `scopes` | `vector<string>` | Requested OAuth scopes |
| `scope_filter` | `function<vector<string>(const vector<string>&)>` | Pre-send scope hook (empty by default, scopes sent as-is): the return value replaces `scope` in the authorization URL and the `client_credentials` token request; refresh requests carry no `scope` and a missing persisted scope falls back to the previous value, unaffected by this hook |
| `token_cache` | `shared_ptr<ITokenCache>` | Token persistence (default: `InMemoryTokenCache`) |
| `resource` | `optional<string>` | RFC 8707 resource indicator sent with the authorization request and token requests; falls back to the discovered metadata `resource`, then to `server_url`; when the resolved value is empty the parameter is omitted entirely; persisted as the token's audience — cached tokens are treated as invalid once the indicator changes |
| `application_type` | `string` | `application_type` sent during dynamic registration (default `"native"`) |
| `private_key_pem` | `optional<string>` | PKCS#8 EC private key PEM (P-256) used for `private_key_jwt` client authentication; mutually exclusive with `client_secret` — once configured, `client_secret` is no longer sent |
| `signing_algorithm` | `string` | Client assertion signing algorithm (default `"ES256"`; it is the only supported value, anything else throws `McpError`) |
| `client_assertion_clock_seconds` | `function<int64_t()>` | Clock hook providing the client assertion `iat`/`exp` (defaults to the system clock; lets tests inject a fixed time) |
| `authorization_redirect_handler` | `function<void(string_view url)>` | Callback to open the authorization URL |
| `authorization_code_callback` | `function<optional<AuthorizationCodeResult>()>` | Callback returning the authorization code, the echoed `state`, and the optional `iss` (`AuthorizationCodeResult{code, state, iss}`), `nullopt` on failure |
| `enable_dpop` | `bool` | DPoP switch (default `false`): proofs are minted and server `DPoP-Nonce` values observed only when on (requires OpenSSL) |
| `token_update_handler` | `function<void(const TokenContainer&)>` | Notification callback fired after tokens are stored/refreshed |

## Setup

```cpp
OAuthClientOptions oauth_opts;
oauth_opts.server_url = "https://auth.server.com";
oauth_opts.redirect_uri = "http://localhost:3000/callback";
oauth_opts.client_id = "my-client";
oauth_opts.scopes = {"profile", "email"};
oauth_opts.authorization_redirect_handler =
    [](std::string_view url) {
        // Open URL in browser for user to authorize
    };
oauth_opts.authorization_code_callback =
    []() -> std::optional<AuthorizationCodeResult> {
        // Return the authorization code, the `state` echoed back
        // by the authorization server (CSRF protection), and the `iss`
        // carried by the authorization response (RFC 9207; nullopt when absent)
        return AuthorizationCodeResult{"auth-code", "state", std::nullopt};
    };

auto auth = std::make_shared<OAuthClientProvider>(oauth_opts);
auth->Authenticate();
auto token = auth->GetAccessToken();
```

`OAuthClientProvider` is a standalone class — it is not passed to `McpClient::Create`.

## API

| Method | Description |
|--------|-------------|
| `Authenticate()` | Full OAuth flow: discover → register → authorize → token exchange (returns `bool`) |
| `GetAccessToken()` | Returns valid access token, auto-refreshes if expiring soon; throws `McpError` (InternalError) on refresh failure; concurrent refreshes coalesce into one flight, owner re-entry short-circuits |
| `RefreshTokens()` | Force-refresh using stored refresh token (returns `bool`); concurrent refreshes coalesce into one flight, owner re-entry short-circuits |
| `IsAuthenticated()` | Check if stored token is present and not expired |
| `HasToken()` | Check if any token exists (may be expired) |
| `GetAuthorizationHeader()` | Returns `"Bearer {token}"` string |
| `StepUpAuthorization(scopes)` | Re-authorize with additional scopes (returns `bool`) |
| `AuthenticateClientCredentials()` | Client credentials grant (RFC 6749 §4.4) for service-to-service scenarios without user interaction (returns `bool`) |
| `HandleAuthChallenge(www_authenticate)` | Handles a 401/403 authentication challenge header (RFC 9728); retries the original request on success (returns `bool`) |
| `Revoke()` | Best-effort call to the RFC 7009 revocation endpoint (when `revocation_endpoint` is configured); clears local tokens regardless of outcome |

## Resource Indicator and Issuer Validation

- **RFC 8707 `resource`**: resolved as "explicit `options.resource` → discovered metadata `resource` → `server_url`". When the resolved value is non-empty it is injected into the authorization URL and all three token requests (`client_credentials`, authorization-code exchange, `refresh_token`).
- **Audience binding**: token responses are persisted together with the current resource indicator (`TokenContainer.resource`, also persisted by `FileTokenCache`); once the indicator changes, `Authenticate` clears the mismatched cache first, and `GetAccessToken` / `IsAuthenticated` / `HasToken` / `GetAuthorizationHeader` all behave as if no valid token exists.
- **RFC 9207 `iss`**: on the authorization-response side the comparison runs after the `state` check and before the code is exchanged — a non-empty `AuthorizationCodeResult::iss` that differs from the discovered metadata issuer is rejected, while an absent `iss` is not validated. **On the token/refresh response side the `iss` is "validated when present"**: a string `iss` that differs from the issuer is rejected, a non-string `iss` is likewise rejected, and a missing `iss` passes. This follows RFC 9207 §2/§2.4 (and the MCP specification's Authorization Response Validation table), which bind obligations only to the `iss` of the **authorization response** — §2.4 explicitly allows clients to accept responses without `iss` per local policy; a token response that does carry `iss` is still validated strictly to prevent mix-up attacks.
- **Issuer binding**: `issuer` is a required field — authorization server implementations missing it should add it; the current implementation logs a warning on a missing issuer and continues down the fallback chain (direct RFC 8414 discovery first, then hardcoded `server_url` defaults), to be tightened into a hard failure in a future version. The first discovery binds the issuer, and a later issuer change clears dynamic registration info and local tokens before rebinding.
- **expectedResource comparison**: the client-side `VerifyResourceMatch` requires the discovered metadata `resource` to equal `server_url` exactly or match its scheme+host prefix (a missing/empty field passes); the server-side audience gate (active when `validate_token_resource` is true and `expected_resource` is non-empty) requires the token `resource` to be non-empty and equal to `expected_resource`, otherwise 401 `invalid_token`. Both sides share the same normalization (strip `#fragment`, strip trailing slash, see `NormalizeResourceUrl`), but the direction is asymmetric — the server rejects a missing audience while the client passes a missing `resource` field.
- **Client authentication methods**: the sending side supports `none`, `client_secret_post` (`client_secret` sent with the form) and `private_key_jwt` — when `private_key_pem` is configured and the discovered metadata's `token_endpoint_auth_methods_supported` contains `private_key_jwt`, all three token requests (`client_credentials`, authorization-code exchange, `refresh_token`) send `client_assertion_type=urn:ietf:params:oauth:client-assertion-type:jwt-bearer` plus an ES256-signed `client_assertion` instead of `client_secret` (and no longer fail merely because `client_secret` is absent). The assertion JWT header is `{alg:ES256,typ:JWT}` and its payload is `{iss,sub,aud,jti,iat,exp}` (`iss`/`sub` are the `client_id`, **`aud` prefers the metadata `issuer` and falls back to `token_endpoint` only when the issuer is absent** — RFC 7523 §3 defines the exact `aud` string as agreed out of band by the authorization server and the assertion issuer, with the token endpoint URL only a MAY, `exp=iat+60`). `jwt-bearer` (resource-owner assertion) remains unsupported, and `token_endpoint_auth_method` is still only adopted informationally (stored in the registration info).
- **CIMD (Client ID Metadata Document)**: only an `http(s)://`-shaped `client_id` is treated as a document URL — the client metadata document at that URL is fetched and parsed (adopting its `token_endpoint_auth_method`; documents with more than 128 `grant_types`/`redirect_uris` entries, a `client_id` field that does not match the requested URL, or a configured `redirect_uri` missing from the document's `redirect_uris` are refused). Failures are best-effort (a warning is logged and the configured values are used) and do not abort the authorization flow; dynamic registration sends `application_type` (default `"native"`).
- **Challenge hardening**: `HandleAuthChallenge` returns false on oversized/illegal input (never throws); illegal URLs and mismatched resources are both refused.

## Integration with Server-Side Bearer Auth

The SDK ships built-in RFC 6750/9728 bearer auth on the server side (`StreamableHttpServerOptions::bearer_auth`, see [Transports](/en/guide/transports)): failed verification returns a 401/403 challenge, the `WWW-Authenticate` header references the protected-resource metadata URL, and the metadata document is optionally served at `/.well-known/oauth-protected-resource`. When `expected_resource` is non-empty the token audience is additionally validated (mismatch → 401 `invalid_token`, split from the scope-gap 403).

The client has two integration points:

- `HttpClientTransportOptions::auth_challenge_handler` (see [Transports](/en/guide/transports)): always invoked on 401, invoked on 403 only when `WWW-Authenticate` contains `insufficient_scope`; returning a non-empty `Authorization` header retries the request exactly once. Inside the callback you can delegate to `OAuthClientProvider::HandleAuthChallenge(www_authenticate)` to parse the challenge and obtain a fresh token.
- `HandleAuthChallenge(www_authenticate)` (returns `bool`): parses the metadata URL from the challenge per RFC 9728 to discover the authorization server and re-runs the authorization flow when needed.

## DPoP Sending-Side Wiring

With `OAuthClientOptions::enable_dpop` on, bind `ApplyDPoPProofHeader`/`ObserveDPoPNonce` to the two `HttpClientTransportOptions` callbacks to drive a DPoP session:

```cpp
http_opts.apply_per_request_headers =
    [auth](const HttpRequestHeaderContext& ctx,
           std::map<std::string, std::string>& headers) {
        auth->ApplyDPoPProofHeader(ctx, headers);
    };
http_opts.observe_response_challenge =
    [auth](const HttpResponseChallengeContext& ctx) {
        auth->ObserveDPoPNonce(ctx.dpop_nonce);
    };
```

- `htu` is derived from `context.url` with query and fragment stripped before the proof is minted, `htm` comes from `context.http_method`; each call mints a fresh proof, so `jti`/`iat` are new per request.
- On 401/400 with `use_dpop_nonce` in `WWW-Authenticate` or the first 4KB of the response body, the request is retried exactly once (single budget shared with the `auth_challenge_handler` retry).

## PKCE Helpers

PKCE helpers reside in the `pkce` namespace within `<mcp/client/auth/OAuthClientProvider.hpp>`.

```cpp
auto verifier = pkce::GenerateCodeVerifier();
auto challenge = pkce::ComputeCodeChallenge(verifier);
auto encoded = pkce::Base64UrlEncode(input);
```

## Token Cache

The SDK provides two implementations of `ITokenCache` (defined in `<mcp/client/auth/TokenCache.hpp>`):

| Implementation | Persistence | Protection |
|----------------|-------------|------------|
| `InMemoryTokenCache` | Runtime only | None |
| `FileTokenCache` (`<mcp/storage/FileTokenCache.hpp>`) | JSON file (DPAPI encrypted on Windows) | `chmod 0600` on POSIX, `CryptProtectData` (DPAPI) on Windows |

```cpp
#include <mcp/storage/FileTokenCache.hpp>

auto token_cache = std::make_shared<FileTokenCache>("./tokens.json");
OAuthClientOptions oauth_opts;
oauth_opts.token_cache = token_cache;
```

## Requirements

OpenSSL development headers are an **optional** dependency (CMake uses `find_package(OpenSSL QUIET)`; when found, `MCP_HAVE_OPENSSL` is defined for `mcp-client`/`mcp-transport`, and `<openssl/rand.h>` is included under `#ifdef MCP_HAVE_OPENSSL` in `OAuthClientProvider.cpp`). PKCE code verifier generation uses `RAND_bytes` when built with OpenSSL, falling back to `BCryptGenRandom` (Windows) or `std::random_device` (POSIX), with SHA-256 from the built-in implementation. Without OpenSSL, TLS is disabled (`TlsSocket::Connect` throws `TlsHandshakeFailed`). Install OpenSSL (`vcpkg install openssl` / `apt install libssl-dev` / `brew install openssl`) to enable TLS (handled by the self-hosted network stack), cryptographic-grade randomness, and the ES256 ECDSA signing used by `private_key_jwt` client assertions and DPoP proofs; without OpenSSL EC support both the `private_key_jwt` assertion (when `private_key_pem` is configured) and DPoP proofs throw `McpError` (InternalError).
