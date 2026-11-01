# Add OAuth to a client

Make `StreamableHttpClientTransport` leave the house with a usable access token, and repair the situation when the server throws a `401`/`403` challenge: discover the authorization server metadata, run authorization code + PKCE or client credentials, store tokens in a cache you own, and step up scopes or swap in a DPoP proof when needed. This page covers the client side only; server-side Bearer validation lives in [/en/howto/protect-an-http-server](/en/howto/protect-an-http-server).

::: info Authentication is era-independent
OAuth, PKCE and DPoP all live in the HTTP transport layer and behave identically in both eras (legacy / modern); the era only changes which **protocol methods** run over that connection — see [/en/learn/protocol-versions](/en/learn/protocol-versions).
:::

## Prerequisites

- Headers: `mcp/client/auth/OAuthClientProvider.hpp`, `mcp/client/auth/TokenCache.hpp`, `mcp/transport/StreamableHttpClientTransport.hpp`, `mcp/client/ClientOptions.hpp`, `mcp/Implementation.hpp`; DPoP types come from `mcp/ext/DPoP.hpp` (pulled in by the provider header).
- The authentication code sits in the `mcp-client` and `mcp-ext` targets — link accordingly (library list in [/en/start/installation](/en/start/installation)).
- **TLS needs OpenSSL**: `-DMCP_WITH_OPENSSL=ON` fails configure outright when OpenSSL is missing; `AUTO` (the default) follows `find_package(OpenSSL)`, and without it the whole HTTPS path is unavailable (the PKCE entropy source falls back, see below).
- Endpoints must be `https`, loopback excepted; otherwise the relevant calls throw `McpError(InvalidParams)` with a message shaped like `OAuth <kind> endpoint must use https (loopback hosts excepted)` (`src/client/auth/OAuthClientProvider.cpp:151-157` and the `RequireSecureEndpoint` helper after it).

## Build the provider and pick a flow

`OAuthClientProvider` is an object **independent of the MCP client**: it obtains tokens, the transport uses them. The minimal configuration is three fields — `server_url`, `redirect_uri`, `token_cache` (`include/mcp/client/auth/OAuthClientProvider.hpp:28-47`).

<<< @/snippets/add-oauth-to-a-client.cpp#provider-options

The two main flows:

- **Authorization code + PKCE**: fill `authorization_redirect_handler` (open the browser yourself once you get the authorization URL) and `authorization_code_callback` (hand back the `code`/`state` from the redirect), then call `Authenticate()`.
- **Client credentials**: no browser involved — call `AuthenticateClientCredentials()` directly, set `application_type` to `"confidential"` and supply `client_secret` if required.

<<< @/snippets/add-oauth-to-a-client.cpp#client-credentials

Leaving `client_id` empty while the metadata advertises a `registration_endpoint` triggers **dynamic client registration** (`include/mcp/client/auth/TokenCache.hpp:72-84`); for `private_key_jwt`-style client assertions give `private_key_pem` plus `client_assertion_clock_seconds` (an injectable clock, handy in tests).

## Wire it into the transport

The transport exposes exactly three hooks (`include/mcp/transport/StreamableHttpClientTransport.hpp:50-55`):

| Hook | When | What you return or do |
|---|---|---|
| `auth_challenge_handler(www_authenticate)` | on `401`, or a `403` carrying `insufficient_scope` | return the **new `Authorization` header value**; an empty string means "give up, don't retry" |
| `apply_per_request_headers(context, headers)` | every outgoing request | add per-request headers such as the DPoP proof |
| `observe_response_challenge(context)` | every response | record new challenge values such as `DPoP-Nonce` |

<<< @/snippets/add-oauth-to-a-client.cpp#wire-transport

The retry policy is hard-coded and worth knowing line by line (`src/http/StreamableHttpClientTransport.cpp:1007-1042`):

