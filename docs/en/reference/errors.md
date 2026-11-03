# Error Codes and the Error Model

This page is tables and one-line conclusions only: the full `McpErrorCode` enumeration, the actual throw sites of each code inside the SDK, the outbound remapping performed per protocol era, the three ways an error is delivered, and how user handler exceptions converge. Signatures and field details live in [Client reference](/en/reference/client), [Types reference](/en/reference/types) and [Transports reference](/en/reference/transports); era semantics live in [Protocol versions](/en/learn/protocol-versions).

## The `McpError` type

| Symbol | Signature | Conclusion |
|---|---|---|
| `McpError` | `class McpError : public std::runtime_error` | The only error type thrown by the SDK; `what()` is the message text |
| Two-arg ctor | `McpError(McpErrorCode code, std::string message)` | `data` is always empty |
| Three-arg ctor | `McpError(McpErrorCode code, std::string message, std::optional<JsonValue> data)` | Carries the normative payload (`requiredCapabilities`, `supported`, ...) |
| `Code` | `McpErrorCode Code() const noexcept` | Returns the raw enumerator, never normalised |
| `ErrorMessage` | `std::string_view ErrorMessage() const noexcept` | Same source as `what()`, always identical |
| `Data` | `const std::optional<JsonValue>& Data() const noexcept` | Optional; most throw sites leave it empty |

## Full `McpErrorCode` table

Underlying type `int32_t`, 20 entries, listed in declaration order (`include/mcp/ErrorCodes.hpp`).

| Enumerator | Value | Meaning | Typical trigger |
|---|---|---|---|
| `ParseError` | -32700 | The JSON text itself cannot be parsed | Malformed peer message; the text carries the failing offset, and the HTTP layer maps it to 400 |
| `InvalidRequest` | -32600 | Envelope violates JSON-RPC 2.0 or the era's requirements | Wrong `jsonrpc` version, missing or null request `id`, call before initialise, modern era without `_meta` |
| `MethodNotFound` | -32601 | Method unknown, or not part of the negotiated era | Era check missed, extension disabled (skills/tasks/apps), client has no elicitation/sampling/roots handler |
| `InvalidParams` | -32602 | `params` or `_meta` break the contract, plus every "not found" case | Field type/range validation, illegal `_meta` field type, tool/resource/prompt/task/skill miss, `outputSchema` mismatch, tasks request without `taskId` |
| `InternalError` | -32603 | Internal SDK failure; also the convergence code for every non-`McpError` exception | Secure random generation, atomic JSON storage write, DPoP/OAuth internals, generalized handler failures |
| `HeaderMismatch` | -32020 | `Mcp-Method`/`Mcp-Name` headers disagree with the body | Stateful HTTP request missing required headers, `Mcp-Name` not equal to body `taskId` |
| `MissingRequiredClientCapability` | -32021 | Client lacks a required capability; `data.requiredCapabilities` names it | `VerifyCapability` checks, server needing sampling/elicitation/tasks |
| `UnsupportedProtocolVersion` | -32022 | Negotiated version unsupported; `data.supported`/`data.requested` | Unsupported `_meta.protocolVersion`, mismatching HTTP version header, rejected `server/discover` probe |
| `UrlElicitationRequired` | -32042 | Protocol definition: URL-mode elicitation required | See the dedicated section below; **no throw site in the SDK** |
| `ResourceNotFound` | -32002 | Protocol reserved: resource unreadable | See the dedicated section below; **no throw site in the SDK** |
| `ConnectionClosed` | -32000 | Connection or handle already closed | Socket/HTTP read hits the end, `HttpClient` closed, pending requests aborted at close |
| `RequestTimeout` | -32001 | Request timed out | Session deadline (60 s by default), HTTP read/write timeout, URL elicitation watchdog |
| `RequestCancelled` | -32800 | Request cancelled; `data.reason` optional | Peer `notifications/cancelled`, local `CancelRequest` or `RequestHandle::Cancel` |
| `ConnectionRefused` | -32003 | Connection could not be established | TCP connect refused, Winsock initialisation failure |
| `TlsHandshakeFailed` | -32004 | TLS unavailable or the handshake failed | OpenSSL init/`SSL_new`/handshake failure; same code when TLS was not compiled in |
| `ProtocolViolation` | -32005 | Protocol misuse (usually by the peer) | Pagination not converging within 64 pages, `logging/setLevel` or `notifications/roots/list_changed` in the modern era, illegal URL scheme |
| `TaskNotFound` | -32006 | Protocol reserved: task missing | See the dedicated section below; **no throw site in the SDK** |
| `HandlerError` | -32007 | Local callback or extension precondition not met | Task result received without declaring the tasks extension, unknown MRTR input-request method, URL elicitation returning an illegal action, apps/server-card disabled |
| `DeserializeFailed` | -32008 | Typed deserialization failed | `JsonValue` holds a different type, content block/enum cannot be restored, error-code value outside `int32_t` |
| `SessionExpired` | -32009 | Stateful session no longer valid | Server reclaimed the session while requests were still in flight, client POST answered with 404 |

