# Report Progress

Tell the caller how far a slow request has gotten before it returns: the server reads the `progressToken` carried by the request, emits `notifications/progress` with `McpServer::SendProgress`, and the client receives it in `RequestOptions::on_progress`. Signature and field tables live only in [/en/reference/server](/en/reference/server), [/en/reference/client](/en/reference/client) and [/en/reference/types](/en/reference/types); this page only covers the procedure.

::: info Division of labour with tasks and logs
Progress is a **transient heartbeat inside one request**: the token stops meaning anything once the request returns. Use [/en/howto/run-long-tasks](/en/howto/run-long-tasks) when the user must be able to check back after disconnecting, and [/en/howto/send-log-notifications](/en/howto/send-log-notifications) for human-readable log lines.
:::

## Prerequisites

- Headers: `mcp/server/McpServer.hpp`, `mcp/server/RequestContext.hpp`, `mcp/server/ServerOptions.hpp`, `mcp/client/McpClient.hpp`, `mcp/client/ClientOptions.hpp`, `mcp/McpTypes.hpp`, `mcp/JsonValue.hpp`, `mcp/Content.hpp`, `mcp/Implementation.hpp`, `mcp/transport/InMemoryTransport.hpp`; always write the full subdirectory path.
- Token type: `ProgressToken = std::variant<std::string, int64_t>` (`include/mcp/Meta.hpp:15`). Only strings and integers are legal on the wire; anything else makes the whole request get rejected (`src/protocol/McpSessionHandler.cpp:52-53`).
- Flags: progress needs no extension flag and no declared server capability — it is a built-in protocol notification.
- No extra CMake option required.

## Read the progressToken

The token is not a business field of `CallToolRequestParams`; the SDK normalises the request `_meta` into `params.meta`, so always read `ctx.Params().meta->progress_token`, which yields `std::optional<ProgressToken>`. An empty optional is the normal case, not an error.

<<< @/snippets/report-progress.cpp#progress-server

The decision point is `src/server/McpServer.cpp:2093-2108`: the modern era deserialises from the envelope-level `_meta`, the legacy era reads `params._meta`, and both end up in the same `params.meta->progress_token`, so business code needs no branching.

## Report from the server

`SendProgress(token, progress, total?, message?)` is a public `McpServer` method; inside a handler reach it through `ctx.Server()`, and outside a handler (background thread, timer) you may call it with a cached token. Only `token` and `progress` are mandatory.

- `progress` is a **raw monotonically increasing counter**; the SDK neither normalises it nor checks it against `total`, so the client computes the ratio itself.
- `total` may be omitted; omitting it means the total is unknown.
- The token is **passed through verbatim with no ownership check**: reporting request B's progress under request A's token routes the frame into A's stream (`src/server/McpServer.cpp:1091-1109` contains no ownership validation at all). Cross-request token reuse is on you.

<<< @/snippets/report-progress.cpp#progress-total-less

## Receive progress on the client

Attach `on_progress` to `RequestOptions`; its type is `std::function<void(const ProgressNotificationParams&)>` (`include/mcp/McpTypes.hpp:402`). Without an explicit token the client mints an auto-incrementing integer token and writes it into `_meta.progressToken`; an explicit one is used as given (`src/client/McpClient.cpp:937-967`). The callback is detached automatically when the request finishes, so there is nothing to clean up by hand.

<<< @/snippets/report-progress.cpp#progress-client

The callback fires only for frames whose token matches **that request**, so a stateless lambda is the least surprising shape; marshal to the UI thread inside the callback if you need to repaint.

## Behaviour without a token

- **Server side**: `ctx.Params().meta->progress_token` is empty. The only correct move is to **skip reporting** — never invent a token, because the peer registered none and the frame would have no owner. Just return the tool result normally, as the degrade branch in the `progress-server` snippet above does.
- **Client side**: when `RequestOptions::on_progress` is unset the client still registers an auto token, but incoming progress frames are silently discarded (case `McpClientTest.CallToolWithoutProgressCallbackIgnoresProgress`).
- Progress notifications **never affect the request result**: dropping a frame, sending one early or late changes neither the response body nor the success of the call.

::: tip Progress renews long requests
Progress frames reset the timeout guard of the matching request: on receiving a client-directed `notifications/progress` the server calls `ResetTimeoutByProgressToken` (`src/server/McpServer.cpp:1437-1455`), and the client-side behaviour is covered by `SessionHandlerTest.ProgressNotificationExtendsDeadline`. Steady reporting therefore extends the life of a slow request by itself.
:::

## Complete code

The server registers a tool that reports progress; the client calls it once with a callback and once more with an explicit token:

<<< @/snippets/report-progress.cpp#full

## Verification

Snippets are syntax-checked only (clang-cl uses `/Zs`, GCC/Clang use `-fsyntax-only`); no process needs to start:

```bash
cmake -S . -B build/p2d -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p2d --target mcp-doc-snippet-report-progress
```

Expected output: configure prints `[mcp-docs] syntax-checking <N> snippet(s) with '/Zs'` and the build prints

