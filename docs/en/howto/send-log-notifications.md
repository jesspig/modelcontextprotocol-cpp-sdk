# Send Protocol Log Notifications

Stream log lines produced while the server works to the connected MCP client via `notifications/message`: declare the `logging` capability, filter by level, and emit from inside or outside a handler. Signature and field tables live only in [/en/reference/server](/en/reference/server), [/en/reference/client](/en/reference/client) and [/en/reference/types](/en/reference/types); this page only covers the procedure.

## Prerequisites

- Headers: `mcp/server/McpServer.hpp`, `mcp/server/ServerOptions.hpp`, `mcp/server/RequestContext.hpp`, `mcp/client/McpClient.hpp`, `mcp/client/ClientOptions.hpp`, `mcp/Log.hpp` (only when the internal log needs distinguishing), `mcp/McpTypes.hpp`, `mcp/JsonValue.hpp`, `mcp/Content.hpp`, `mcp/Implementation.hpp`, `mcp/transport/InMemoryTransport.hpp`; always write the full subdirectory path.
- Level enum: `LoggingLevel{Debug, Info, Notice, Warning, Error, Critical, Alert, Emergency}` (`include/mcp/Meta.hpp:17-19`); on the wire these are the official strings `debug`/`info`/`notice`/`warning`/`error`/`critical`/`alert`/`emergency` (`src/core/Content.cpp:328-330`), and an out-of-range value throws `InvalidParams`.
- Capability: protocol logging expects the server to declare the `logging` capability in `initialize`, controlled by `ServerOptions::declare_logging`, **default `false`**.
- Protocol era: level negotiation works completely differently in the two eras — see "Era differences".

## Declare the logging capability

`declare_logging` is the only switch: `capabilities_.logging` is written into the `initialize` response once it is true (`src/server/McpServer.cpp:1959-1961`). Frames can still be sent without it — the SDK performs no capability check before sending — but the client is under no obligation to listen, and it violates the protocol's capability negotiation.

<<< @/snippets/send-log-notifications.cpp#log-capability

## Log from inside a handler

Prefer `RequestContext::Log(level, data)`. It filters against **the current request's** log level before delivering, so a line only ever returns to the client that issued that request and never leaks into another session.

<<< @/snippets/send-log-notifications.cpp#log-in-handler

The three checks run in this order (`include/mcp/server/RequestContext.hpp:64-70`): in the modern era a request without a `_meta` log level drops everything outright; with a level present, lines below it are dropped; only then is the line handed to `McpServer::SendLoggingMessage(level, data, request level)`.

## Log from outside a handler

To add a frame from a background thread, a timer or elsewhere, use the two overloads on `McpServer`:

<<< @/snippets/send-log-notifications.cpp#log-outside-handler

Their real meanings differ a lot (`src/server/McpServer.cpp:961-982`):

- The two-argument `SendLoggingMessage(level, data)` **returns immediately in the modern era and sends nothing**; only the legacy era filters against the level recorded by `logging/setLevel` and sends.
- The three-argument version, given a `min_level`, **filters by `min_level` alone and sends, overriding the `setLevel` record**, and it is not era-gated; passing `std::nullopt` makes it fall back to the two-argument behaviour.

::: warning The two-argument overload is silent in 2026
`McpServerTest.ModernSendLoggingMessageWithoutRequestIsSilent` pins "modern + two arguments = silent" as expected behaviour. To emit deliberately in the modern era you must use the three-argument overload with an explicit `min_level`, or simply call `ctx.Log()` inside the handler.
:::

## Receive and filter on the client

Register `SetLoggingHandler` to consume `LoggingMessageNotificationParams{level, logger, data}` (`include/mcp/client/McpClient.hpp:126`, `include/mcp/McpTypes.hpp:344-348`). `logger` is always the string `"mcp-server"` (`src/server/McpServer.cpp:107`, `988`), and `data` is a **JSON string** on the wire, not an object — serialise structured payloads yourself before putting them in.