### The three codes without a throw site

::: warning
`UrlElicitationRequired` (-32042), `ResourceNotFound` (-32002) and `TaskNotFound` (-32006) have **no throw site at all** in SDK production code. They appear only in two registration places: the category message branches (`include/mcp/ErrorCodes.hpp`) and the server-side known-code whitelist `IsKnownMcpErrorCode` (`src/server/McpServer.cpp`).
:::

Every "not found" outcome goes through `InvalidParams` instead — task, tool, resource, prompt and skill misses all throw that code from `src/server/McpServer.cpp`. URL-mode elicitation never emits -32042 either: the server simply sends an elicitation request with `mode:"url"`, and a client that has not declared the elicitation capability gets `MissingRequiredClientCapability` — both in `src/server/McpServer.cpp`.

## Bridging to `std::error_code`

| Mechanism | Conclusion |
|---|---|
| `is_error_code_enum` specialization | `McpErrorCode` converts implicitly to `std::error_code` |
| `make_error_code` | `error_code(static_cast<int>(e), GetMcpErrorCategory())` |
| Category name | `McpErrorCategory::name()` is always `"MCP"` |
| `message()` | One English sentence per enumerator, `"Unknown MCP error"` otherwise |
| `default_error_condition()` | `ConnectionClosed`/`RequestTimeout`/`ConnectionRefused`/`TlsHandshakeFailed`/`SessionExpired` → `std::errc::connection_aborted`; `InternalError`/`DeserializeFailed`/`ProtocolViolation`/`HandlerError` → `std::errc::io_error`; `TaskNotFound` → `no_such_process`; `ResourceNotFound` → `no_such_file_or_directory`; everything else → this category itself |

```cpp
std::error_code ec = e.Code();          // implicit, via make_error_code
bool aborted = ec == std::errc::connection_aborted;  // covers 5 codes
```

::: tip
The bridge only covers the code: `McpError` has no member returning `std::error_code`, and `ErrorMessage()`/`Data()` play no part. Compare across layers with `Code()` or with the `errc` values grouped above.
:::

## Era remapping

::: info
`MakeWireCodec` picks `Rev2026Codec` when `protocol_version >= kLatestProtocolVersion` (`"2026-07-28"`) and `Rev2025Codec` otherwise (`src/protocol/WireCodec.cpp`). Remapping applies **only where outbound error responses are encoded**: `McpSessionHandler::SendErrorResponse` and the three response delivery branches (`src/protocol/McpSessionHandler.cpp`). Local `McpError` objects, logs and `data` are untouched.
:::

### The three rewritten codes

| Code | Legacy outbound | Modern outbound | Warning logged |
|---|---|---|---|
| `RequestTimeout` (-32001) | unchanged | `InternalError` (-32603) | yes, `Warning` level |
| `ConnectionRefused` (-32003) | unchanged | `InternalError` (-32603) | yes, `Warning` level |
| `TlsHandshakeFailed` (-32004) | unchanged | `InternalError` (-32603) | yes, `Warning` level |

The evidence sits in `src/protocol/WireCodec.cpp`: the modern codec's `EncodeErrorCode` downgrades the code, while the legacy codec's counterpart returns it unchanged.

### Codes that pass through unchanged

