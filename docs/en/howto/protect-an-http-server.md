# Protect an HTTP server

Turn a `StreamableHttpServerTransport` from "runs on my machine" into "survives the public internet": Bearer verification, `WWW-Authenticate` challenges and the metadata endpoint, DPoP, Host/Origin allowlists, size limits, and consistency between `Mcp-Param-*` headers and the body. Protocol-level session behaviour is in [/en/howto/run-a-streamable-http-server](/en/howto/run-a-streamable-http-server); the client side of OAuth is [/en/howto/add-oauth-to-a-client](/en/howto/add-oauth-to-a-client).

## Prerequisites

- Headers: `mcp/transport/StreamableHttpServerTransport.hpp`, `mcp/http/SessionStore.hpp`, `mcp/http/EventStore.hpp`, `mcp/storage/FileEventStore.hpp`, `mcp/server/McpServer.hpp`, `mcp/server/ServerOptions.hpp`.
- Link `mcp-http` and `mcp-server` (dependency graph in [/en/start/installation](/en/start/installation)).
- HTTPS exists only when OpenSSL was enabled at configure time: with `MCP_WITH_OPENSSL=AUTO` and no OpenSSL found the whole server is **plaintext only**, so TLS must be terminated by a reverse proxy in front of it (`OFF` disables TLS outright).
- All protection lives in the transport and is era-independent; an absent `bearer_auth` means **no token check at all** — do not mistake "not configured" for "strictly enforced".

## Bearer verification: `verify` is the only mandatory piece

`StreamableHttpServerOptions::bearer_auth` is a `std::optional<BearerAuthConfig>` (`include/mcp/transport/StreamableHttpServerTransport.hpp:66-77`). `verify` receives the extracted token string and returns `AuthResult{ok, scopes, resource}` (`:31-35`). The remaining fields shape the challenge rather than the strength of validation: `required_scopes` feeds the 403 hint, `scopes_supported`/`authorization_servers`/`resource_metadata_url` feed the metadata endpoint, and `expected_resource` plus `validate_token_resource` (on by default) enforce audience binding.

<<< @/snippets/protect-an-http-server.cpp#bearer-auth

::: warning verify must reject expiry and forgery itself
The SDK never inspects the token: a false `ok` yields 401, nothing more. Signature checks, expiry, revocation all belong in your `verify` — treat it as the single trust boundary, and **do not make blocking remote calls inside it** (it runs on the request thread).
:::

## Challenge frames and the metadata endpoint

On missing or bad authorization the server answers `401` with `WWW-Authenticate` shaped like `Bearer resource_metadata="<url>"`, appending `error="invalid_token"` when the token was judged invalid and `scope="..."` when scope information exists; the body is a JSON-RPC error `-32000` + `unauthorized` (`src/http/StreamableHttpServerTransport.cpp:228-241`). Insufficient scope yields `403` + `Bearer error="insufficient_scope", scope="..."` with body `-32000` + `insufficient_scope` (`:243-252`).

With `serve_metadata_endpoint = true` (the default) the server publishes the protected-resource metadata endpoint itself for client discovery; turn it off and your gateway must serve the same URL, otherwise conformant clients stall during discovery.

## DPoP: bind the token to this connection

With `enable_dpop = true` the server accepts `Authorization: DPoP <token>` (scheme matching is case-insensitive, `src/http/StreamableHttpServerTransport.cpp:254-262`) and validates the proof that accompanies each request. `authorization_servers` declares which authorization servers issued the tokens, which matters when they differ from the resource origin.

<<< @/snippets/protect-an-http-server.cpp#dpop-server

The matching client shape (`apply_per_request_headers` + `observe_response_challenge`) is in [/en/howto/add-oauth-to-a-client](/en/howto/add-oauth-to-a-client) — **enabling DPoP on the server while the client omits those two hooks looks like endless 401s**, because the nonce is never obtained.

