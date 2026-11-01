# Transports

A transport only moves JSON-RPC envelopes to the peer; it never arbitrates protocol semantics — every era decision happens in the codec layer, see [/en/learn/architecture](/en/learn/architecture). This page covers selection, where the stateful/stateless line falls, where session ids come from, and how the transport category changes version-negotiation fallback. Field tables live in [/en/reference/transports](/en/reference/transports).

## The Five Transports

| Transport | Client | Server | Carriage | Fits |
|-----------|--------|--------|----------|------|
| stdio | `StdioClientTransport` | `StdioServerTransport` | pipes on a child process's standard input/output | local one-to-one, host launches the server |
| Streamable HTTP | `StreamableHttpClientTransport` | `StreamableHttpServerTransport` | HTTP POST for messages, optional GET stream for notifications and streamed results | the default choice for remote deployment |
| SSE | `SseClientTransport` | no dedicated server class | one long-lived GET push plus POST upstream | talking to servers that still expose the legacy SSE endpoint |
| WebSocket | `WebSocketClientTransport` | none | bidirectional frames over TCP | when the peer only offers WebSocket |
| InMemory | both halves of `InMemoryTransport::CreatePair()` | same | two in-process queues wired together | unit tests and same-process assembly |

Two facts that must be stated plainly: **SSE and WebSocket have client implementations only**. The server-side SSE stream is served by `StreamableHttpServerTransport` on the same endpoint when `enable_legacy_sse` is turned on (off by default; while off no GET route is registered at all, so the listen stream and event replay are unavailable). There is no standalone "SSE server class". Treating InMemory as a network substitute, or expecting to connect to this SDK's server over WebSocket, fails at the very first step because the class does not exist.

## How to Choose

- Server and host share a machine and the host launches the process: stdio. No port, no authentication surface, lifetime ends with the process.
- The server is deployed independently, serves many clients, or crosses a network: Streamable HTTP. The other three are compatibility paths for when the peer already has a fixed shape.
- Tests that should skip serialisation and networking: InMemory — it still runs the full engine and codec path, so protocol behaviour is genuinely exercised.

## Stateful and Stateless

The dividing line is whether the server keeps session state across requests.

- **Stateful**: the server stores the negotiated version, peer capabilities, initialization status and subscription table per session; later requests must carry the same session identifier or they are judged expired.
- **Stateless**: each request is handled in isolation and the server retains nothing; the request carries all of its own context (the modern era does this with per-request `_meta` holding version and client info, see [/en/learn/protocol-versions](/en/learn/protocol-versions)).

The Streamable HTTP server is stateless by default (`StreamableHttpServerOptions::stateless` defaults to `true`); the HTTP client is stateful by default (`HttpClientTransportOptions::stateless` defaults to `false`), and the two must agree or the client keeps sending a session header the server never recognises. stdio, SSE, WebSocket and InMemory have no notion of a mode: one connection is one session, which is equivalent to stateful.

## Where the Session ID Comes From

The HTTP header `mcp-session-id` is the only source of a session id:

- In stateful mode the server generates an id on a successful `initialize`, stores it in the session store and echoes it in the response header; later requests route by that header.
- When the header is absent and the request is not `initialize`, the session id is the empty string — empty means "no session context".
- An unknown or expired session returns 404 with error code `-32009` (SessionExpired).
- Stateless mode skips all resolution: the session id is always the empty string.
- The resolved value is written into the message's own session field, not into the transport object's `SessionId()`; non-HTTP transports never resolve a session header, so both are empty. The server folds an empty session onto its own default session state, which is why a local transport is naturally "one instance, one session".

Session capacity and idle reclamation are configured on the server's session store and are not part of the protocol.

## What `IsStateless()` Changes

`ITransport::IsStateless()` is the only channel through which a transport discloses its mode; the server reads it once at construction and narrows functionality accordingly:

- **Multi-round-trip interactions are unavailable**: with a stateless transport `McpServer::IsMrtrSupported()` returns `false` outright — MRTR requires the server to remember "this round is waiting for whose input", and a stateless server cannot. Collecting input repeatedly through elicitation demands a stateful server.
- **The notification delivery path changes**: no SSE broadcast and no append to the event store; only responses tied to a pending request are delivered as JSON.
- **A batch of methods is rejected additionally in stateless mode**: while the request's version header is not pinned to the legacy era, `ping`, `logging/setLevel`, `resources/subscribe` and `resources/unsubscribe` all answer "method not found"; `initialize` is rejected too, but only when the version header is a modern one — a request explicitly pinned to a legacy version may still handshake. Reverse requests (user input collection, sampling, roots) are not covered by this gate: whether they may be sent is decided by the protocol era, see [/en/learn/protocol-versions](/en/learn/protocol-versions).
- **Version headers must agree**: in stateless mode, a request whose version header differs from the version inside `_meta` is judged a header mismatch.

## Negotiation Fallback Is Classified by Transport

The default Auto mode probes `server/discover` first, and what happens on a failed probe depends on the transport category rather than on the protocol:

| Probe outcome | stdio-like transports | network transports |
|---------------|-----------------------|--------------------|
| probe timeout | fall back to `initialize` | throw a request-timeout error |
| request exception | fall back to `initialize` | throw a connection-closed error |
| unsupported version whose `supported` list contains the latest | corrective retry once with that version | same |
| second rejection on the corrective retry | hard error, no further fallback | same |
| any other error code | fall back to `initialize` | fall back to `initialize` |

"stdio-like" is decided from the runtime type and covers exactly two shapes: the in-process transport and the stdio client session transport. Everything else counts as a network transport. The rationale: a local process is either alive or dead, so an unreachable probe almost always means the peer does not know the method; a failed network probe may be packet loss, latency or a broken peer, and falling back silently would mask the real fault — so it throws instead. The corrective retry has a one-shot budget and a second rejection is fatal, which prevents the two ends from probing each other in a loop.

The three connect modes and which era each lands in are covered in [/en/learn/protocol-versions](/en/learn/protocol-versions).

## Further Reading

- [/en/reference/transports](/en/reference/transports) — option struct field tables for every transport
- [/en/learn/protocol-versions](/en/learn/protocol-versions) — era detection and the dual-era matrix
- [/en/learn/architecture](/en/learn/architecture) — threading model of transports and engine
- [/en/learn/extensions](/en/learn/extensions) — how statelessness limits extensions
- [/en/howto/connect-to-a-server](/en/howto/connect-to-a-server) — picking a connect mode per transport
- [/en/howto/manage-subprocesses](/en/howto/manage-subprocesses) — subprocess lifetime behind the stdio transport