The legacy era negotiates once with `SetLoggingLevel`; the modern era carries the level **per request** in `_meta`:

<<< @/snippets/send-log-notifications.cpp#log-client-receive

In the modern era `SetLoggingLevel` throws `ProtocolViolation("logging/setLevel is not available …; use a per-request logLevel instead")` right away (`src/client/McpClient.cpp:1913-1919`). The `_meta` key is fixed as `io.modelcontextprotocol/logLevel` and **must be a string** (`src/protocol/McpSessionHandler.cpp:58-59`); an integer gets the whole request rejected.

## Division of labour with the SDK's internal log

These are two unrelated mechanisms — do not conflate them:

| Dimension | Protocol logging (this page) | SDK internal logging |
|---|---|---|
| Purpose | Business events for the MCP client to see | Framework events for whoever hosts the process |
| API | `ctx.Log`, `McpServer::SendLoggingMessage` | `MCP_LOG`/`MCP_LOG_CTX` macros, `mcp::SetLogLevel`, `mcp::SetLogHandler` |
| Levels | `mcp::LoggingLevel` (8, protocol semantics) | `mcp::LogLevel{Off,Error,Warning,Info,Debug,Trace}` (6, implementation semantics) |
| Switch | `ServerOptions::declare_logging` plus the peer's level | `MCP_LOG_LEVEL` environment variable or `mcp::SetLogLevel`, **off by default** |
| Destination | `notifications/message` frames sent over the transport | stderr, or a custom handler; never leaves the process |

<<< @/snippets/send-log-notifications.cpp#log-vs-internal-log

## Complete code

The server declares the capability and registers a tool that emits protocol logs; the client installs a handler and issues one call with a request-level log level:

<<< @/snippets/send-log-notifications.cpp#full

## Verification

Snippets are syntax-checked only (clang-cl uses `/Zs`, GCC/Clang use `-fsyntax-only`); no process needs to start:

```bash
cmake -S . -B build/p2d -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p2d --target mcp-doc-snippet-send-log-notifications
```

Expected output: configure prints `[mcp-docs] syntax-checking <N> snippet(s) with '/Zs'` and the build prints

```
[1/4] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-send-log-notifications.dir\__\__\docs\snippets\send-log-notifications.cpp.obj
```

with exit code 0 and no `error:` line (`clang-cl: warning: argument unused during compilation: '-c'` is inherent to this mode and can be ignored).

Behaviour is guarded by these existing cases; run `git submodule update --init third_party/googletest` first, then `ctest --preset debug -R '<case name>'`: `McpServerTest.InitializeDeclaresExplicitLoggingAndCompletions`, `McpServerTest.SendLoggingMessageEncodesLevelAsString`, `McpServerTest.ModernLoggingRequiresRequestLogLevel`, `McpServerTest.ModernLoggingHonorsRequestLogLevel`, `McpServerTest.ModernSendLoggingMessageWithoutRequestIsSilent`, `McpClientTest.SetLoggingLevelLegacySendsRequest`, `McpClientTest.SetLoggingLevelModernRejected`, `McpTypesTest.LoggingLevelRoundTrip`, `LogTest.LevelFilterBelowNotTriggered`.

## Era differences

| Item | legacy (≤ 2025-06-18) | modern (2026-07-28) |
|---|---|---|
| How the level is negotiated | Client sends `logging/setLevel`; the server remembers one global level | No `logging/setLevel`; the level travels with each request as `_meta.io.modelcontextprotocol/logLevel` |
| What the server filters on | The level recorded by `setLevel` | The level carried by that request; without one, **not a single line is sent** |
| `SendLoggingMessage(level, data)` | Filters against the recorded level and sends | **Returns immediately, silently sends nothing** |
| Delivery channel | `handler_->SendNotification` broadcast | Tries `BufferRequestStreamNotification` into hold-open request streams first |
| After a buffering miss | Not applicable | Transports that require request-scoped notifications **drop the frame** (unlike progress, which falls back to a global notification) |
| Client `SetLoggingLevel` | Sends the request normally | Throws `ProtocolViolation` |

