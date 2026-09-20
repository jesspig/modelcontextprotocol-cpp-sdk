# OAuth Support

The client supports the MCP OAuth authorization flow for servers that require authentication.

## Flow

1. **Authorization Code + PKCE** with S256 code challenge
2. **Client identity**: when `client_id` is a URL, the Client ID Metadata Document (CIMD) is fetched and parsed; **Dynamic Client Registration** (DCR, HTTP POST to the registration endpoint) runs only when no `client_id` is configured
3. **Resource indicator**: the authorization request and all token requests carry a `resource` parameter per RFC 8707
4. **Token refresh** via preemptive expiry check (not 401-driven)
5. **Token revocation** via manual `Revoke()` call (best-effort call to the RFC 7009 revocation endpoint; failure does not affect local token clearing)

## OAuthClientOptions

| Field | Type | Description |
|-------|------|-------------|
| `server_url` | `string` | Authorization server base URL |
| `redirect_uri` | `string` | OAuth redirect URI |
| `client_id` | `optional<string>` | Client identifier (auto-registered if absent) |
| `client_secret` | `optional<string>` | Client secret (optional) |
| `scopes` | `vector<string>` | Requested OAuth scopes |
| `token_cache` | `shared_ptr<ITokenCache>` | Token persistence (default: `InMemoryTokenCache`) |
| `resource` | `optional<string>` | RFC 8707 resource indicator sent with the authorization request and token requests; falls back to the discovered metadata `resource`, then to `server_url`; when the resolved value is empty the parameter is omitted entirely |
| `authorization_redirect_handler` | `function<void(string_view url)>` | Callback to open the authorization URL |
| `authorization_code_callback` | `function<optional<AuthorizationCodeResult>()>` | Callback returning the authorization code, the echoed `state`, and the optional `iss` (`AuthorizationCodeResult{code, state, iss}`), `nullopt` on failure |

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
| `Authenticate()` | Full OAuth flow: discover → register → authorize → token exchange |
| `GetAccessToken()` | Returns valid access token, auto-refreshes if expiring soon; throws `McpError` (InternalError) on refresh failure |
| `RefreshTokens()` | Force-refresh using stored refresh token |
| `IsAuthenticated()` | Check if stored token is present and not expired |
| `HasToken()` | Check if any token exists (may be expired) |
| `GetAuthorizationHeader()` | Returns `"Bearer {token}"` string |
| `StepUpAuthorization(scopes)` | Re-authorize with additional scopes |
| `AuthenticateClientCredentials()` | Client credentials grant (RFC 6749 §4.4) for service-to-service scenarios without user interaction |
| `HandleAuthChallenge(www_authenticate)` | Handles a 401/403 authentication challenge header (RFC 9728); retries the original request on success |
| `Revoke()` | Best-effort call to the RFC 7009 revocation endpoint (when `revocation_endpoint` is configured); clears local tokens regardless of outcome |

## Resource Indicator and Issuer Validation

- **RFC 8707 `resource`**: resolved as "explicit `options.resource` → discovered metadata `resource` → `server_url`". When the resolved value is non-empty it is injected into the authorization URL and all three token requests (`client_credentials`, authorization-code exchange, `refresh_token`).
- **RFC 9207 `iss`**: the `iss` in token and refresh responses must be present and match the discovered metadata issuer, otherwise the response is rejected. On the authorization-response side the same comparison runs after the `state` check and before the code is exchanged: a non-empty `AuthorizationCodeResult::iss` that differs from the issuer is rejected, while an absent `iss` is not validated.
- **CIMD (Client ID Metadata Document)**: when `client_id` is a URL, the client metadata document at that URL is fetched and parsed. Failures are best-effort (a warning is logged and the configured values are used) and do not abort the authorization flow.

## Integration with Server-Side Bearer Auth

The SDK ships built-in RFC 6750/9728 bearer auth on the server side (`StreamableHttpServerOptions::bearer_auth`, see [Transports](/en/guide/transports)): failed verification returns a 401/403 challenge, the `WWW-Authenticate` header references the protected-resource metadata URL, and the metadata document is optionally served at `/.well-known/oauth-protected-resource`.

The client has two integration points:

- `HttpClientTransportOptions::auth_challenge_handler` (see [Transports](/en/guide/transports)): invoked with the `WWW-Authenticate` header on 401/403; returning a non-empty `Authorization` header retries the request exactly once. Inside the callback you can delegate to `OAuthClientProvider::HandleAuthChallenge(www_authenticate)` to parse the challenge and obtain a fresh token.
- `HandleAuthChallenge(www_authenticate)` (returns `bool`): parses the metadata URL from the challenge per RFC 9728 to discover the authorization server and re-runs the authorization flow when needed.

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

OpenSSL development headers are an **optional** dependency (CMake uses `find_package(OpenSSL QUIET)`; when found, `MCP_HAVE_OPENSSL` is defined for `mcp-client`/`mcp-transport`, and `<openssl/rand.h>` is included under `#ifdef MCP_HAVE_OPENSSL` in `OAuthClientProvider.cpp`). PKCE code verifier generation uses `RAND_bytes` when built with OpenSSL, falling back to `BCryptGenRandom` (Windows) or `std::random_device` (POSIX), with SHA-256 from the built-in implementation. Without OpenSSL, TLS is disabled (`TlsSocket::Connect` throws `TlsHandshakeFailed`). Install OpenSSL (`vcpkg install openssl` / `apt install libssl-dev` / `brew install openssl`) to enable TLS (handled by the self-hosted network stack) and cryptographic-grade randomness.
