# Debug a stuck server

The request went out and sank without a trace. This page splits "no response" into three decidable cause classes and gives the shortest diagnostic for each: internal logs, a log sink of your own, span hooks, and reproduction via conformance. Protocol semantics themselves live in [/en/learn/protocol-versions](/en/learn/protocol-versions).

## Classify first: three kinds of "no response"

| What you observe | Which class | Jump to |
|---|---|---|
| The client waits, then gets `RequestTimeout(-32001)` | the server swallowed the request (promise never fulfilled, or an exception lost inside `std::async`) | cause one |
| The connection is declared dead after an idle stretch | the read loop treats "no data" as EOF | cause two |
| An error frame comes back at once, coded `-32021`/`-32601` | capability not declared or era mismatch | cause three |

Tell "connection died" from "request never settled": the former fires `ClientOptions::on_transport_close` / `on_transport_error`, the latter does not. Transport callbacks are covered in [/en/howto/connect-to-a-server](/en/howto/connect-to-a-server).

## Turn on internal logging

Logging is **off by default**. `MCP_LOG_LEVEL` accepts 0-5 (`Off`/`Error`/`Warning`/`Info`/`Debug`/`Trace`) and is read exactly once, when the logging subsystem is first touched; any value `>= 5` becomes `Trace`, and a non-numeric or 0 value means off (`include/mcp/Log.hpp:51-64`). To change it at runtime, call `mcp::SetLogLevel(LogLevel::Debug)`, which overwrites the current level directly (`include/mcp/Log.hpp:99-101`).

<<< @/snippets/debug-a-stuck-server.cpp#enable-internal-logs

The default sink writes every record to **stderr** as `[MCP][hh:mm:ss][tag] message`, and the timestamp is **UTC** (`gmtime` plus `%H:%M:%S`, `include/mcp/Log.hpp:66-78`, `:124-128`). That matters for stdio servers: stderr never pollutes the protocol stream, but if the child's stderr is discarded you see nothing at all — capture it with `StdioClientTransportOptions::stderr_path` or `on_stderr`, see [/en/howto/manage-subprocesses](/en/howto/manage-subprocesses).

## Replace the log sink

`SetLogHandler` takes over all internal logging. Records carry `level`, `tag`, `message`, `file`, `line`, and an optional `context` (`request_id`, `method`, `session_id`, `peer` — `include/mcp/Log.hpp:23-39`). Once a custom sink is installed the **default stderr output stops**; pass `nullptr` to restore it.

<<< @/snippets/debug-a-stuck-server.cpp#custom-log-sink

::: info A throwing log sink cannot break the protocol
The handler call inside `LogWrite` is wrapped in `try { } catch (...) { }` (`include/mcp/Log.hpp:116-119`) — the exception is swallowed and the request goes on. Conversely: do not expect an exception raised in your log sink to be catchable upstream.
:::

## Watch frames in and out with span hooks

`SpanEvent` is the structured observation point: `kind` is one of `ServerRequest` / `TransportReceive` / `TransportSend`, `phase` is `Begin` or `End`, and it carries `method`, `request_id`, `failed` plus `traceparent`/`tracestate`/`baggage` (`include/mcp/SpanHooks.hpp:9-32`). Servers attach it via `ServerOptions::span_handler` (`include/mcp/server/ServerOptions.hpp:64`), clients via `ClientOptions::span_handler` (`include/mcp/client/ClientOptions.hpp:69`).

<<< @/snippets/debug-a-stuck-server.cpp#span-dump

How to read it: a request ID with `ServerRequest/Begin` but no `ServerRequest/End` means the handler never completed; if both appear but the client got nothing, check whether `TransportSend` fired and whether the peer's `TransportReceive` saw it — the problem is then in transport or routing.

## Cause one: the handler never fulfilled the promise

The server hands each request's reply channel to the handler as a `std::promise<JsonValue>` (`src/protocol/McpSessionHandler.cpp:479-489`). **Neither `set_value` nor `set_exception` means permanent silence**: the SDK does not synthesise an error response for you, and the client can only time out. Asynchronous handlers usually park the promise in a queue and fulfil it later:

<<< @/snippets/debug-a-stuck-server.cpp#held-promise

That is what a `held.Held()`-style counter is for — **expose the number of unfulfilled promises as a metric**, and check whether it grew the moment something sticks. The shipped example teaches exactly this: `examples/RequestCancellation/main.cpp:124-128`.

A sibling trap in the same class: put a **circular `$ref`** into a tool's `input_schema` and the validator's exception lands inside a `std::async` future that `Close()` only `wait()`s on, never `get()`s (`src/server/McpServer.cpp:742-748`), so the exception is forgotten forever and the symptom is again "this request never answers". Avoid self-referential cycles, or expand `$ref` into a finite structure.

## Cause two: treating idle as EOF

This bites when you write your own transport or drive stdio yourself: on POSIX, `Read` polls with a 100 ms timeout and **returns 0 both for "nothing yet" and for end of stream**, so real EOF must be asked of `IsEof()` (`include/mcp/transport/detail/PlatformIO.hpp:20-24`). The SDK's own read loops do precisely that (`src/transport/StdioClientTransport.cpp:103-105`, `:130-132`). Without the extra check, 100 ms of idleness looks like the peer exited, and the symptom is "it drops after a quiet moment".

