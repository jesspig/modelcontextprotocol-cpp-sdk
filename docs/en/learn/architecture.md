# Architecture

This page covers how the libraries are layered, how the façade classes divide work with the underlying engine, the threading and IO model, and the build-time facts that explain why warnings appear elsewhere in the documentation. Field tables and signatures live in the reference layer: [/en/reference/index](/en/reference/index).

## Library Layers and Dependency Chain

Every product library is a static library — seven targets in total, plus one interface-only target `mcp-openssl` that carries the optional dependency.

```text
mcp-core ──┬── mcp-transport ──┬── mcp-protocol ──┬── mcp-server
           │                   │                  └── mcp-client
           │                   └── mcp-http
           └─────────────────────── mcp-ext ──────┘
```

| Target | Links against | Contents |
|--------|---------------|----------|
| `mcp-core` | threading library | JSON value model, JSON-RPC envelopes, protocol data types, error codes and method constants, capability structs, transport interfaces |
| `mcp-transport` | `mcp-core` | transport implementations and the first-party network stack (TCP/TLS/HTTP/WebSocket), platform IO, child processes |
| `mcp-protocol` | `mcp-transport` | the JSON-RPC engine `McpSessionHandler` and the dual-era codec `WireCodec` |
| `mcp-http` | `mcp-transport`, `mcp-ext` | first-party HTTP server, event store, session store, both sides of Streamable HTTP |
| `mcp-ext` | `mcp-core`, `mcp-transport` | implementations of the apps, DPoP, server card and triggers extensions |
| `mcp-server` | `mcp-protocol`, `mcp-ext` | the `McpServer` façade, registration API, task store |
| `mcp-client` | `mcp-protocol`, `mcp-ext` | the `McpClient` façade, version negotiation, OAuth and token cache |

`mcp-ext` is the link usually left out of summaries: it is not optional. The server, client and HTTP targets all depend on it directly, so it ends up in the binary no matter which façade you link.

## Façade and Engine

Four layers top to bottom, each doing one job:

| Layer | Type | Responsibility |
|-------|------|----------------|
| Façade | `McpServer` / `McpClient` | registration and invocation API, capability derivation, extension switch arbitration |
| Engine | `McpSessionHandler` | message loop, request/response correlation, timeouts and cancellation, filter pipelines, per-session negotiated version |
| Codec | `WireCodec` | selects the era codec from the negotiated version and decides whether a method belongs to this era and how `_meta` and `resultType` reach the wire |
| Queue | `MessageChannel` | bounded async queue joining engine and transport; blocking send when full (backpressure) |

The key point: **the codec belongs to the engine, not to the transport**. Transports only move JSON text; every era difference is arbitrated inside `WireCodec`. Switching transport therefore never changes protocol behaviour, and vice versa.

## Threading and IO Model

No asio — plain threads plus standard-library synchronisation primitives.

| Component | Threads |
|-----------|---------|
| `McpSessionHandler` | message-loop thread + timeout-check thread (100 ms poll) + a single response writer thread |
| stdio transport | read-loop thread |
| SSE client | SSE read thread + sender thread |
| Streamable HTTP client | sender thread + GET listen thread + detached short-lived threads for notifications and responses |
| HTTP server | accept thread + one thread per connection (cap 256) + SSE keepalive thread |

Request handlers are invoked synchronously on the message-loop thread and responses are consumed by one writer thread that awaits the handler promise — **not one thread per request**. A handler that blocks for a long time therefore stalls everything else on that connection, including the receive path apart from the timeout check. To parallelise, hand the work to your own thread inside the handler and fulfil the promise from there.

## Closing From Inside a Callback Self-Joins

The IO threads of the stdio, SSE and HTTP transports execute user callbacks directly (`on_transport_error`, `on_transport_close`). Calling `Close()` from such a callback makes the thread join itself and throws. Internally the SDK always goes through `detail::JoinThreadSafely`: when the thread is the current one it detaches instead of joining. Two consequences follow:

- A detached thread keeps running until it exits naturally, so the transport object must keep itself alive at the entry of its read loop, otherwise the access is use-after-free.
- Detached threads still alive at close are never waited for — do not assume all IO has stopped once `Close()` returns.

## Capabilities Are Derived From Registration

`ServerOptions` has no `capabilities` field: server capabilities are the result of what you registered, not an input you declare. Tools, resources and prompts appear automatically once their registry is non-empty; logging and completions require explicit switches; extension entries follow the extension switches (see [/en/learn/extensions](/en/learn/extensions)). The reason is to make "declared a capability but never implemented it" impossible: whatever is advertised must already be wired up internally.

## Build-Time Facts

These four are not implementation trivia — they are the source of several warnings elsewhere in the docs. The step-by-step treatment belongs to the how-to layer.

- **Unity build is on by default but only for `mcp-core`/`mcp-server`/`mcp-client`/`mcp-http`/`mcp-ext`**; `mcp-protocol` and `mcp-transport` switch it off explicitly. Whether a new source file collides on duplicate static symbols depends on which target it lands in.
- **Release builds enable interprocedural optimization automatically**: ThinLTO for Clang, LTCG for MSVC and clang-cl, IPO for GCC. Slow local Release linking is expected.
- **Non-CI, non-Apple, non-Windows builds add `-march=native` by default** (the CI flag comes from the `CI` environment variable). Resulting binaries are therefore not distributable across machines; turn it off explicitly when you need to ship them.
- **OpenSSL is an optional dependency** (`MCP_WITH_OPENSSL=AUTO|ON|OFF`); availability is applied as a unit — compile definition plus link — by one interface target. Never infer OpenSSL availability from whether its headers are includable: visible headers do not imply the macro is defined, and certainly not that the library is linked.

## Further Reading

- [/en/learn/what-is-mcp](/en/learn/what-is-mcp) — protocol roles and the SDK boundary
- [/en/learn/transports](/en/learn/transports) — transport layering and session semantics
- [/en/learn/protocol-versions](/en/learn/protocol-versions) — the dual-era arbitration done by `WireCodec`
- [/en/learn/extensions](/en/learn/extensions) — switches and how they map onto capabilities
- [/en/reference/process-and-storage](/en/reference/process-and-storage) — process and storage types
- [/en/howto/register-a-tool](/en/howto/register-a-tool) — the real call sequence behind the registration API
- [/en/howto/run-a-streamable-http-server](/en/howto/run-a-streamable-http-server) — server assembly and threading in practice