-32020, -32021, -32022 and -32042 are not in the `switch`, so both eras emit them as-is; the remaining internal codes (-32000, -32002, -32005, -32006, -32007, -32008, -32009, -32800) pass through too. A modern-era caller therefore cannot tell timeout from connection refused from a TLS failure by response code alone, only by the message and the peer's warning log.

## Three ways an error is delivered

| Shape | Signature | How the error appears |
|---|---|---|
| Handle | `RequestHandle SendRequest(std::string_view method, JsonValue params, const RequestOptions& options)` | `future<RequestOutcome>` **never throws**; the error is the `ErrorData` branch |
| Raw | `future<JsonValue> SendRequestRaw(...)`; the compatibility overload `SendRequest(..., meta, timeout)` | The error is serialized into a **normal return value** `{"code","message"[,"data"]}` |
| Typed wrapper | `CallTool`/`ListTools`/`GetTask`/`Ping` and friends | Internal `DoSendRequest` sees `code < 0` and does `throw McpError(code, message)` |

### `RequestOutcome`

`using RequestOutcome = std::variant<JsonValue, ErrorData>;`, with `OutcomeCallback` next to it in `include/mcp/protocol/IncomingRequestMeta.hpp`. Errors produced by the session layer itself (connection closed, timeout, cancellation) build an `ErrorData` directly in three settlement paths of `src/protocol/McpSessionHandler.cpp`; `DeliverOutcome` hands the same `ErrorData` to the raw callback serialized and to the typed callback untouched.

### The difference between the two futures

`SendRequest(..., meta, timeout)` merely forwards to `SendRequestRaw` (`src/client/McpClient.cpp`), so their behaviour is identical and only the name differs; the real boundary is the `RequestOptions` overload. An error in the raw future is **not an exception** — check whether `code` in the `get()` result is negative; the `RequestHandle` future needs `std::get_if<ErrorData>`.

::: warning
The typed wrapper restores only `code` and `message`; **`data` is dropped** (`src/client/McpClient.cpp`). Reading `requiredCapabilities` or `supported` requires the `RequestOutcome` or raw shape.
:::

### Inconsistent handling of unknown peer codes

- The client side `ExtractProtocolError` does no whitelist check: any negative `code` is `static_cast` straight into `McpErrorCode` (`src/client/McpClient.cpp`).
- The server side (when it acts as a caller, e.g. elicitation) uses `FindResponseErrorCode`, and **a negative code outside the whitelist is silently treated as a successful payload** (`src/server/McpServer.cpp`).

## Convergence of user handler exceptions

### Server tool handlers

| What the handler throws | Outcome visible to the peer | Code preserved |
|---|---|---|
| `McpError` (sync, non-task path) | `CallToolResult.is_error = true` (wire field `isError`) with `content[0].text = e.ErrorMessage()` | **dropped** (together with `data`) |
| Any other `std::exception` | Also `is_error = true`, text generalized to `Error executing tool <name>`; the original text goes to the `Error` log only | dropped |
| `...` unknown exception | Same as above | dropped |
| Arguments violating `inputSchema` | `is_error = true`, text `invalid arguments: <reason>` | no code at all |
| Result violating `outputSchema` | JSON-RPC error `InvalidParams` | preserved |

::: danger
A `McpError` thrown by a tool handler **never** becomes a JSON-RPC error response; the peer only sees an `isError` text. To let the peer read a code you must rely on SDK-issued errors (capability checks, `InvalidParams`, and so on).
:::

### Protocol, resource and prompt handlers

Non-tool paths propagate `McpError` untouched through `promise.set_exception(std::current_exception())` (twice in `src/server/McpServer.cpp`, and in `src/client/McpClient.cpp` on the client side), and `McpSessionHandler::DeliverResponse` encodes it as a JSON-RPC error: code through the era remapping, `message` from `e.what()`, `data` preserved (`src/protocol/McpSessionHandler.cpp`). Any non-`McpError` exception converges to `InternalError` plus the fixed text `"internal error"`, with the original `e.what()` going to the log only (the neighbouring branch of the same file). When a handler throws **synchronously** instead of using the promise: `McpError` → `reject(e.Code(), e.what(), e.Data())`, any other exception → `InternalError` plus `request handler failed` (both in `src/protocol/McpSessionHandler.cpp`).