## Cause three: era or capability mismatch

"Stuck with an immediate error" is usually a declaration/reality mismatch:

- Without `declare_logging` the server advertises no `logger` capability, so the client's `SetLoggingLevel` cannot work; both `declare_logging` and `declare_completions` default to `false` (`include/mcp/server/ServerOptions.hpp:51`).
- The modern era has no `logging/setLevel`: calling `SetLoggingLevel` throws `ProtocolViolation` with a hint to use per-request `logLevel` instead (`src/client/McpClient.cpp:1913-1925`).
- Elicitation requires the **target session** to declare the capability, otherwise `-32021`; session resolution and its fallback order are in [/en/howto/collect-user-input](/en/howto/collect-user-input).
- With the `tasks` or `skills` extension switch off, the entry points answer `MethodNotFound` straight away, see [/en/howto/run-long-tasks](/en/howto/run-long-tasks).

The era matrix lives in [/en/learn/protocol-versions](/en/learn/protocol-versions); first confirm which version the two sides actually negotiated with `McpClient::GetNegotiatedProtocolVersion()`, then decide where to dig.

## Reproduce with conformance

The minimal reproduction harness is the shipped fixture, refereed by the official tool:

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMCP_BUILD_CONFORMANCE=ON
cmake --build build/debug --target conformance-server
CONFORMANCE_SPEC_VERSION=2026-07-28 ./scripts/run-conformance.sh server
```

Notes on `scripts/run-conformance.sh`: `MODE` is `server` or `client`; `PORT` defaults to 3010 and the script **exits immediately if the port is taken** (kill the leftover or pick another port); `CONFORMANCE_SPEC_VERSION` selects the baseline (`tests/conformance/baseline-2026-07-28.yaml` / `baseline-2025-11-25.yaml` / the default `baseline.yaml`); the `2026-07-28` run starts the server with `--stateless`; the referee itself is fetched via `npx -y @modelcontextprotocol/conformance@0.2.0-alpha.12`, which needs network access. Entries inside a baseline are "known not to pass yet" — **newly appearing unexpected failures are the regression signal**.

## Verification

Snippets are syntax-checked only (clang-cl `/Zs`, GCC/Clang `-fsyntax-only`):

```bash
cmake -S . -B build/p3a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3a --target mcp-doc-snippet-debug-a-stuck-server
```

Expected: configure prints `[mcp-docs] syntax-checking 17 snippet(s) with '/Zs'`, the build prints one `Building CXX object ...debug-a-stuck-server.cpp.obj` line, return code 0 with no `error:` lines.

The `full` snippet is itself runnable (a client plus an in-process server that deliberately withholds the promise): it prints `held promises: 1` and returns — the minimal shape of a swallowed request. For the terminal codes after cancellation, run the `RequestCancellation` example (measured output in [/en/howto/cancel-a-request](/en/howto/cancel-a-request)).

## Quick table

| Symptom | First step | Usual cause |
|---|---|---|
| Request never answered | count unfulfilled promises | handler neither set_value nor set_exception |
| One class of requests never answers, no logs | check the schema for circular `$ref` | exception lost inside a `std::async` future |
| Connection drops while idle | inspect the read loop | `Read()` returning 0 without consulting `IsEof()` |
| Error code `-32021` | print `GetNegotiatedProtocolVersion()` | capability not declared for that session |
| `logging/setLevel` raises `ProtocolViolation` | look at the era | modern uses per-request `_meta.logLevel` |
| Nothing visible at all (stdio) | capture stderr | logs go to stderr while the child's stderr is discarded |
| More logging made it harder to find anything | lower the level, install your own sink | `Trace` includes transport detail — noise buries signal |
| Conformance refuses to start | change `PORT` | the port is held by a leftover process |

## Next steps

- Cancellation and timeout semantics: [/en/howto/cancel-a-request](/en/howto/cancel-a-request)
- Subprocesses and stderr capture: [/en/howto/manage-subprocesses](/en/howto/manage-subprocesses)
- Connections and transport callbacks: [/en/howto/connect-to-a-server](/en/howto/connect-to-a-server)
- Full error table: [/en/reference/errors](/en/reference/errors)

## Source behind this page

- `include/mcp/Log.hpp` (`LogLevel`, `LogRecord`, `LogContext`, `SetLogLevel`, `SetLogHandler`, `LogWrite`)
- `include/mcp/SpanHooks.hpp` (`SpanKind`, `SpanPhase`, `SpanEvent`, `SpanHandler`)
- `include/mcp/server/ServerOptions.hpp` (`span_handler`, `declare_logging`)
- `include/mcp/client/ClientOptions.hpp` (`span_handler`, `on_transport_close`, `on_transport_error`)
- `include/mcp/transport/detail/PlatformIO.hpp` (`Read`/`IsEof` semantics)
- `include/mcp/protocol/McpSessionHandler.hpp` (request handlers and the promise handover)
- `src/protocol/McpSessionHandler.cpp`, `src/server/McpServer.cpp`, `src/transport/StdioClientTransport.cpp`
- `scripts/run-conformance.sh`, `tests/conformance/`
- `examples/RequestCancellation/main.cpp`