## Host and Origin: loopback-only by default

This check lives in the generic HTTP layer and gates every path (`src/http/HttpServerImpl.cpp:766-795`; a failure returns `403 Forbidden` and logs a Warning, `:470-475`):

| Situation | Behaviour |
|---|---|
| No `Host` header | rejected outright |
| `allowed_hosts` non-empty | the `Host` value must match **character for character** |
| `allowed_hosts` empty | only loopback-style hosts pass (fail-closed default) |
| No `Origin` header | allowed (non-browser clients are unaffected) |
| `Origin` present, allowlist non-empty | character-for-character match required, else rejected |
| `Origin` present, allowlist empty | must be same-origin with `Host` |

<<< @/snippets/protect-an-http-server.cpp#host-and-origin

::: danger equality includes the port
`Host` normally carries a port, so the allowlist needs `localhost:3010`, not `localhost`. Omitting the port does not "match loosely" — it produces **403 for everything**, the most common first-day outage.
:::

## Size and rate limits

<<< @/snippets/protect-an-http-server.cpp#size-limits

- Exceeding `max_request_body_bytes` answers `413` immediately (`src/http/StreamableHttpServerTransport.cpp:1091-1093`); the default is `kMaxHttpBodyBytes`, and setting it lower is what stops large payloads from exhausting memory.
- `max_sse_event_bytes` caps one SSE event; `sse_keep_alive_ms` (default 15000) sets the comment-frame heartbeat interval used to survive aggressive intermediary idle timeouts.

## Stateless versus stateful protection

<<< @/snippets/protect-an-http-server.cpp#stateless-mode

`stateless` defaults to `true`: no sessions are created, `session_store` is passed `nullptr`, and replay after a disconnect relies on `event_store` (`FileEventStore` persists to disk and survives processes). Only with `stateless = false` do you need `session_store` — and then **the session ID is a capability bearer**: once leaked, the peer can keep using that session **within the idle window** (the built-in constant `kSessionIdleTimeoutMs` expires a session after 30 inactive minutes, `include/mcp/transport/detail/Limits.hpp:24`, not configurable), so it must be combined with Bearer auth, and you should `Remove` the session explicitly on logout or revocation. `listen_hold_open` keeps the modern `GET` stream open so progress and logging are routed into the owning request stream (details in [/en/howto/report-progress](/en/howto/report-progress)).

## Header/body parameter consistency

`Mcp-Param-*` headers expose certain parameters in both header and body (useful for gateway routing and auditing). Consistency is validated server-side, and each mismatch shape has its own rejection reason (`src/http/StreamableHttpServerTransport.cpp:1039-1069`):

| Rejection reason | Meaning |
|---|---|
| `Mcp-Param-<n> header present but the parameter is absent from arguments` | header sent, parameter missing from the body |
| `missing Mcp-Param-<n> header for tools/call argument` | parameter present in the body but no header |
| `Mcp-Param-<n> header value is malformed` | header value could not be decoded |
| `Mcp-Param-<n> header value does not match the request body` | header and body disagree |

The annotations come from `x-mcp-header` inside your tool `input_schema`, resolved through the `resolve_param_annotations` callback (`include/mcp/transport/StreamableHttpServerTransport.hpp:79-81`). The callback needs the `McpServer` instance while the transport is created first — use the "declare a null pointer, assign it after the server exists" shape, and make sure nothing is served before that assignment:

<<< @/snippets/protect-an-http-server.cpp#param-headers

## Complete code

Bearer plus Host/Origin allowlists plus size limits plus parameter-header consistency plus the logging capability declaration:

<<< @/snippets/protect-an-http-server.cpp#full

## Verification

Snippets are syntax-checked only (clang-cl `/Zs`, GCC/Clang `-fsyntax-only`):

```bash
cmake -S . -B build/p3a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3a --target mcp-doc-snippet-protect-an-http-server
```