- Authentication retry happens only on the **first attempt** — if the repaired request fails again the error goes straight back to the caller, with no loop.
- `401` is always eligible; a `403` is eligible only when `WWW-Authenticate` is an `insufficient_scope` challenge.
- An empty return value from the handler aborts the retry. Separately, `use_dpop_nonce`-style errors trigger their own retry, and a `400` whose body is a parameter-header mismatch first refreshes tool annotations and then retries (see [/en/howto/protect-an-http-server](/en/howto/protect-an-http-server)).

## Token retrieval and automatic renewal

`GetAccessToken()` does more than read the cache: when the token is present, **close to expiry, and a refresh token exists**, it first performs a coalesced refresh (only one refresh runs at a time, other callers wait) and then returns (`src/client/auth/OAuthClientProvider.cpp:987-996`). It returns an **empty string** in two cases: no token in the cache, or the token's `resource` does not match the `resource` you configured (RFC 8707 audience binding, `TokenResourceMatches`).

<<< @/snippets/add-oauth-to-a-client.cpp#step-up

- `RefreshTokens()` fails in two flavours: `invalid_grant`/`invalid_client` mean the credential was rejected (re-run the authorization flow); anything else is treated as transient.
- `StepUpAuthorization(scopes)` adds scopes after a 403; `scope_filter` lets you prune what the server advertises before the authorization URL is built.
- `Revoke()` best-effort calls the revocation endpoint and clears the cache.

## DPoP

Three options turn it on: `enable_dpop = true`, `private_key_pem` (an EC P-256 key), and `signing_algorithm` (`"ES256"` by default). After that `Authorization` carries `"DPoP <token>"` instead of `"Bearer ..."`, and **every request** needs a fresh proof:

<<< @/snippets/add-oauth-to-a-client.cpp#dpop

`ApplyDPoPProofHeader(context, headers)` builds `htm`/`htu` from `context.http_method`, `context.url` and `context.purpose`; `ObserveDPoPNonce(context.dpop_nonce)` stores the server-issued nonce back into the provider so the next request includes it. To craft a proof by hand (say you issue your own HTTP calls) use `BuildDPoPProof(htm, htu, access_token)`.

## Don't point at the wrong authorization server

`allowed_authorization_server_origins` is an allowlist: the origin of the metadata's `issuer` (falling back to `token_endpoint` when empty) must equal the origin of `server_url`, or appear in the allowlist, otherwise it is rejected (`IsAllowedAuthorizationServer` in `src/client/auth/OAuthClientProvider.cpp`). Use `IsSameOrigin` for origin comparison and `IsSecureEndpoint` for the scheme check (loopback excepted).

<<< @/snippets/add-oauth-to-a-client.cpp#endpoint-hygiene

A missing `iss` in the token response only logs **a Warning** and is accepted per local policy (RFC 9207 makes it optional); to enforce it, validate yourself or tighten the allowlist.

## The cache is yours to implement

`ITokenCache` has four required methods plus two optional ones (persisting the bound issuer, so the same issuer can be reused across processes), and `InMemoryTokenCache` is provided as the default (`include/mcp/client/auth/TokenCache.hpp:23-45`). `TokenContainer` holds `access_token`, `refresh_token`, `scopes`, `expires_at` (an **absolute second count**, not seconds remaining), `token_type`, `resource`, plus `IsExpired()` / `WillExpireSoon(margin_ms = 60000)`.

::: warning Never let a token reach the logs
`access_token`, `refresh_token`, `client_secret` and `private_key_pem` are all secrets: do not print them even while debugging with `MCP_LOG_LEVEL=4`, and choose the persistence location of your `ITokenCache` implementation according to your project's key-management rules.
:::

## Complete code

Assembling a client that authenticates at startup and renews on challenge:

<<< @/snippets/add-oauth-to-a-client.cpp#full

## Verification

Snippets are syntax-checked only (clang-cl `/Zs`, GCC/Clang `-fsyntax-only`); no process, no network:

```bash
cmake -S . -B build/p3a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3a --target mcp-doc-snippet-add-oauth-to-a-client
```

Expected: configure prints `[mcp-docs] syntax-checking 15 snippet(s) with '/Zs'`, and the build prints