### Task-backed calls

Tool calls suspended as a task **keep the code**: `McpError` → failure payload `{"code","message"[,"data"]}` (`src/server/McpServer.cpp`, and the same for the first round of a required task), any other exception → `InternalError` plus the generalized text. The **resume** branch differs: `e.what()` of a `std::exception` is written verbatim into the failure payload, and only `...` falls back to the fixed text (both branches in the same file).

## Symptom → code → likely cause → how to locate it

| Symptom | Code | Likely cause | How to locate it |
|---|---|---|---|
| Tool failed but the peer only sees text | none (`isError`) | Handler exception converged into an `isError` result | Set `MCP_LOG_LEVEL=4` and look for `tool '<name>' failed` |
| Timeout, refused connection and TLS all become internal error in the modern era | -32603 (was -32001/-32003/-32004) | Era remapping | Search for the `internal error code remapped` warning |
| Repeated re-probing or immediate failure after the handshake | -32022 | Declared version not in `supported` | Read `data.supported`/`requested`, see [Protocol versions](/en/learn/protocol-versions) |
| Every HTTP request answered 400 | -32600/-32020 | Envelope missing `id`, missing `Mcp-Method`/`Mcp-Name` header | Status mapping in `src/http/StreamableHttpServerTransport.cpp` |
| 404 for a method name that is correct | -32601 | Method not in the negotiated era, or extension disabled | `WireValidation::NotInEra` branch in `src/protocol/McpSessionHandler.cpp` |
| First request after an idle period fails, retry succeeds | -32009 | Stateful session was reclaimed | `reinit_on_expired_session` (default true) in `include/mcp/client/ClientOptions.hpp` |
| Long task stream drops mid-flight | -32000 | Read hit the end or the peer closed | Distinguish timeout from EOF with `IsEof()`, see [Transports reference](/en/reference/transports) |
| No result after a cancellation | -32800 | Peer `notifications/cancelled` or local `Cancel` | Check `data.reason`; settlement lives in `src/protocol/McpSessionHandler.cpp` |
| List pagination never finishes | -32005 | Peer `nextCursor` does not converge (>64 pages) | The pagination loop in `src/client/McpClient.cpp` |
| Connecting to a TLS endpoint always fails | -32004 | OpenSSL not compiled in (`TLS support not compiled in`) | Check `MCP_HAVE_OPENSSL`, see [Installation](/en/start/installation) |
| MRTR fails after several rounds | -32007 or -32603 | Handler returned an illegal action / `max_rounds` exceeded | The MRTR loop in `src/client/McpClient.cpp` |

Task-shaped guides: [/en/howto/handle-errors](/en/howto/handle-errors), [/en/howto/cancel-a-request](/en/howto/cancel-a-request).

## Source files behind this page

- `include/mcp/ErrorCodes.hpp`, `include/mcp/McpError.hpp`, `include/mcp/JsonRpc.hpp`
- `include/mcp/protocol/IncomingRequestMeta.hpp`, `include/mcp/client/RequestHandle.hpp`, `include/mcp/client/McpClient.hpp`, `include/mcp/client/ClientOptions.hpp`
- Protocol layer: `src/protocol/WireCodec.cpp`, `src/protocol/McpSessionHandler.cpp`
- Server and client: `src/server/McpServer.cpp`, `src/client/McpClient.cpp`
- Core: `src/core/JsonParser.cpp`, `src/core/JsonRpc.cpp`, `src/core/JsonValue.cpp`, `src/core/Content.cpp`
- Transport and networking: `src/http/StreamableHttpServerTransport.cpp`, `src/http/StreamableHttpClientTransport.cpp`, `src/transport/detail/net/HttpClient.cpp`, `src/transport/detail/net/TlsSocket.cpp`, `src/transport/detail/net/TcpSocketPosix.cpp`, `src/transport/detail/net/TcpSocketWin32.cpp`
- Other throw sites: `src/detail/AtomicJsonFile.cpp`, `src/ext/DPoP.cpp`
