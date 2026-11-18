# What is MCP

The Model Context Protocol is an open standard that specifies how LLM applications discover and invoke external data sources and tools. It defines message shapes and method semantics only: it does not pin an implementation language, does not say how a host assembles context, and does not prescribe how a server stores data.

Pages at this layer answer "what is this, why is it designed this way, where does this SDK draw the line". How to write the code belongs to the reference and how-to layers — start at [/en/reference/index](/en/reference/index).

## The Three Roles

| Role | Responsibility | Provided by this SDK |
|------|----------------|----------------------|
| Host | Runs the LLM, owns the user interface, decides whether a capability gets invoked | No |
| Client | Maintains exactly one connection to one server and relays messages in both directions | Yes: `McpClient` |
| Server | Exposes capabilities and answers requests from the peer | Yes: `McpServer` |

A host typically holds several clients at once, each connected to a different server. The client is an internal connector inside the host, not the user-facing application — this is the most common point of confusion when first meeting the protocol.

## The Message Layer

Every exchange between client and server is a JSON-RPC 2.0 message, expressed by four structs in `include/mcp/JsonRpc.hpp`: request (carries `id`), notification (no `id`), success response, error response. Protocol method names, error codes and capability declarations are all built on those four envelopes, so any extension method the SDK does not model can still be sent verbatim through `SendRequest`.

## Capability Support Matrix

| Capability | Introduced by | Current availability | Authoritative page |
|------------|---------------|----------------------|--------------------|
| Tools | Server | Full — registering a tool declares the capability | [/en/reference/server](/en/reference/server) |
| Resources and resource templates | Server | Full — registering a resource declares the capability | [/en/reference/server](/en/reference/server) |
| Prompts | Server | Full — registering a prompt declares the capability | [/en/reference/server](/en/reference/server) |
| Completion | Server | Requires a separate completion callback; without one the entry point is rejected as "method not found" | [/en/reference/server](/en/reference/server) |
| Skills | Server | Requires an explicit switch and is available on the modern protocol era only | [/en/reference/auth-and-extensions](/en/reference/auth-and-extensions) |
| Tasks | Server | Requires an explicit switch plus a task store; available in both eras | [/en/reference/auth-and-extensions](/en/reference/auth-and-extensions) |
| Elicitation | Server asks, client answers | Full — form and URL modes | [/en/reference/client](/en/reference/client) |
| Sampling | Server asks, client answers | Callable but deprecated (SEP-2577); new code should use elicitation | [/en/reference/client](/en/reference/client) |
| Roots | Client answers | Callable but deprecated (SEP-2577) | [/en/reference/client](/en/reference/client) |

The deprecation for sampling and roots sits on the client-side setters (`McpClient::SetSamplingHandler` and `McpClient::SetRootsHandler` carry `[[deprecated]]`); the option fields and the legacy-era wire behaviour are still present. **Nothing was removed** — on a legacy-era connection they still work. In the modern era `sampling/createMessage` no longer exists as a standalone request and can only surface as an input request inside a multi-round-trip interaction, and the server first checks that the client declared the sampling capability. See [/en/learn/protocol-versions](/en/learn/protocol-versions).

## Two Protocol Eras

This SDK supports five protocol versions spanning two eras: the legacy era handshakes with `initialize` and lets the server send reverse requests directly, while the modern era (from `2026-07-28`) uses `server/discover`, per-request `_meta`, multi-round-trip interactions and subscription streams. Any statement about capability availability is meaningless without naming the era. The version matrix is maintained in exactly one place: [/en/learn/protocol-versions](/en/learn/protocol-versions).

## How This SDK Positions Itself

- **Both ends** — `McpServer` and `McpClient` are peer façades; there is no "server-only" bias.
- **All static libraries** — no shared objects, no plugin mechanism, no DLLs to ship alongside. How you link is how you integrate; see [/en/learn/architecture](/en/learn/architecture).
- **Own network stack** — TCP/TLS/HTTP/WebSocket are first-party implementations, there is no asio dependency. OpenSSL is the only optional system-level dependency: without it only TLS is disabled, everything else stays functional.

## SDK Tier Comparison

This SDK was developed against the official MCP specification and reference implementations:

| Language | Repository | Tier |
|----------|------------|------|
| Python | [modelcontextprotocol/python-sdk](https://github.com/modelcontextprotocol/python-sdk) | Tier 1 |
| TypeScript | [modelcontextprotocol/typescript-sdk](https://github.com/modelcontextprotocol/typescript-sdk) | Tier 1 |
| Go | [modelcontextprotocol/go-sdk](https://github.com/modelcontextprotocol/go-sdk) | Tier 1 |
| C# | [modelcontextprotocol/csharp-sdk](https://github.com/modelcontextprotocol/csharp-sdk) | Tier 1 |
| Rust | [modelcontextprotocol/rust-sdk](https://github.com/modelcontextprotocol/rust-sdk) | Tier 1 |
| Ruby | [modelcontextprotocol/ruby-sdk](https://github.com/modelcontextprotocol/ruby-sdk) | Tier 1 |
| Java | [modelcontextprotocol/java-sdk](https://github.com/modelcontextprotocol/java-sdk) | Tier 2 |
| Swift | [modelcontextprotocol/swift-sdk](https://github.com/modelcontextprotocol/swift-sdk) | Tier 3 |
| PHP | [modelcontextprotocol/php-sdk](https://github.com/modelcontextprotocol/php-sdk) | Tier 3 |
| Kotlin | [modelcontextprotocol/kotlin-sdk](https://github.com/modelcontextprotocol/kotlin-sdk) | Tier 3 |

Tiers follow the [official SDK list (2026-07-28 revision)](https://modelcontextprotocol.io/docs/2026-07-28/sdk): 6 Tier 1 (TypeScript, Python, C#, Go, Rust, Ruby — Rust promoted to Tier 1 via [PR #3287](https://github.com/modelcontextprotocol/modelcontextprotocol/pull/3287) on 2026-08-21), Java is Tier 2, Swift/PHP/Kotlin are Tier 3. Tier rules: [SDK Tiering System](https://modelcontextprotocol.io/community/sdk-tiers) — experimental features and protocol extensions (such as Tasks and MCP Apps) are not required for any tier.

### Ruby SDK Cross-Check

Implementation notes on the official Ruby SDK (`mcp` gem, `main` branch as of 2026-10-11; latest release `1.7.0` dated 2026-10-05, see [CHANGELOG.md](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/CHANGELOG.md) and [version.rb](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/lib/mcp/version.rb)), compared with this SDK per dimension:

| Dimension | Ruby (`mcp` gem) | This SDK |
|-----------|------------------|----------|
| Spec revisions | 2025-06-18, 2025-11-25, 2026-07-28 ([ROADMAP.md](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/ROADMAP.md), Current Status) | 2024-11-05…2026-07-28, dual-era codec (see [Protocol versions](/en/learn/protocol-versions)) |
| `server/discover` | Modern era never negotiated via `initialize` (CHANGELOG §1.2.0); `server/discover` finalized with modern client support; per-transport era lock on stdio, sessionless modern path on Streamable HTTP ([methods.rb](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/lib/mcp/methods.rb)) | Auto mode with transport-classified fallback ([version negotiation](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/wiki/concepts/version-negotiation.md)) |
| MRTR | SEP-2322 `input_required` results with `inputRequests`/`inputResponses` + `requestState`, fulfilled on the legacy wire too (CHANGELOG §0.24.0/§1.2.0); `resultType` is only `complete`/`input_required` ([result_type.rb](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/lib/mcp/result_type.rb)) | `InputRequiredResult` + elicitation loop ([MRTR](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/wiki/concepts/mrtr.md)); additional `task` resultType for task handles |
| Headers | Sends SEP-2243 `Mcp-Method`/`Mcp-Name`, mirrors `x-mcp-header` params into `Mcp-Param-*` (CHANGELOG §0.22.0/§1.2.0; [mcp_param_headers.rb](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/lib/mcp/client/mcp_param_headers.rb)); `Mcp-Method` required on the modern path | `Mcp-Name` validation incl. tasks `taskId` check ([MCP param headers](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/wiki/concepts/mcp-param-headers.md), [tasks extension](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/wiki/concepts/tasks-extension.md)) |
| Cache | Server stamps SEP-2549 `ttlMs`/`cacheScope` (CHANGELOG §0.24.0/§1.2.0); no client-side response cache: `lib/mcp/client/` holds only elicitation, http, mcp_param_headers, modern_envelope, oauth(+oauth/), paginated_result, stdio, tool, and `client/http.rb` matches no `ttlMs`/`cacheScope`/cache-hit logic (verified 2026-10-13 against `main`) | Client response cache consuming both the 2026 top-level `ttlMs`/`cacheScope` and the 2025 nested `cacheHint`, with TTL + public/private partitions ([response cache](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/wiki/concepts/response-cache.md)) |
| Auth | Full OAuth 2.1 client (DCR, PKCE, CIMD, `client_credentials`, `private_key_jwt`, `jwt-bearer`, RFC 9207 `iss`, PRM-first discovery; CHANGELOG §0.17.0–§1.6.0) + server resource-server protection ([README Features](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/README.md)) | PKCE/CIMD (DCR fallback) + `FileTokenCache`, server Bearer auth, opt-in server-side DPoP proof verification ([DPoP](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/wiki/concepts/dpop.md); see [Discover an authorization server](/en/howto/add-oauth-to-a-client)) |
| Tasks (SEP-2663) | Not implemented — "remains planned" ([ROADMAP.md](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/ROADMAP.md), [#391](https://github.com/modelcontextprotocol/ruby-sdk/issues/391)) | `tasks/get\|update\|cancel` dual-era + `FileTaskStore` ([tasks extension](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/wiki/concepts/tasks-extension.md)) |
| Skills (SEP-2640) | Not implemented【assumption: no skills entry in the `methods.rb` method set, `lib/mcp/`, `lib/mcp/client/`, `test/mcp/`; no skills record in any CHANGELOG version】 | `skills/list\|get` + `resources/directory/read`, 2026-only ([skills extension](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/wiki/concepts/skills-extension.md)) |

## What This SDK Does Not Do

- **No host implementation** — no host type, no conversation orchestrator, no LLM call wrapper. Wiring clients into your application is host-side work.
- **No server registry discovery** — the connection object is constructed by the caller and passed in (`McpClient::Create(transport, options)`); the SDK never looks up which servers exist. The protocol's `server/discover` is a handshake method on a single connection, not service discovery.
- **No identity platform** — only bearer-token verification hooks, the OAuth client flow and optional DPoP. The identity provider, token issuance and policy storage live outside the SDK.
- **No orchestration beyond process lifetime** — the stdio transport spawns and reaps the child process, but there is no clustering, restart, rate-limit or routing layer.

### Unsupported Protocol Features

The following items are explicitly declared unsupported; when a peer sends them, the SDK degrades gracefully (ignores the unknown field or falls back to the baseline path) instead of failing the session.

- **Apps UI rendering** — server-pushed UI components are not rendered and pass through as opaque data only.
- **Full sampling** — sampling is legacy partial only: the deprecated `sampling_handler` covers basic form and single-content sampling, and the modern 2026 era is not treated as a complete sampling capability; full sampling is out of scope for this round and is not counted as a Tier 1 parity closure item.
- **EMA** — EMA negotiation is not implemented and peers fall back to baseline capability negotiation.
- **DPoP client-side per-request proofs** — server-side proof verification is implemented (off by default, experimental `enable_dpop`, requires OpenSSL; see [Add OAuth](/en/howto/add-oauth-to-a-client)); transports attach a per-request proof only when `apply_per_request_headers`/`observe_response_challenge` are bound via `ApplyDPoPProofHeader`/`ObserveDPoPNonce`, and configured callbacks drive a DPoP session with a single `use_dpop_nonce` retry; with the switch off the plain Bearer path is unchanged.
- **jwt-bearer** — `private_key_jwt` client authentication is supported (an ES256 client assertion is sent on token requests when `private_key_pem` is configured and the authorization server advertises `private_key_jwt` in `token_endpoint_auth_methods_supported`; requires OpenSSL), while the authorization-owner `jwt-bearer` grant is not, and the public-client PKCE/CIMD flow remains the supported path.
- **File Uploads** — resumable file-upload primitives are not implemented, and file content moves through existing tool/resource payloads only.

## Further Reading

- [/en/learn/architecture](/en/learn/architecture) — library layering, façades and engines, threading model
- [/en/learn/transports](/en/learn/transports) — choosing among the five transports and stateful semantics
- [/en/learn/protocol-versions](/en/learn/protocol-versions) — the single authoritative dual-era matrix
- [/en/learn/extensions](/en/learn/extensions) — the extension switch family and explicit declaration
- [/en/reference/types](/en/reference/types) — types and field tables
- [/en/howto/register-a-tool](/en/howto/register-a-tool) — turning the tool primitive into running code
- [/en/howto/serve-a-resource](/en/howto/serve-a-resource) — exposing resources and templates
