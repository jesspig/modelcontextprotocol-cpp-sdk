# Run a Streamable HTTP Server

Put a server behind an HTTP port so remote clients can connect: wire the transport, control sessions, and decide between stateful and stateless.

## Prerequisites

- Link `mcp-server` and `mcp-http` (dependencies propagate; see [Installation and Integration](/en/start/installation)).
- Plain HTTP. This transport does not provide TLS; terminate TLS in a reverse proxy.
- Port defaults to `3001` and the endpoint to `/mcp` (`include/mcp/transport/StreamableHttpServerTransport.hpp:29-41`).

## Stateful or stateless

`StreamableHttpServerOptions::stateless` **defaults to `true`** (same file, `:43`). The two modes differ in more than "is there a session":

| Aspect | Stateful | Stateless |
|---|---|---|
| Session identity | `initialize` mints one and returns it in the `mcp-session-id` response header (`src/http/StreamableHttpServerTransport.cpp:1172`) | No session issued; every request carries its own context |
| Later requests | Must carry `mcp-session-id`; missing or unknown returns 404 (`:541-543`, `:783-786`) | No session header needed |
| Server-side state | Negotiated version, client capabilities, subscriptions and task owners isolated per session | Independent per request |
| Idle reaping | Expired once past `detail::kSessionIdleTimeoutMs` (`:818`, `:868`, `include/mcp/http/SessionStore.hpp:104`) | Not applicable |
| Typical era | `initialize` handshake, so negotiation lands in the legacy era | `server/discover`, modern era available |

Rule of thumb: pick stateful when the server must push (subscriptions, task status, log broadcasts) or reuse one handshake; pick stateless when you want a connectionless API that scales horizontally.

## Step 1: Assemble a stateful server

Three fields matter: `stateless = false`, a `session_store` implementation, and the `allowed_hosts`/`allowed_origins` allowlists. See the `stateful` region under "Full code".

- Leaving `session_store` empty falls back to the transport's internal session table; inject a `SessionStore` (built-in: `InMemorySessionStore`) only for cross-process sharing or a custom expiry policy.
- Host/Origin validation rejects unknown values — do not treat an empty allowlist as "allow everything".

## Step 2: Session lifecycle

1. The client `POST`s `initialize` to `/mcp` without a session header.
2. The server mints a session id and returns it in the `mcp-session-id` response header.
3. Every following request (including `GET` and `DELETE`) must carry that header; missing, unknown or expired always returns 404 (`:541-543`, `:783-786`, `:868`).
4. When a session is closed, its still-pending requests are settled with the JSON-RPC error `SessionExpired` (`-32009`, `:947-952`) instead of hanging forever.
5. `DELETE` closes only that session, dropping its pending requests and replay records without touching other sessions on the same port.

## Step 3: Stateless and the held-open POST stream

```cpp
http_options.stateless = true;
http_options.listen_hold_open = true;
```

`listen_hold_open` makes the server inject progress, logs and other notifications into the SSE stream of the POST that owns them (routed by `progressToken`), so the client receives notifications and the final response on one request. In stateless mode without this switch, server-initiated notifications have nowhere to go.

## Step 4: Replay after disconnect and legacy SSE

- `event_store` (a `FileEventStore` can be injected) appends and replays events per session; a client `GET` carrying `Last-Event-ID` resumes from that point.
- `enable_legacy_sse` additionally exposes the older SSE endpoint for hosts that do not support Streamable HTTP yet; `sse_keep_alive_ms` (default 15000) sets the heartbeat interval.
- `max_request_body_bytes` and `max_sse_event_bytes` bound inbound and outbound size; exceeding them is rejected, never silently truncated.
- To check that `Mcp-Param-*` headers match the `tools/call` body, wire `resolve_param_annotations` to `McpServer::ResolveToolParamAnnotations` (real wiring: `examples/conformance/server/main.cpp:149-153`).

## Full code

<<< @/snippets/run-a-streamable-http-server.cpp#stateful

<<< @/snippets/run-a-streamable-http-server.cpp#stateless

## Verification

Syntax check first (this command is measured to pass):

```bash
cmake -S . -B build/docs-snippets -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/docs-snippets --target mcp-doc-snippet-run-a-streamable-http-server
```

Then handshake once: start the process, send `initialize` to `http://localhost:3001/mcp` and expect a `mcp-session-id` response header; follow with a request without that header and expect 404. Finally connect a host in stateful mode and confirm the tool list contains `ping`.

## Era differences

Stateful runs the `initialize` handshake, so negotiation can only land in the legacy era. The modern era's single-request model via `server/discover` needs stateless mode plus `listen_hold_open`. Matrix: [Protocol Versions](/en/learn/protocol-versions).

## Common failures

| Symptom | Cause | Fix |
|---|---|---|
| 404 `Not Found` | Session header missing, unknown or expired | Client re-sends `initialize` to obtain a new session |
| Error code `-32009` (`session closed`) | The session was closed and its pending requests were settled | Re-run `initialize`; inject your own `SessionStore` to change the idle policy |
| `InvalidParams` with `unsupported protocol version` (`-32022`) | Declared version not in the support table | Narrow `ServerOptions::supported_protocol_versions` only after confirming peer capability |
| Startup fails on the port | Default 3001 already taken | Change `http_options.port` |
| Client never receives progress or logs | Stateless mode without `listen_hold_open` | Enable the switch, or switch to stateful with subscriptions |

## Next steps

- Get notifications onto the same stream: [Report Progress](/en/howto/report-progress)
- Turn slow tools into tasks: [Run Long Tasks](/en/howto/run-long-tasks)
- All fields and signatures: [Transports Reference](/en/reference/transports)
- Transport choice rationale: [Transports](/en/learn/transports)

## Source files behind this page

- `include/mcp/transport/StreamableHttpServerTransport.hpp`, `src/http/StreamableHttpServerTransport.cpp`, `include/mcp/http/SessionStore.hpp`, `include/mcp/http/EventStore.hpp`, `include/mcp/storage/FileEventStore.hpp`, `examples/conformance/server/main.cpp`
