# Handle errors

A single "failure" has three completely different exits in the SDK: a thrown `McpError`, an `is_error` tool result, and the `ErrorData` inside `RequestOutcome` — then protocol-era error-code remapping is layered on top. This page gives you the decision order and a retry policy. Cancellation and timeout details are in [/en/howto/cancel-a-request](/en/howto/cancel-a-request).

## Prerequisites

- Headers: `mcp/McpError.hpp`, `mcp/ErrorCodes.hpp`, `mcp/McpTypes.hpp`, `mcp/protocol/IncomingRequestMeta.hpp`, `mcp/client/McpClient.hpp`, `mcp/client/RequestHandle.hpp`, `mcp/server/RequestContext.hpp`; include paths must carry the subdirectory.
- Type testing needs `<variant>`: `RequestOutcome` is `std::variant<JsonValue, ErrorData>` (`include/mcp/protocol/IncomingRequestMeta.hpp:29`).
- Grouping by semantics needs `<system_error>` (already pulled in by the public headers): `McpErrorCode` is registered as a `std::error_code` enum (`include/mcp/ErrorCodes.hpp:32-39`).
- One division of labour has to be understood first: tool-call failures surface as `is_error` results, protocol-method failures as error frames — the two are tested differently, and this page covers each.

## Fix the decision order first

| Which API layer you use | Shape of failure | How to test it |
|---|---|---|
| Convenience methods (`CallTool`, `ListTools`, `ReadResource`, `GetPrompt`, …) | **throws `McpError`** | `catch (const McpError&)` + `Code()` |
| `SendRequestRaw` / the four-argument `SendRequest` | the future resolves normally, the value is a serialised error object | check whether `code` is negative |
| `SendRequest(method, params, RequestOptions)` | `RequestOutcome = std::variant<JsonValue, ErrorData>` | `std::get_if<ErrorData>` |
| Tool handlers (server side) | a `CallToolResult` with `is_error = true`, or an exception the SDK catches | look at `is_error` and `content` |

`McpError` derives from `std::runtime_error` and carries `Code()`, `ErrorMessage()`, `Data()` (`include/mcp/McpError.hpp:13-35`); `Data()` is a `std::optional<JsonValue>` holding the protocol error's `data` field.

## Full code table and its semantic groups

Values are defined in `include/mcp/ErrorCodes.hpp:8-30` (reference table in [/en/reference/errors](/en/reference/errors)):

| Code | Name | Typical source |
|---|---|---|
| `-32700` | `ParseError` | JSON parse failure |
| `-32600` | `InvalidRequest` | malformed request frame |
| `-32601` | `MethodNotFound` | unregistered method or extension switch off |
| `-32602` | `InvalidParams` | parameter validation, registration conflicts, structured-output mismatch |
| `-32603` | `InternalError` | fallback for unknown exceptions, MRTR rounds exhausted |
| `-32020` | `HeaderMismatch` | `Mcp-Param-*` disagrees with the body |
| `-32021` | `MissingRequiredClientCapability` | peer did not declare the required capability |
| `-32022` | `UnsupportedProtocolVersion` | version negotiation rejected; `data.supported` lists candidates |
| `-32042` | `UrlElicitationRequired` | declared and pass-through-listed only, **no throw site inside the SDK** |
| `-32000` | `ConnectionClosed` | close settling in-flight requests, HTTP 401/403 bodies |
| `-32001` | `RequestTimeout` | local timeout |
| `-32002` | `ResourceNotFound` | **no throw site inside the SDK**, can only come from the peer |
| `-32800` | `RequestCancelled` | explicit cancellation |
| `-32003` / `-32004` | `ConnectionRefused` / `TlsHandshakeFailed` | transport connect failures |
| `-32005` | `ProtocolViolation` | call disallowed by the era (e.g. `logging/setLevel` in modern) |
| `-32006` | `TaskNotFound` | **no throw site inside the SDK** |
| `-32007` | `HandlerError` | non-MCP errors from handlers, unknown input request method |
| `-32008` | `DeserializeFailed` | deserialization failure |
| `-32009` | `SessionExpired` | unknown or expired session |

