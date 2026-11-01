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

## What This SDK Does Not Do

- **No host implementation** — no host type, no conversation orchestrator, no LLM call wrapper. Wiring clients into your application is host-side work.
- **No server registry discovery** — the connection object is constructed by the caller and passed in (`McpClient::Create(transport, options)`); the SDK never looks up which servers exist. The protocol's `server/discover` is a handshake method on a single connection, not service discovery.
- **No identity platform** — only bearer-token verification hooks, the OAuth client flow and optional DPoP. The identity provider, token issuance and policy storage live outside the SDK.
- **No orchestration beyond process lifetime** — the stdio transport spawns and reaps the child process, but there is no clustering, restart, rate-limit or routing layer.

## Further Reading

- [/en/learn/architecture](/en/learn/architecture) — library layering, façades and engines, threading model
- [/en/learn/transports](/en/learn/transports) — choosing among the five transports and stateful semantics
- [/en/learn/protocol-versions](/en/learn/protocol-versions) — the single authoritative dual-era matrix
- [/en/learn/extensions](/en/learn/extensions) — the extension switch family and explicit declaration
- [/en/reference/types](/en/reference/types) — types and field tables
