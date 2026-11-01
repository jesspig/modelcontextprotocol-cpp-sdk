# Cancel a request and handle timeouts

Take a request that is already in flight but no longer wanted **out of the running as early as possible**: the client calls `RequestHandle::Cancel` or lets the deadline expire, the SDK settles it locally into a terminal error straight away, and decides per era and transport whether the peer gets a notice; the server can only notice cooperatively and is never interrupted. Signatures and field tables live in [/en/reference/client](/en/reference/client) and [/en/reference/transports](/en/reference/transports) — this page is about the operations.

::: info Division of labour with closing and tasks
Cancellation targets **one request ID**. To tear down the whole connection use `McpClient::Close()` (everything in flight settles as `ConnectionClosed`, see [/en/howto/connect-to-a-server](/en/howto/connect-to-a-server)); to keep work queryable and resumable after a disconnect, use tasks — see [/en/howto/run-long-tasks](/en/howto/run-long-tasks).
:::

## Prerequisites

- Headers: `mcp/client/McpClient.hpp`, `mcp/client/RequestHandle.hpp`, `mcp/CancelToken.hpp`, `mcp/McpTypes.hpp`, `mcp/protocol/IncomingRequestMeta.hpp`, `mcp/Content.hpp`, `mcp/JsonValue.hpp`, `mcp/server/McpServer.hpp`, `mcp/server/RequestContext.hpp`, `mcp/transport/InMemoryTransport.hpp` — include paths must carry the subdirectory.
- Only the `RequestOptions` overload of `SendRequest` returns a cancellable `RequestHandle` (`include/mcp/client/McpClient.hpp:130-133`); there the options argument is **mandatory — there is no default argument**.
- No extension switch and no server capability required: cancellation is the protocol's built-in `notifications/cancelled`.
- `RequestHandle` is move-only (`include/mcp/client/RequestHandle.hpp:29-32`); don't copy one out of a container.

## Cancel an in-flight request

