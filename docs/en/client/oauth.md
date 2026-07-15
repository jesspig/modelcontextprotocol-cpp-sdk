# OAuth Support

The client supports the MCP OAuth authorization flow for servers that require authentication.

## Flow

1. **Authorization Code + PKCE** with S256 code challenge
2. **Dynamic Client Registration** (DCR) for first-time clients
3. **Token refresh** with automatic retry on 401 responses
4. **Token revocation** on client shutdown

## Setup

```cpp
auto oauth = std::make_shared<OAuthClientProvider>(
    "https://auth.server.com/.well-known/oauth-authorization-server",
    "client-id");

client->SetOAuthProvider(oauth);
```

## Token Cache

The SDK provides two implementations of `ITokenCache`:

| Implementation | Persistence |
|----------------|-------------|
| `InMemoryTokenCache` | Runtime only |
| `FileTokenCache` | JSON file on disk |

```cpp
auto token_cache = std::make_shared<FileTokenCache>("./tokens.json");
OAuthClientOptions oauth_opts;
oauth_opts.token_cache = token_cache;
```

## Requirements

OAuth PKCE requires OpenSSL. Enable via:

```bash
cmake --preset debug -DMCP_HAVE_OPENSSL=ON
```