```
[1/4] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-report-progress.dir\__\__\docs\snippets\report-progress.cpp.obj
```

with exit code 0 and no `error:` line (`clang-cl: warning: argument unused during compilation: '-c'` is inherent to this mode and can be ignored).

Behaviour is guarded by these existing cases; run `git submodule update --init third_party/googletest` first, then `ctest --preset debug -R '<case name>'`: `McpServerTest.SendProgressDeliversNotificationWithStringToken`, `McpServerTest.SendProgressDeliversNotificationWithInt64Token`, `McpClientTest.CallToolReceivesProgressNotifications`, `McpClientTest.CallToolWithoutProgressCallbackIgnoresProgress`, `McpClientTest.ProgressCallbackRemovedAfterRequestCompletes`, `SessionHandlerTest.LegacyEraRequestCarriesProgressTokenInParams`, `SessionHandlerTest.ModernEraRequestCarriesProgressTokenInParamsMetaOnWire`, `SessionHandlerTest.IntProgressTokenExtendsDeadline`, `StreamableHttpTransportTest.RequestStreamHoldOpenDeliversProgressBeforeResult`.

## Era differences

| Item | legacy (≤ 2025-06-18) | modern (2026-07-28) |
|---|---|---|
| Where `_meta.progressToken` lands | stays in `params._meta`; `JsonRpcRequest::meta` is empty | hoisted to the envelope-level `_meta`, written back into `params._meta` when serialising |
| How business code reads it | `ctx.Params().meta->progress_token` | identical — the SDK normalises, no branching |
| Delivery channel | `SendNotification` broadcasts onto the transport | tries `BufferRequestStreamNotification` into the owning request's stream first |
| Routing basis | none (requests are not distinguished) | `progress_token_routes_` maps the token back to the originating hold-open request |
| After a buffering miss | not applicable | falls back to a standalone global notification (unlike protocol logs, progress is **not** swallowed by `RequiresRequestScopedNotifications`) |
| Stateless HTTP | not applicable | progress frames and the final response arrive on the same stream, frames first |

The notification body is identical in both eras: `{"progressToken":…,"progress":…,"total":…,"message":…}`, with `total` and `message` optional.

## Common mistakes

| Symptom | Cause | Fix |
|---|---|---|
| The whole request is rejected over an illegal `_meta.progressToken` | The token was a boolean, float or object | Use only a string or an integer |
| The client's `on_progress` never fires | The server could not read a token and invented one, or reported under another request's token | Always read back `ctx.Params().meta->progress_token`; skip reporting when it is empty |
| The server clearly sent frames but the client sees nothing | The callback was attached to a different call, or that call had already returned and the callback was detached | Token lifetime equals request lifetime; never reuse across requests |
| The client computes a ratio above 1 after progress reaches 100 | `progress` is a raw counter — the SDK neither caps nor normalises it | Clamp before reporting, or make `total` match the real number of steps |
| Omitting `total` shows "unknown ratio" on the client | Omitting `total` means the total is unknown — expected | Send `total` when you need a ratio |
| A slow request times out before finishing | No renewing frames in between, or the reporting interval exceeds the timeout window | Report steadily at each milestone (see the tip above) |
| Nothing to check back on after a disconnect | Progress is not durable state | Switch to tasks; see [/en/howto/run-long-tasks](/en/howto/run-long-tasks) |

## Next steps

- Turn a slow tool into a pollable task: [/en/howto/run-long-tasks](/en/howto/run-long-tasks)
- Send structured human-readable log lines: [/en/howto/send-log-notifications](/en/howto/send-log-notifications)
- Ask the user back mid-task: [/en/howto/request-extra-input](/en/howto/request-extra-input), [/en/howto/collect-user-input](/en/howto/collect-user-input)
- Full two-era semantics: [/en/learn/protocol-versions](/en/learn/protocol-versions)
- Signatures and field tables: [/en/reference/server](/en/reference/server), [/en/reference/client](/en/reference/client), [/en/reference/types](/en/reference/types)

## Source behind this page

- `include/mcp/Meta.hpp` (`ProgressToken`, `RequestMeta::progress_token`)
- `include/mcp/McpTypes.hpp` (`CallToolRequestParams::meta`, `ProgressNotificationParams`, `RequestOptions::on_progress`)
- `include/mcp/server/McpServer.hpp` (`SendProgress`)
- `include/mcp/server/RequestContext.hpp` (`Params()`, `Server()`)
- `include/mcp/server/ServerOptions.hpp`
- `include/mcp/client/McpClient.hpp`, `include/mcp/client/ClientOptions.hpp`
- `include/mcp/JsonValue.hpp`, `include/mcp/Content.hpp`, `include/mcp/Implementation.hpp`
- `include/mcp/transport/InMemoryTransport.hpp`
- `src/server/McpServer.cpp` (token normalisation, the reporting implementation, inbound progress renewal)
- `src/client/McpClient.cpp` (token minting, callback registration and detachment)
- `src/protocol/McpSessionHandler.cpp` (`_meta` field validation)
- `src/http/StreamableHttpServerTransport.cpp` (token-based routing into the owning request stream)