Expected: configure prints `[mcp-docs] syntax-checking 18 snippet(s) with '/Zs'`, the build prints one `Building CXX object ...protect-an-http-server.cpp.obj` line, return code 0 with no `error:` lines.

For behaviour, the cheapest reproduction is pointing the official referee at your server: `CONFORMANCE_SPEC_VERSION=2026-07-28 ./scripts/run-conformance.sh server` (the fixture needs `MCP_BUILD_CONFORMANCE=ON`). The pass criterion is an empty unexpected-failures list; entries already registered in the baseline are "known gaps", see `tests/conformance/`.

## Era differences

| Item | legacy (≤ 2025-11-25) | modern (2026-07-28) |
|---|---|---|
| Bearer / 401 / 403 challenges | identical | identical |
| Host/Origin checks | identical (generic HTTP layer) | identical |
| 413 and SSE event caps | identical | identical |
| Where notifications land | the separate SSE stream from `enable_legacy_sse` | with `listen_hold_open` they route per request stream, progress and logging in-band |
| Recovery after disconnect | relies on the global SSE stream | relies on `event_store` plus `Last-Event-ID` |

## Common mistakes

| Symptom | Cause | Fix |
|---|---|---|
| Every request gets 403 Forbidden, log says `Host not allowed` | `allowed_hosts` without the port | write exactly what the client sends in `Host` |
| curl works but the browser does not | `allowed_origins` non-empty and the frontend origin missing | add the frontend's scheme://host[:port], or leave it empty to use same-origin logic |
| Endless 401 with no nonce ever obtained | DPoP enabled server-side while the client skips `observe_response_challenge` | wire both ends, see [/en/howto/add-oauth-to-a-client](/en/howto/add-oauth-to-a-client) |
| A valid token is rejected | `expected_resource` disagrees with the token's `resource` | align the audience (RFC 8707), or disable `validate_token_resource` and own the risk |
| Metadata URL returns 404 | `serve_metadata_endpoint = false` and no gateway equivalent | keep the built-in endpoint, or have the gateway serve the same URL |
| Large bodies exhaust memory | `max_request_body_bytes` not set | tighten it explicitly and watch for 413 |
| SSE streams get cut after idle | `sse_keep_alive_ms` larger than the intermediary idle timeout | lower the heartbeat interval |
| Header/body mismatch slips through | `resolve_param_annotations` returns empty (server pointer not yet assigned) | only `Start()` once the callback reaches the real `McpServer` |
| Treating the session ID as a secret | stateful mode distinguishing callers by session alone | combine sessions with Bearer auth; the idle expiry is a fixed 30 minutes, so call `Remove` yourself when you need immediate revocation |

## Next steps

- Establishing server sessions and streams: [/en/howto/run-a-streamable-http-server](/en/howto/run-a-streamable-http-server)
- Obtaining tokens on the client: [/en/howto/add-oauth-to-a-client](/en/howto/add-oauth-to-a-client)
- Transport and port choice: [/en/learn/transports](/en/learn/transports)
- Full field tables: [/en/reference/transports](/en/reference/transports), [/en/reference/auth-and-extensions](/en/reference/auth-and-extensions)

## Source behind this page

- `include/mcp/transport/StreamableHttpServerTransport.hpp` (`StreamableHttpServerOptions`, `BearerAuthConfig`, `AuthResult`, `resolve_param_annotations`)
- `include/mcp/http/SessionStore.hpp`, `include/mcp/http/EventStore.hpp`, `include/mcp/storage/FileEventStore.hpp`
- `src/http/StreamableHttpServerTransport.cpp` (challenge frames, 413, parameter-header consistency, DPoP scheme)
- `src/http/HttpServerImpl.cpp` (Host/Origin admission and 403)
- `src/http/McpParamHeaders.hpp` (`Mcp-Param-*` encoding and decoding)
- `scripts/run-conformance.sh`, `tests/conformance/`