The notification body is identical in both eras: `{"level":"<official string>","data":"<string>","logger":"mcp-server"}`. The modern era additionally carries `protocolVersion` in the envelope `_meta`.

## Common mistakes

| Symptom | Cause | Fix |
|---|---|---|
| The client receives no log lines at all (modern) | The request carried no `_meta` log level, so `ctx.Log` drops everything by design | Have the client set `io.modelcontextprotocol/logLevel` in `RequestOptions::meta` |
| The request is rejected over an illegal `io.modelcontextprotocol/logLevel` | The field was written as an integer instead of an official string | Use one of the eight strings `"debug"`…`"emergency"` |
| `SetLoggingLevel` throws `ProtocolViolation` in modern | The method was removed from the protocol in 2026 | Switch to a per-request `_meta` level |
| `initialize` reports no `logging` capability | `declare_logging` defaults to `false` | Set it explicitly |
| Two-argument `SendLoggingMessage` prints nothing in modern | That overload returns on its very first line in modern | Use the three-argument overload with an explicit `min_level`, or call `ctx.Log` in the handler |
| A "forced" line still gets filtered out | The three-argument call passed `std::nullopt`, falling back to the two-argument behaviour | Pass a real level to override the filter |
| `data` arrives as a quoted string rather than an object | Protocol logging writes `data` out as a string | Agree on a `Dump()`/`JSON.stringify` convention on both sides |
| Log lines pick the wrong era on a multi-session server | `EmitLoggingMessage`/`SendProgress` decide the era from the **global** negotiated version (`NegotiatedProtocolVersion()` without a session) | On mixed-era multi-session servers, use the explicit three-argument overload and take the version per session |
| `MCP_LOG_LEVEL=4` shows nothing about `notifications/message` | That switch controls the SDK internal log, unrelated to protocol logging | See "Division of labour with the SDK's internal log" |

## Next steps

- Report progress ratios and milestones: [/en/howto/report-progress](/en/howto/report-progress)
- Turn a slow tool into a pollable task: [/en/howto/run-long-tasks](/en/howto/run-long-tasks)
- Push resource changes: [/en/howto/serve-a-resource](/en/howto/serve-a-resource)
- Full two-era semantics: [/en/learn/protocol-versions](/en/learn/protocol-versions)
- Signatures and field tables: [/en/reference/server](/en/reference/server), [/en/reference/client](/en/reference/client), [/en/reference/types](/en/reference/types)

## Source behind this page

- `include/mcp/Meta.hpp` (`LoggingLevel`, `RequestMeta::log_level`)
- `include/mcp/McpTypes.hpp` (`LoggingMessageNotificationParams`, `SetLevelRequestParams`, `RequestOptions::meta`)
- `include/mcp/server/ServerOptions.hpp` (`declare_logging`)
- `include/mcp/server/McpServer.hpp` (both `SendLoggingMessage` overloads)
- `include/mcp/server/RequestContext.hpp` (`Log`, `LogLevel`)
- `include/mcp/client/McpClient.hpp` (`SetLoggingHandler`, `SetLoggingLevel`)
- `include/mcp/Log.hpp` (`LogLevel`, `SetLogLevel`, `SetLogHandler`, `MCP_LOG_LEVEL`)
- `include/mcp/JsonValue.hpp`, `include/mcp/Content.hpp`, `include/mcp/Implementation.hpp`
- `include/mcp/transport/InMemoryTransport.hpp`
- `src/server/McpServer.cpp` (capability declaration, the two overloads and `EmitLoggingMessage`, per-request level parsing)
- `src/client/McpClient.cpp` (`setLevel` era gating and handler registration)
- `src/protocol/McpSessionHandler.cpp` (`_meta` field validation)
- `src/core/Content.cpp` (official level string mapping)
- `src/http/StreamableHttpServerTransport.cpp` (`notifications/message` broadcast into hold-open streams)
