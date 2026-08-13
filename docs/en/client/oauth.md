# OAuth Support

The client supports the MCP OAuth authorization flow for servers that require authentication.

## Flow

1. **Authorization Code + PKCE** with S256 code challenge
2. **Dynamic Client Registration** (DCR) for first-time clients (HTTP POST to registration endpoint)
3. **Token refresh** via preemptive expiry check (not 401-driven)
4. **Token revocation** via manual `Revoke()` call (best-effort call to the RFC 7009 revocation endpoint; failure does not affect local token clearing)

## OAuthClientOptions

| Field | Type | Description |
|-------|------|-------------|
| `server_url` | `string` | Authorization server base URL |
| `redirect_uri` | `string` | OAuth redirect URI |
| `client_id` | `optional<string>` | Client identifier (auto-registered if absent) |
| `client_secret` | `optional<string>` | Client secret (optional) |
| `scopes` | `vector<string>` | Requested OAuth scopes |
| `token_cache` | `shared_ptr<ITokenCache>` | Token persistence (default: `InMemoryTokenCache`) |
| `authorization_redirect_handler` | `function<void(string_view url)>` | Callback to open the authorization URL |
| `authorization_code_callback` | `function<optional<AuthorizationCodeResult>()>` | Callback returning the authorization code plus the echoed `state` (`AuthorizationCodeResult{code, state}`), `nullopt` on failure |

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
        // Return the authorization code plus the `state` echoed back
        // by the authorization server (CSRF protection)
        return AuthorizationCodeResult{"auth-code", "state"};
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

OpenSSL development headers are a **hard requirement** for building `mcp-client` (`OAuthClientProvider.cpp` unconditionally includes `<openssl/rand.h>`; CMake locates OpenSSL via `find_package(OpenSSL QUIET)` — if not found, `MCP_HAVE_OPENSSL` is undefined, but compilation still fails for lack of headers). PKCE code verifier generation uses `RAND_bytes` when built with OpenSSL (`MCP_HAVE_OPENSSL`), falling back to `std::random_device`. Install OpenSSL (`vcpkg install openssl` / `apt install libssl-dev` / `brew install openssl`) for TLS (handled by libhv's HTTP client) and cryptographic-grade randomness.
