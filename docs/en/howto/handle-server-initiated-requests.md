# Handle server-initiated requests

The server does more than answer your calls — it also **asks you things**: for missing input (elicitation), for a completion you can produce (sampling, deprecated), for your workspace list (roots, deprecated). This page covers which callbacks the client side needs, what the SDK already does for you, and when you have to drive it yourself. Signatures and field tables live in [/en/reference/client](/en/reference/client); the server side of asking is [/en/howto/collect-user-input](/en/howto/collect-user-input).

::: info The two eras take completely different roads
In the legacy era (≤ 2025-11-25) a question is a **standalone JSON-RPC request**: `elicitation/create` lands directly in your handler. In the modern era (2026-07-28) it becomes an `input_required` result of the same tool call plus an `inputResponses` follow-up round, and by default the SDK **runs that exchange for you** — your handler is still invoked exactly once.
:::

## Prerequisites

- Headers: `mcp/client/ClientOptions.hpp`, `mcp/client/McpClient.hpp`, `mcp/McpTypes.hpp`, `mcp/Capabilities.hpp`, `mcp/Content.hpp`, `mcp/JsonValue.hpp`, `mcp/client/RequestHandle.hpp`, `mcp/protocol/IncomingRequestMeta.hpp`, `mcp/transport/InMemoryTransport.hpp` — include paths must carry the subdirectory.
- All four handler slots live on `ClientOptions` (`include/mcp/client/ClientOptions.hpp:42-45`): `elicitation_handler`, `url_elicitation_handler`, `sampling_handler`, `roots_handler`; after the client exists you can still attach them with `SetElicitationHandler` / `SetUrlElicitationHandler` (`include/mcp/client/McpClient.hpp:117-121`).
- Handlers must **return quickly**: they run on the protocol dispatch path, so blocking one blocks the connection. To ask a human, hand the question to your own UI thread and let the handler only deliver the answer.
- No CMake switch required.

## Form elicitation

The handler receives `ElicitRequestParams` (`message`, `requested_schema`, `mode="form"`) and returns `ElicitResult{action, content}` (`include/mcp/McpTypes.hpp:350-361`). The three states are simply three `action` strings: `accept` (with `content`), `decline`, `cancel`.

<<< @/snippets/handle-server-initiated-requests.cpp#form-handler

::: warning "No handler" does not mean "no answer"
In form mode a missing handler makes the client reply with `MethodNotFound` + `elicitation not supported` (`src/client/McpClient.cpp:735-738`) — the server treats that question as failed, not as "the user hasn't answered yet".
:::

Before the answer goes back, one **optional** default-filling step runs: only if you explicitly declare `"applyDefaults": true` inside `capabilities->elicitation->form` does the SDK fill the fields you left out from the `requested_schema` defaults (`src/client/McpClient.cpp:239-265`). Without that declaration nothing is filled in for you.

## URL mode: hand the user off externally

With `mode == "url"` the `url_elicitation_handler` is invoked and the params carry `url`. You only need to state a position: `accept` means the user has started the external flow, and the server collects the real outcome later.

<<< @/snippets/handle-server-initiated-requests.cpp#url-handler

Three implementation details worth knowing:

- With no URL handler attached the answer is automatically **`decline`**, never an error (`src/client/McpClient.cpp:296-305`).
- In the legacy era a URL question must carry `elicitationId`; without it the client rejects with `InvalidParams` (`:710-715`).
- Also in the legacy era, after answering, the client **sends `notifications/elicitation/complete`** itself (`{"elicitationId": ...}`) to wake the waiting server (`:722-733`); the modern era has no such step and settles through the follow-up round.

## The capability table is derived from handlers

`DeriveClientCapabilities` (`src/client/McpClient.cpp:360-392`) builds the capability table from what is present: form handler → `elicitation.form`, URL handler → `elicitation.url`, sampling → `sampling`, roots → `roots` plus `list_changed = true`. If none is set and no explicit `capabilities` are given, it returns nothing — i.e. **no capabilities declared at all**. Capabilities you wrote yourself are never overwritten; the SDK only fills gaps.

<<< @/snippets/handle-server-initiated-requests.cpp#capability-derivation

That means "attach a handler" ≈ "declare the capability", and neither declaring without implementing nor implementing without declaring works: the server never reads your code, it only reads the `initialize` capability table.