```text
[1/4] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-add-oauth-to-a-client.dir\__\__\docs\snippets\add-oauth-to-a-client.cpp.obj
```

Return code 0 with no `error:` lines. For end-to-end behaviour run the shipped conformance client (needs `-DMCP_BUILD_CONFORMANCE=ON`), which exercises both Bearer and DPoP chains: `examples/conformance/client/main.cpp:1265-1281` (Bearer) and `:1283-1307` (DPoP), with `scripts/run-conformance.sh`.

## Era differences

Authentication is not era-gated, but the protocol method set on the connection is:

| Item | legacy (≤ 2025-11-25) | modern (2026-07-28) |
|---|---|---|
| OAuth / PKCE / DPoP behaviour | identical (transport layer) | identical (transport layer) |
| 401/403 retry | identical, and only on the first attempt | same |
| Interaction with `Mcp-Param-*` headers | not applicable | a `400` header mismatch triggers the tool-annotation refresh retry, independent of the auth retry |
| Session header | `mcp-session-id` required | same; an authentication failure and its reconnect do not affect session recovery |

## Common mistakes

| Symptom | Cause | Fix |
|---|---|---|
| `OAuth <kind> endpoint must use https (loopback hosts excepted)` | plaintext http endpoint that is not loopback | switch to https; use `localhost` for local integration |
| Configure fails saying OpenSSL was not found | explicit `-DMCP_WITH_OPENSSL=ON` without OpenSSL installed | install OpenSSL 3.x/4.x, or go back to `AUTO`/`OFF` (`OFF` disables TLS, so every HTTPS endpoint becomes unavailable) |
| `GetAccessToken()` returns an empty string | no cached token, or `resource` does not match the issued audience | call `Authenticate()` first; `resource` must equal the one used at issuance (RFC 8707) |
| A 403 produces no retry at all | `WWW-Authenticate` lacks `insufficient_scope` | this non-retry is deliberate per spec; call `StepUpAuthorization` yourself when you need more scope |
| Repeated 401s hammering the authorization server | assuming auth retries are multi-round | only the first attempt is repaired; if it still fails you are back at the caller — check scope and audience |
| DPoP requests keep being rejected with a nonce error | `observe_response_challenge` not wired | connect all three hooks; the provider remembers the nonce |
| No complaint about a mismatched `iss` | a missing `iss` only logs a Warning (optional per RFC 9207) | tighten `allowed_authorization_server_origins`, or validate yourself |
| Re-authorisation on every process restart | `InMemoryTokenCache` in use | implement and persist your own `ITokenCache`, and implement `StoreBoundIssuer` too |

## Next steps

- Server-side Bearer and DPoP validation: [/en/howto/protect-an-http-server](/en/howto/protect-an-http-server)
- Transport and endpoint choice: [/en/learn/transports](/en/learn/transports), [/en/reference/transports](/en/reference/transports)
- Reading failures and error codes: [/en/reference/errors](/en/reference/errors)
- Connecting and when to close: [/en/howto/connect-to-a-server](/en/howto/connect-to-a-server)

## Source behind this page

- `include/mcp/client/auth/OAuthClientProvider.hpp` (`OAuthClientOptions`, public methods, `pkce::*`)
- `include/mcp/client/auth/TokenCache.hpp` (`TokenContainer`, `ITokenCache`, `OAuthMetadata`, `ClientRegistrationInfo`)
- `include/mcp/ext/DPoP.hpp` (EC P-256 JWK)
- `include/mcp/transport/StreamableHttpClientTransport.hpp` (the three auth hooks, `HttpRequestHeaderContext`, `HttpResponseChallengeContext`)
- `src/client/auth/OAuthClientProvider.cpp` (endpoint validation, metadata discovery, PKCE entropy, coalesced refresh, audience binding)
- `src/http/StreamableHttpClientTransport.cpp` (the 401/403 and nonce retry paths)
- `examples/conformance/client/main.cpp` (real Bearer and DPoP assembly)