`McpErrorCode` is also a `std::error_code` enum (`include/mcp/ErrorCodes.hpp:32-39`), so you can branch on semantic groups instead of matching codes one by one (`include/mcp/ErrorCodes.hpp:71-91`):

<<< @/snippets/handle-errors.cpp#classify-outcome

`connection_aborted` covers `ConnectionClosed`/`RequestTimeout`/`ConnectionRefused`/`TlsHandshakeFailed`/`SessionExpired`; `io_error` covers `InternalError`/`DeserializeFailed`/`ProtocolViolation`/`HandlerError`; `TaskNotFound` maps to `no_such_process` and `ResourceNotFound` to `no_such_file_or_directory`.

## Era remapping happens only on egress

In the modern era `-32001`, `-32003` and `-32004` are downgraded to `-32603` during wire encoding (`src/protocol/WireCodec.cpp:271-283`); legacy passes them through unchanged (`:158-160`). Two unavoidable consequences:

- A client-**local** timeout stays `-32001` regardless of era; the same error arriving from the peer only ever appears as `-32603` in the modern era.
- To tell "internal error" from "timeout / refused / TLS failure" on the modern era you must rely on `message` text or `data`, never on the code.

## Three tool-layer failures, three different results

<<< @/snippets/handle-errors.cpp#tool-error-surface

1. **Return `is_error = true`**: the protocol call succeeded, the model sees a failure text and can continue. This is what the SDK does when arguments violate `input_schema`, with text prefixed `invalid arguments: ` (`src/server/McpServer.cpp:553-559`, `:2359`, `:2507`, `:2616`).
2. **Throw `McpError`**: `Code()` is **discarded**, only the `message` reaches the tool result text (`src/server/McpServer.cpp:2633-2643`) — you cannot convey a code from a tool.
3. **Throw anything else**: the text is generalised to `Error executing tool <name>`, your message never reaches the peer and lives only in server logs (same location).

::: warning a structured-output mismatch is not is_error
`structured_content` failing `output_schema` is a protocol error `InvalidParams` with the outbound message `tool output does not match outputSchema: <reason>` (`src/server/McpServer.cpp:2673-2681`). If you declare a schema you must honour it — declare less instead.
:::

Protocol-method handlers (resources, prompts, completion, …) **keep the code**: `McpError`'s `Code()` and message go into the error frame verbatim (`src/protocol/McpSessionHandler.cpp:576-586`), while any non-`McpError` exception becomes `InternalError` + `"internal error"`, with the real cause only in server logs (`:587-614`).

## Registration errors: explode at startup, not on first call

Every registration check throws `McpError(InvalidParams)` at the `RegisterTool`/`RegisterResource`/`RegisterPrompt` call site: duplicate names, invalid tool names, malformed resource templates, empty handlers (`src/server/McpServer.cpp:93`, `:287`, `:329`). Funnel registration into one function and let the caller decide between crashing and skipping:

<<< @/snippets/handle-errors.cpp#registration-errors

## Reading the `data` field

The `-32022` candidate list and request-state rejections both convey structured reasons through `data`:

<<< @/snippets/handle-errors.cpp#version-rejection

The key is fixed as `"supported"` (`src/protocol/McpSessionHandler.cpp:35`); a failed `requestState` check answers `InvalidParams` + `data.reason = "invalid_request_state"`. The typed channel exposes it the same way via `ErrorData::data` (`include/mcp/protocol/IncomingRequestMeta.hpp`).

## Retry policy: branch on codes, never on messages

<<< @/snippets/handle-errors.cpp#retry-policy

- `SessionExpired` → rebuild the session and retry once; the client already performs its own "re-initialize and retry once" inside MRTR (`src/client/McpClient.cpp:1057-1066`), so do not stack an unbounded loop on top.
- `RequestTimeout` → retry **only idempotent** operations; leave write-shaped tools to the caller.
- `UnsupportedProtocolVersion` → do not retry; read `data.supported` and decide whether to downgrade. Stdio-style auto fallback is in [/en/learn/protocol-versions](/en/learn/protocol-versions).
- `ConnectionClosed` / `ConnectionRefused` / `TlsHandshakeFailed` → recreate the transport; retrying on the same client instance is pointless.
- `HandlerError` / `InternalError` / `DeserializeFailed` → treat as a peer defect: log it and stop automatic retries.

