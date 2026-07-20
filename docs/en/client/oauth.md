# OAuth Support

The client supports the MCP OAuth authorization flow for servers that require authentication.

## Flow

1. **Authorization Code + PKCE** with S256 code challenge
2. **Dynamic Client Registration** (DCR) for first-time clients (synthetic, no wire call)
3. **Token refresh** via preemptive expiry check (not 401-driven)
4. **Token revocation** via manual `Revoke()` call

## Setup

```cpp
OAuthClientOptions oauth_opts;
oauth_opts.server_url = "https://auth.server.com";
oauth_opts.redirect_uri = "http://localhost:3000/callback";
oauth_opts.client_id = "my-client";
oauth_opts.authorization_redirect_handler =
    [](std::string_view url) {
        // Open URL in browser for user to authorize
    };
oauth_opts.authorization_code_callback =
    []() -> std::optional<std::string> {
        // Return authorization code from redirect
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
| `GetAccessToken()` | Returns valid access token, auto-refreshes if expiring soon |
| `RefreshTokens()` | Force-refresh using stored refresh token |
| `IsAuthenticated()` | Check if stored token is present and not expired |
| `HasToken()` | Check if any token exists (may be expired) |
| `GetAuthorizationHeader()` | Returns `"Bearer {token}"` string |
| `StepUpAuthorization(scopes)` | Re-authorize with additional scopes |
| `Revoke()` | Clear stored tokens |

## PKCE Helpers

```cpp
auto verifier = pkce::GenerateCodeVerifier();
auto challenge = pkce::ComputeCodeChallenge(verifier);
auto encoded = pkce::Base64UrlEncode(input);
```

## Token Cache

The SDK provides two implementations of `ITokenCache`:

| Implementation | Persistence | Encryption |
|----------------|-------------|------------|
| `InMemoryTokenCache` | Runtime only | None |
| `FileTokenCache` | JSON file (POSIX) / DPAPI-encrypted (Windows) | `chmod 0600` on POSIX, `CryptProtectData` on Windows |

```cpp
auto token_cache = std::make_shared<FileTokenCache>("./tokens.json");
OAuthClientOptions oauth_opts;
oauth_opts.token_cache = token_cache;
```

## Requirements

OAuth PKCE falls back to `std::random_device` when OpenSSL is absent. With OpenSSL, it uses `RAND_bytes` for the code verifier. Install OpenSSL (`vcpkg install openssl` / `apt install libssl-dev` / `brew install openssl`) for TLS-secured token exchange.