## Deprecated sampling and roots

`SetSamplingHandler` and `SetRootsHandler` carry `[[deprecated("... SEP-2577")]]` (`include/mcp/client/McpClient.hpp:113-116`); from 2026-07-28 onward the server no longer raises either request type. `Ping()` is deprecated the same way (`:108-109`). The migration shape is to keep receiving legacy peers' requests through the plain **member fields** on `ClientOptions`, and to express the same need as an MRTR question instead:

<<< @/snippets/handle-server-initiated-requests.cpp#sampling-roots

`CreateMessageResult` and `ListRootsResult` remain perfectly usable result types (`include/mcp/McpTypes.hpp:378-392`) — you just shouldn't rely on the server asking. `MakeInputRequestForSampling` / `MakeInputRequestForRoots` (`:371-373`) are constructors for **you** to request the equivalent input in a follow-up round.

## Automatic rounds

In the modern era, when a tool result carries `resultType == "input_required"`, `SendRequestWithMrtr` fills the answer in and re-sends on the spot until it gets a complete result or hits a limit (`src/client/McpClient.cpp:1069-1135`). The defaults are `auto_fulfill = true`, `max_rounds = 10`, `round_timeout = 600s` (`include/mcp/client/ClientOptions.hpp:55-63`).

Rules that matter:

- **A missing required handler is not an error**: the `input_required` result is handed back to you as-is (`src/client/McpClient.cpp:997-1009`) — the one silent-degradation shape of automatic mode, so always inspect the result before treating it as success.
- Each round's timeout is the smaller of `round_timeout` and the caller's timeout; `max_total_timeout` is the whole-flow budget and throws `RequestTimeout` + `MRTR: exceeded max_total_timeout` when exceeded (`:1077-1097`).
- Running out of rounds throws `InternalError` + `MRTR: exceeded max_rounds (N)` (`:1133-1134`).
- A round that only refreshes `requestState` **backs off exponentially** and retries without filling `inputResponses` (`:1116-1121`).
- On `SessionExpired` with `reinit_on_expired_session = true` the client re-runs `initialize` and retries once (`:1057-1066`).

To take the exchange over yourself, turn automatic fulfilment off:

<<< @/snippets/handle-server-initiated-requests.cpp#disable-auto-fulfil

## Manual rounds: replying to input_required yourself

`CallToolResult` exposes `result_type` and `input_required` (`include/mcp/McpTypes.hpp:109-117`, `:268-273`). Note that `CallTool()` **does not** pass `RequestOptions::input_responses` / `request_state` into the outgoing params (`src/client/McpClient.cpp:1312-1356`), so a follow-up round must go through `SendRequest` with both keys placed directly in `params`: `inputResponses` and `requestState` (`src/detail/JsonFields.hpp:21`, `:33`).

<<< @/snippets/handle-server-initiated-requests.cpp#manual-round

Build every answer with the official constructors — `MakeInputResponseFromElicitResult` / `MakeInputResponseFromCreateMessageResult` / `MakeInputResponseFromListRootsResult` (`include/mcp/McpTypes.hpp:374`, `:394-395`) — rather than hand-assembling the shape, and **reuse the keys of `input_requests` verbatim** (`src/client/McpClient.cpp:1019-1047`). A `method` the SDK does not recognise throws `HandlerError` + `unsupported input request method: ...` (`:1041-1043`).

## Other passive entry points

<<< @/snippets/handle-server-initiated-requests.cpp#passive-notifications

- `SetNotificationHandler(method, handler)` accepts any notification; the callback signature is `std::function<void(const JsonRpcNotification&)>` (`include/mcp/client/McpClient.hpp:29-30`, `:122-124`).
- `SetLoggingHandler` receives protocol log frames only (`:126`).
- The three `*_list_changed` notifications and `resources_updated` are handled **internally**: they invalidate the local response cache (`src/client/McpClient.cpp:748-760`), so no handler is needed from you.
- `SubscribeAsync` merely registers a subscription (modern `subscriptions/listen`), see [/en/howto/serve-a-resource](/en/howto/serve-a-resource).

## Complete code

<<< @/snippets/handle-server-initiated-requests.cpp#full

## Verification

Snippets are syntax-checked only (clang-cl uses `/Zs`, GCC/Clang use `-fsyntax-only`); no process is needed:

```bash
cmake -S . -B build/p3a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3a --target mcp-doc-snippet-handle-server-initiated-requests
```

Expected: configure prints `[mcp-docs] syntax-checking 14 snippet(s) with '/Zs'`, and the build prints

```
[1/4] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-handle-server-initiated-requests.dir\__\__\docs\snippets\handle-server-initiated-requests.cpp.obj
```

Return code 0 with no `error:` lines. If CMake warns that the object directory is 194 characters against the 250 limit, your worktree path is simply too deep — use a shallower `build` directory; it has nothing to do with this page.

## Era differences

| Item | legacy (≤ 2025-11-25) | modern (2026-07-28) |
|---|---|---|
| How the question arrives | standalone `elicitation/create` request into your handler | `resultType == "input_required"` + `inputRequests` on the tool result |
| How the answer travels back | that request's response body | next round's `params.inputResponses` (optionally with `requestState`) |
| Who drives the exchange | the server, one request at a time | the client's `SendRequestWithMrtr` loop, automatic unless disabled |
| Missing handler | form replies `MethodNotFound`; URL auto-`decline` | `input_required` handed back to the caller, no error |
| URL mode wrap-up | client sends `notifications/elicitation/complete` | settled by the follow-up round; no such notification |
| sampling / roots | usable but deprecated | no longer raised by the server (SEP-2577) |

One-line conclusion: **the capability table decides whether the server dares to ask; the era decides what the question looks like.** Full matrix in [/en/learn/protocol-versions](/en/learn/protocol-versions).

## Common mistakes

| Symptom | Cause | Fix |
|---|---|---|
| Server reports "client does not support elicitation" | no handler attached, or attached after `initialize` | set it via `ClientOptions` before creating the client; the setter only affects later requests |
| Tool call "succeeded" but the payload is empty | automatic rounds degraded because a handler was missing, and you treated `input_required` as completion | check `result_type == ResultType::InputRequired` and whether `input_required` has a value |
| `MRTR: exceeded max_rounds (10)` thrown | the server demands new input every round | raise `max_rounds`, or move to tasks — see [/en/howto/run-long-tasks](/en/howto/run-long-tasks) |
| The follow-up round is rejected as invalid | `requestState` not echoed, or `inputResponses` keys renamed | copy the keys and `requestState` back verbatim |
| Passing `RequestOptions::input_responses` to `CallTool` does nothing | `CallTool` does not forward those two fields | use `SendRequest` for the round |
| URL question fails with `InvalidParams` in legacy | the request lacks `elicitationId` | that is a protocol defect on the peer; the client refuses per spec |
| A modal dialog inside the handler wedges the connection | handlers run on the dispatch path | move the interaction to your own thread; the handler only fetches or states an answer |
| Schema defaults were not applied | `applyDefaults` not declared | declare `{"applyDefaults": true}` in `capabilities->elicitation->form` |

## Next steps

- Asking from the server: [/en/howto/collect-user-input](/en/howto/collect-user-input), [/en/howto/request-extra-input](/en/howto/request-extra-input)
- Ending things with cancellation and timeouts: [/en/howto/cancel-a-request](/en/howto/cancel-a-request)
- Connecting and when to close: [/en/howto/connect-to-a-server](/en/howto/connect-to-a-server)
- Capabilities and negotiation: [/en/learn/protocol-versions](/en/learn/protocol-versions), [/en/reference/client](/en/reference/client)

## Source behind this page

- `include/mcp/client/ClientOptions.hpp` (four handler slots, `InputRequiredConfig`)
- `include/mcp/client/McpClient.hpp` (`SetElicitationHandler`, `SetUrlElicitationHandler`, `SetNotificationHandler`, deprecated setters)
- `include/mcp/McpTypes.hpp` (`ElicitRequestParams`, `ElicitResult`, `InputRequest`, `InputRequiredResult`, `Make*InputResponse*`, `ResultType`)
- `include/mcp/Capabilities.hpp` (`ElicitationCapability` `form`/`url`)
- `include/mcp/Content.hpp` (`ContentVariant`, `TextContent`)
- `src/client/McpClient.cpp` (inbound dispatch, capability derivation, URL completion notice, MRTR rounds and backoff)
- `src/detail/JsonFields.hpp` (wire key names `inputResponses`, `requestState`)