## The raw channel is the easiest to misjudge

<<< @/snippets/handle-errors.cpp#raw-channel

The raw channel never throws: the error is a **value**. The test is a negative `code` in the serialised object (`src/client/McpClient.cpp:62-72`); miss it and you parse an error object as business data. Convenience methods can throw precisely because they perform this check internally.

## Complete code

<<< @/snippets/handle-errors.cpp#full

The `full` snippet is runnable: both ends in one process, the client calls a tool that never settles with a 120 ms timeout and prints the enum value of the classification (`TimedOut` is `2`).

## Verification

Snippets are syntax-checked only (clang-cl `/Zs`, GCC/Clang `-fsyntax-only`):

```bash
cmake -S . -B build/p3a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3a --target mcp-doc-snippet-handle-errors
```

Expected: configure prints `[mcp-docs] syntax-checking 19 snippet(s) with '/Zs'`, the build prints one `Building CXX object ...handle-errors.cpp.obj` line, return code 0 with no `error:` lines.

Measured terminal codes for cancellation and timeouts are in the verification section of [/en/howto/cancel-a-request](/en/howto/cancel-a-request) (the `-32001` and `-32800` lines verbatim).

## Era differences

| Item | legacy (≤ 2025-11-25) | modern (2026-07-28) |
|---|---|---|
| egress of `-32001`/`-32003`/`-32004` | passed through | downgraded to `-32603` |
| local cancellation/timeout codes | `-32800` / `-32001` | same (never remapped) |
| `ProtocolViolation` trigger | `logging/setLevel` works | `logging/setLevel` throws |
| Main tool-failure channel | `is_error` results | same, plus `input_required` handed back via MRTR |

## Common mistakes

| Symptom | Cause | Fix |
|---|---|---|
| `Code()` differs from what the tool threw | tool-handler codes are discarded | in tools convey semantics via `is_error` plus structured content, not codes |
| Peer reports `-32603` while your log says timeout | modern egress remapping | do not classify cross-era behaviour by code; classification is local only |
| An error object parsed as a result | raw channel used without checking `code` | test for a negative `code`, or switch to the typed channel |
| Retrying the same client forever | the connection is closed | rebuild transport and session |
| Worry that internal exception text leaks to the peer | — | it cannot: non-`McpError` always answers `"internal error"`, details stay in logs |
| Occasional structured-output failures break whole calls | schema declared too wide | narrow `output_schema`, or return unstructured content instead |
| Startup crash with no obvious reason | registration-time `InvalidParams` | funnel registration into one function and report it in aggregate at startup |

## Next steps

- Cancellation and timeouts: [/en/howto/cancel-a-request](/en/howto/cancel-a-request)
- Tool input validation: [/en/howto/validate-tool-input](/en/howto/validate-tool-input)
- MRTR rounds and requestState: [/en/howto/request-extra-input](/en/howto/request-extra-input)
- Full table and throw sites: [/en/reference/errors](/en/reference/errors)

## Source behind this page

- `include/mcp/ErrorCodes.hpp` (`McpErrorCode`, category mapping, `make_error_code`)
- `include/mcp/McpError.hpp` (`Code()`, `ErrorMessage()`, `Data()`)
- `include/mcp/protocol/IncomingRequestMeta.hpp` (`RequestOutcome`, `ErrorData`)
- `include/mcp/client/McpClient.hpp` (the three channel signatures)
- `src/client/McpClient.cpp` (error detection, throwing, session-expiry retry)
- `src/protocol/McpSessionHandler.cpp` (outbound error frames and remapping call sites)
- `src/protocol/WireCodec.cpp` (era `EncodeErrorCode`)
- `src/server/McpServer.cpp` (argument pre-validation, handler fallback, structured-output check, registration checks)