`RequestHandle::Cancel(reason)` takes an optional reason. The settlement order is fixed and has three steps (`src/protocol/McpSessionHandler.cpp:1121-1138`): first the ID is **removed** from the pending table (if it isn't there the call returns silently — so cancelling twice, or cancelling something already settled, is a no-op), then era and transport decide how the peer is told, and finally `RequestCancelled(-32800)` is delivered locally.

<<< @/snippets/cancel-a-request.cpp#typed-handle

A non-empty reason is placed into `ErrorData::data` as `{"reason": ...}` and is also carried on the outgoing notification (`:1129-1133`, `:1153-1164`). `RequestOutcome` is `std::variant<JsonValue, ErrorData>` (`include/mcp/protocol/IncomingRequestMeta.hpp:29`), so `std::get_if<JsonValue>` splits success from failure.

::: warning Cancellation is the local end of the story
Once the entry has been removed, a result the server produces later **has nobody to claim it** — the response finds no pending entry and is dropped. Never treat "it may still finish" as a reason to keep waiting.
:::

## Timeout, renewal, and whether a cancel is sent

`RequestOptions::timeout` is this request's deadline; `ClientOptions::max_total_timeout` sets a client-wide absolute deadline, `0` by default meaning disabled (`include/mcp/client/ClientOptions.hpp:53`), and once enabled every outgoing request registers an absolute deadline (`src/client/McpClient.cpp:627`, `src/protocol/McpSessionHandler.cpp:848-851`) — whichever of the two comes **first** fires (`:310-340`). On expiry:

- with `cancel_on_timeout` true (**the default**, `include/mcp/McpTypes.hpp:404`) the transport-level cancellation also runs, with the reason hard-coded to `"timeout"`;
- the local terminal code is **always `RequestTimeout(-32001)`**, never `-32800` — to tell "I cancelled it" from "it timed out", read the code, not the reason.

<<< @/snippets/cancel-a-request.cpp#timeout-only

A request carrying a `progressToken` gets its remaining time clamped up to at least the renewal step whenever a progress frame arrives (`ResetTimeoutByProgressToken`, `:343-354`), so reporting progress keeps a slow request alive — see [/en/howto/report-progress](/en/howto/report-progress).

## Cancel a whole batch with one token

`CancelToken` is a copyable handle over shared state: put it into `RequestOptions::cancel_token` and several requests hang off one cancellation, where a single `token.Cancel(reason)` takes them all out (`src/client/McpClient.cpp:1867-1872`). Handlers are detached automatically when the request ends (subscription attach/detach at `src/client/McpClient.cpp:321-357`) — no cleanup needed on your side.

<<< @/snippets/cancel-a-request.cpp#cancel-token-fanout

`CancelToken` also offers `IsCancelled()` / `Reason()` / `AddCancelHandler()`, so you can check before dispatching more work that is doomed to be cancelled.

## When you only have the request ID

If no handle is at hand (say you built your own outbound queue), use `McpClient::CancelRequest(id, reason)` — the semantics are identical, because both simply forward to `McpSessionHandler::CancelRequest` (`src/client/McpClient.cpp:1909-1911`).

<<< @/snippets/cancel-a-request.cpp#cancel-by-id

## Three failure channels — don't mix them up

The same "failure" looks completely different across the three API shapes, which is the most common pitfall:

| Call shape | What failure looks like | Evidence |
|---|---|---|
| Convenience methods (`CallTool`, `ListTools`, `ReadResource`, …) | **throws `McpError`**; code and message via `Code()` / `what()` | `src/client/McpClient.cpp:81-89` |
| `SendRequestRaw` (and the four-argument `SendRequest`, which just forwards to it) | the future **resolves normally** with the serialised error object `{"code": <negative>, "message": ...}`, no throw | `src/protocol/McpSessionHandler.cpp:99-102`, `src/client/McpClient.cpp:62-72`, `:1884-1902` |
| `SendRequest(method, params, RequestOptions)` | a `RequestOutcome` variant; pull the error with `std::get_if<ErrorData>` | `include/mcp/client/RequestHandle.hpp:14-16` |

<<< @/snippets/cancel-a-request.cpp#three-channels

The convenience methods can throw because they re-inspect the raw value: if `code` is present and negative they throw. Legitimate results with a non-negative `code` are untouched — but **on the raw channel you have to do that check yourself**, otherwise you will parse an error object as if it were the payload.

## The server can only notice cooperatively

When the peer's cancellation arrives the SDK does exactly one thing: it sets the atomic flag for that (session, request ID) pair (`src/protocol/McpSessionHandler.cpp:1086-1096`). **It does not interrupt your handler** and does not produce an error on your behalf — `RequestContext::IsCancellationRequested()` merely reads that flag (`include/mcp/server/RequestContext.hpp:72-74`), so if you want to stop early you must poll.

<<< @/snippets/cancel-a-request.cpp#server-side-poll

Flags are erased when the request ends and cleared when the connection closes (`src/protocol/McpSessionHandler.cpp:1175`, `:265`), so polling only means something while that request is alive. Inside long loops it is also worth reporting progress, see [/en/howto/report-progress](/en/howto/report-progress).

::: warning Hitting the flag requires the same session
Inbound cancellation looks the flag up by `SessionRequestKey(session_id, request_id)`, while the pending table is keyed by the bare request ID (`:1088`, `:1098`). Under stateful HTTP, a notification whose `sessionId` differs from the request's session simply misses.
:::

## Complete code

One process hosts both ends: register a tool that polls the cancellation flag, start the call from the client, cancel it immediately, and print the terminal outcome.

<<< @/snippets/cancel-a-request.cpp#full

## Verification

Snippets are syntax-checked only (clang-cl uses `/Zs`, GCC/Clang use `-fsyntax-only`); no process is needed:

```bash
cmake -S . -B build/p3f -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3f --target mcp-doc-snippet-cancel-a-request
```

Expected: configure prints `[mcp-docs] syntax-checking 13 snippet(s) with '/Zs'`, and the build prints

```
[1/2] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-cancel-a-request.dir\__\__\docs\snippets\cancel-a-request.cpp.obj
```

Return code 0 with no `error:` lines (`clang-cl: warning: argument unused during compilation: '-c'` is inherent to this mode).

For the real terminal codes, run the shipped example (needs `-DMCP_BUILD_EXAMPLES=ON`):

```bash
cmake --build build/p3f --target RequestCancellation
build/p3f/examples/RequestCancellation/RequestCancellation.exe
```

Measured output — four lines, return code 0:

```
timeout handle id: 2
error: request timed out (code -32001)
cancel handle id: 3
error: request cancelled (code -32800)
```

That is "timeout settles as `-32001`, explicit cancel settles as `-32800`" — both produced locally, regardless of whether the peer actually stopped.

## Era differences

| Item | legacy (≤ 2025-11-25) | modern (2026-07-28) |
|---|---|---|
| Notification style | always sends `notifications/cancelled` | if the transport implements `IRequestCanceller` it **only closes that request's SSE stream** and sends no notification; otherwise it falls back to the notification |
| Where that is decided | — | `src/protocol/McpSessionHandler.cpp:1141-1151` (probed with `dynamic_cast`) |
| Interface | — | `include/mcp/transport/IRequestCanceller.hpp:10-14` |
| Local terminal codes | `-32800` / `-32001` / `-32000` | unchanged — local settlement never goes through `EncodeErrorCode` |
| Those codes arriving from the peer | passed through | `-32001`/`-32003`/`-32004` are downgraded to `-32603` (`src/protocol/WireCodec.cpp:271-283`) |

::: tip Closing the stream is not universal
"Close the request stream" reaches only the handler/canceller layer: `InMemoryTransport` and the stdio transport do not implement `IRequestCanceller`, so even in the modern era they still send `notifications/cancelled` (the fallback branch at `:1144-1150`).
:::

## Common mistakes

| Symptom | Cause | Fix |
|---|---|---|
| `Cancel()` called but the future never returns | that request had already been settled (a missing pending entry is a silent no-op) | cancel each ID once; always `get()` or `wait_for` the future |
| Cannot tell a manual cancel from a timeout | both treated as one code | manual cancel is `-32800`, timeout is `-32001`; `reason` is only text |
| Timed out but the server keeps working | `cancel_on_timeout = false`, or the notice had no effect on that transport | keep the default `true` and make the server poll the flag |
| Server received the cancellation but didn't stop | the SDK only sets a flag; it never interrupts a handler | check `ctx.IsCancellationRequested()` before every unit of work |
| A cancelled call still corrupted state | the result is dropped locally, but the server **keeps writing side effects** | collapse side effects as soon as cancellation is observed, or move to tasks |
| Expecting `-32603` for a local timeout in the modern era | remapping happens only on **outgoing** encoding | client-local code stays `-32001`; `-32603` can only come from the peer |
| Compile error about copying `RequestHandle` | the handle is move-only | transfer ownership with `std::move`, never copy |
| Cancellation notice never hits the in-flight request | under stateful HTTP the `sessionId` differs from the request's session | cancel within the same session, see [/en/howto/run-a-streamable-http-server](/en/howto/run-a-streamable-http-server) |

## Next steps

- Connecting and when to close: [/en/howto/connect-to-a-server](/en/howto/connect-to-a-server)
- Making long work queryable: [/en/howto/run-long-tasks](/en/howto/run-long-tasks)
- Keeping slow requests alive with progress: [/en/howto/report-progress](/en/howto/report-progress)
- Full error table and remapping: [/en/reference/errors](/en/reference/errors)
- Complete dual-era matrix: [/en/learn/protocol-versions](/en/learn/protocol-versions)

## Source behind this page

- `include/mcp/client/RequestHandle.hpp` (`Cancel`, `future`, move-only)
- `include/mcp/client/McpClient.hpp` (`SendRequest(RequestOptions)`, `SendRequestRaw`, `CancelRequest`)
- `include/mcp/CancelToken.hpp`, `include/mcp/McpTypes.hpp` (`RequestOptions`)
- `include/mcp/protocol/IncomingRequestMeta.hpp` (`RequestOutcome`)
- `include/mcp/transport/IRequestCanceller.hpp`
- `include/mcp/server/RequestContext.hpp` (`IsCancellationRequested`)
- `include/mcp/ErrorCodes.hpp`
- `src/protocol/McpSessionHandler.cpp` (cancel settlement, timeout sweep, inbound flag)
- `src/client/McpClient.cpp` (handle assembly, token subscription, error typing)
- `src/protocol/WireCodec.cpp` (era error-code remapping)
- `examples/RequestCancellation/main.cpp`
