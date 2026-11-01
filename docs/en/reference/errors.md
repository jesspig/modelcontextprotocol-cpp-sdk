# Error Codes and the Error Model

This page is tables and one-line conclusions only: the full `McpErrorCode` enumeration, the actual throw sites of each code inside the SDK, the outbound remapping performed per protocol era, the three ways an error is delivered, and how user handler exceptions converge. Signatures and field details live in [Client reference](/en/reference/client), [Types reference](/en/reference/types) and [Transports reference](/en/reference/transports); era semantics live in [Protocol versions](/en/learn/protocol-versions).

## The `McpError` type

| Symbol | Signature | Conclusion | Evidence |
|---|---|---|---|
| `McpError` | `class McpError : public std::runtime_error` | The only error type thrown by the SDK; `what()` is the message text | `include/mcp/McpError.hpp:13` |
| Two-arg ctor | `McpError(McpErrorCode code, std::string message)` | `data` is always empty | `McpError.hpp:15-18` |
| Three-arg ctor | `McpError(McpErrorCode code, std::string message, std::optional<JsonValue> data)` | Carries the normative payload (`requiredCapabilities`, `supported`, ...) | `McpError.hpp:20-24` |
| `Code` | `McpErrorCode Code() const noexcept` | Returns the raw enumerator, never normalised | `McpError.hpp:26` |
| `ErrorMessage` | `std::string_view ErrorMessage() const noexcept` | Same source as `what()`, always identical | `McpError.hpp:27` |
| `Data` | `const std::optional<JsonValue>& Data() const noexcept` | Optional; most throw sites leave it empty | `McpError.hpp:28` |

## Full `McpErrorCode` table

Underlying type `int32_t`, 20 entries, listed in declaration order (`include/mcp/ErrorCodes.hpp:8-30`).

| Enumerator | Value | Meaning | Typical trigger | Throw-site evidence |
|---|---|---|---|---|
| `ParseError` | -32700 | The JSON text itself cannot be parsed | Malformed peer message; the text carries the failing offset | `src/core/JsonParser.cpp:35`; mapped to HTTP 400 at `src/http/StreamableHttpServerTransport.cpp:118` |
| `InvalidRequest` | -32600 | Envelope violates JSON-RPC 2.0 or the era's requirements | Wrong `jsonrpc` version, missing or null request `id`, call before initialise, modern era without `_meta` | `src/core/JsonRpc.cpp:17`、`src/core/JsonRpc.cpp:104`、`src/protocol/McpSessionHandler.cpp:419`、`src/server/McpServer.cpp:590` |
| `MethodNotFound` | -32601 | Method unknown, or not part of the negotiated era | Era check missed, extension disabled (skills/tasks/apps), client has no elicitation/sampling/roots handler | `src/protocol/McpSessionHandler.cpp:415`、`src/server/McpServer.cpp:2956`、`src/client/McpClient.cpp:739` |
| `InvalidParams` | -32602 | `params` or `_meta` break the contract, plus every "not found" case | Field type/range validation, illegal `_meta` field type, tool/resource/prompt/task/skill miss, `outputSchema` mismatch, tasks request without `taskId` | `src/core/Content.cpp:213`、`src/protocol/McpSessionHandler.cpp:425`、`src/protocol/WireCodec.cpp:308`、`src/server/McpServer.cpp:2148` |
| `InternalError` | -32603 | Internal SDK failure; also the convergence code for every non-`McpError` exception | Secure random generation, atomic JSON storage write, DPoP/OAuth internals, generalized handler failures | `src/server/McpServer.cpp:438`、`src/detail/AtomicJsonFile.cpp:106`、`src/protocol/McpSessionHandler.cpp:505` |
| `HeaderMismatch` | -32020 | `Mcp-Method`/`Mcp-Name` headers disagree with the body | Stateful HTTP request missing required headers, `Mcp-Name` not equal to body `taskId` | `src/protocol/WireCodec.cpp:311`、`src/http/StreamableHttpServerTransport.cpp:317`、`src/http/StreamableHttpServerTransport.cpp:1197` |
| `MissingRequiredClientCapability` | -32021 | Client lacks a required capability; `data.requiredCapabilities` names it | `VerifyCapability` checks, server needing sampling/elicitation/tasks | `src/protocol/McpSessionHandler.cpp:533`、`src/server/McpServer.cpp:386`、`src/server/McpServer.cpp:1165` |
| `UnsupportedProtocolVersion` | -32022 | Negotiated version unsupported; `data.supported`/`data.requested` | Unsupported `_meta.protocolVersion`, mismatching HTTP version header, rejected `server/discover` probe | `src/protocol/McpSessionHandler.cpp:432`、`src/http/StreamableHttpServerTransport.cpp:362`、`src/client/McpClient.cpp:542` |
| `UrlElicitationRequired` | -32042 | Protocol definition: URL-mode elicitation required | See the dedicated section below | **No throw site in the SDK** |
| `ResourceNotFound` | -32002 | Protocol reserved: resource unreadable | See the dedicated section below | **No throw site in the SDK** |
| `ConnectionClosed` | -32000 | Connection or handle already closed | Socket/HTTP read hits the end, `HttpClient` closed, pending requests aborted at close | `src/transport/detail/net/HttpClient.cpp:60`、`src/transport/detail/net/TcpSocketPosix.cpp:195`、`src/protocol/McpSessionHandler.cpp:275` |
| `RequestTimeout` | -32001 | Request timed out | Session deadline (60 s by default), HTTP read/write timeout, URL elicitation watchdog | `src/protocol/McpSessionHandler.cpp:337`、`src/transport/detail/net/HttpClient.cpp:245`、`src/server/McpServer.cpp:1254` |
| `RequestCancelled` | -32800 | Request cancelled; `data.reason` optional | Peer `notifications/cancelled`, local `CancelRequest` or `RequestHandle::Cancel` | `src/protocol/McpSessionHandler.cpp:1100`、`src/protocol/McpSessionHandler.cpp:1128` |
| `ConnectionRefused` | -32003 | Connection could not be established | TCP connect refused, Winsock initialisation failure | `src/transport/detail/net/TcpSocketPosix.cpp:128`、`src/transport/detail/net/TcpSocketWin32.cpp:87`、`src/transport/detail/net/TcpSocketWin32.cpp:130` |
| `TlsHandshakeFailed` | -32004 | TLS unavailable or the handshake failed | OpenSSL init/`SSL_new`/handshake failure; same code when TLS was not compiled in | `src/transport/detail/net/TlsSocket.cpp:93`、`src/transport/detail/net/TlsSocket.cpp:137`、`src/transport/detail/net/TlsSocket.cpp:279` |
| `ProtocolViolation` | -32005 | Protocol misuse (usually by the peer) | Pagination not converging within 64 pages, `logging/setLevel` or `notifications/roots/list_changed` in the modern era, illegal URL scheme | `src/client/McpClient.cpp:169`、`src/client/McpClient.cpp:1915`、`src/transport/detail/net/HttpClient.cpp:69` |
| `TaskNotFound` | -32006 | Protocol reserved: task missing | See the dedicated section below | **No throw site in the SDK** |
| `HandlerError` | -32007 | Local callback or extension precondition not met | Task result received without declaring the tasks extension, unknown MRTR input-request method, URL elicitation returning an illegal action, apps/server-card disabled | `src/client/McpClient.cpp:290`、`src/client/McpClient.cpp:1042`、`src/server/McpServer.cpp:2768` |
| `DeserializeFailed` | -32008 | Typed deserialization failed | `JsonValue` holds a different type, content block/enum cannot be restored, error-code value outside `int32_t` | `src/core/JsonValue.cpp:184`、`src/core/Content.cpp:123`、`src/core/JsonRpc.cpp:68` |
| `SessionExpired` | -32009 | Stateful session no longer valid | Server reclaimed the session while requests were still in flight, client POST answered with 404 | `src/http/StreamableHttpServerTransport.cpp:949`、`src/http/StreamableHttpClientTransport.cpp:85` |

### The three codes without a throw site

::: warning
`UrlElicitationRequired` (-32042), `ResourceNotFound` (-32002) and `TaskNotFound` (-32006) have **no throw site at all** in SDK production code. They appear only in two registration places: the category message branches (`include/mcp/ErrorCodes.hpp:56-65`) and the server-side known-code whitelist `IsKnownMcpErrorCode` (`src/server/McpServer.cpp:456-483`).
:::

Every "not found" outcome goes through `InvalidParams` instead: task `src/server/McpServer.cpp:1547`, tool `:2148`, resource `:2816`, prompt `:2912`, skill `:3030`. URL-mode elicitation never emits -32042 either: the server simply sends an elicitation request with `mode:"url"` (`src/server/McpServer.cpp:1173`), and a client that has not declared the elicitation capability gets `MissingRequiredClientCapability` (`:1165`).

## Bridging to `std::error_code`

| Mechanism | Conclusion | Evidence |
|---|---|---|
| `is_error_code_enum` specialization | `McpErrorCode` converts implicitly to `std::error_code` | `include/mcp/ErrorCodes.hpp:36-39` |
| `make_error_code` | `error_code(static_cast<int>(e), GetMcpErrorCategory())` | `ErrorCodes.hpp:99-101` |
| Category name | `McpErrorCategory::name()` is always `"MCP"` | `ErrorCodes.hpp:45` |
| `message()` | One English sentence per enumerator, `"Unknown MCP error"` otherwise | `ErrorCodes.hpp:46-70` |
| `default_error_condition()` | `ConnectionClosed`/`RequestTimeout`/`ConnectionRefused`/`TlsHandshakeFailed`/`SessionExpired` → `std::errc::connection_aborted`; `InternalError`/`DeserializeFailed`/`ProtocolViolation`/`HandlerError` → `std::errc::io_error`; `TaskNotFound` → `no_such_process`; `ResourceNotFound` → `no_such_file_or_directory`; everything else → this category itself | `ErrorCodes.hpp:71-91` |

```cpp
std::error_code ec = e.Code();          // implicit, via make_error_code
bool aborted = ec == std::errc::connection_aborted;  // covers 5 codes
```

::: tip
The bridge only covers the code: `McpError` has no member returning `std::error_code`, and `ErrorMessage()`/`Data()` play no part. Compare across layers with `Code()` or with the `errc` values grouped above.
:::

## Era remapping

::: info
`MakeWireCodec` picks `Rev2026Codec` when `protocol_version >= kLatestProtocolVersion` (`"2026-07-28"`) and `Rev2025Codec` otherwise (`src/protocol/WireCodec.cpp:315-322`). Remapping applies **only where outbound error responses are encoded**: `McpSessionHandler::SendErrorResponse` (`src/protocol/McpSessionHandler.cpp:1054-1059`) and the response delivery branches (`:581`, `:592`, `:606`). Local `McpError` objects, logs and `data` are untouched.
:::

### The three rewritten codes

| Code | Legacy outbound | Modern outbound | Warning logged |
|---|---|---|---|
| `RequestTimeout` (-32001) | unchanged | `InternalError` (-32603) | yes, `Warning` level |
| `ConnectionRefused` (-32003) | unchanged | `InternalError` (-32603) | yes, `Warning` level |
| `TlsHandshakeFailed` (-32004) | unchanged | `InternalError` (-32603) | yes, `Warning` level |

Evidence: `src/protocol/WireCodec.cpp:271-283` (the modern codec's `EncodeErrorCode`) and `src/protocol/WireCodec.cpp:158-160` (the legacy codec's counterpart returns the code unchanged).

### Codes that pass through unchanged

-32020, -32021, -32022 and -32042 are not in the `switch`, so both eras emit them as-is; the remaining internal codes (-32000, -32002, -32005, -32006, -32007, -32008, -32009, -32800) pass through too. A modern-era caller therefore cannot tell timeout from connection refused from a TLS failure by response code alone, only by the message and the peer's warning log.

## Three ways an error is delivered

| Shape | Signature | How the error appears | Evidence |
|---|---|---|---|
| Handle | `RequestHandle SendRequest(std::string_view method, JsonValue params, const RequestOptions& options)` | `future<RequestOutcome>` **never throws**; the error is the `ErrorData` branch | `include/mcp/client/McpClient.hpp:130-133`、`include/mcp/client/RequestHandle.hpp:14-25` |
| Raw | `future<JsonValue> SendRequestRaw(...)`; the compatibility overload `SendRequest(..., meta, timeout)` | The error is serialized into a **normal return value** `{"code","message"[,"data"]}` | `McpClient.hpp:134-143`、`src/client/McpClient.cpp:1884-1903` |
| Typed wrapper | `CallTool`/`ListTools`/`GetTask`/`Ping` and friends | Internal `DoSendRequest` sees `code < 0` and does `throw McpError(code, message)` | `src/client/McpClient.cpp:74-89`、`:61-71` |

### `RequestOutcome`

`using RequestOutcome = std::variant<JsonValue, ErrorData>;` (`include/mcp/protocol/IncomingRequestMeta.hpp:29`), with `OutcomeCallback` next to it (`:30`). Errors produced by the session layer itself (connection closed, timeout, cancellation) build an `ErrorData` directly: `src/protocol/McpSessionHandler.cpp:273-276`, `:335-338`, `:1100-1106`, `:1128-1137`. `DeliverOutcome` hands the same `ErrorData` to the raw callback serialized and to the typed callback untouched (`:96-113`).

### The difference between the two futures

`SendRequest(..., meta, timeout)` merely forwards to `SendRequestRaw` (`src/client/McpClient.cpp:1884-1903`), so their behaviour is identical and only the name differs; the real boundary is the `RequestOptions` overload. An error in the raw future is **not an exception** — check whether `code` in the `get()` result is negative; the `RequestHandle` future needs `std::get_if<ErrorData>`.

::: warning
The typed wrapper restores only `code` and `message`; **`data` is dropped** (`src/client/McpClient.cpp:84-86`). Reading `requiredCapabilities` or `supported` requires the `RequestOutcome` or raw shape.
:::

### Inconsistent handling of unknown peer codes

- The client side `ExtractProtocolError` does no whitelist check: any negative `code` is `static_cast` straight into `McpErrorCode` (`src/client/McpClient.cpp:61-71`).
- The server side (when it acts as a caller, e.g. elicitation) uses `FindResponseErrorCode`, and **a negative code outside the whitelist is silently treated as a successful payload** (`src/server/McpServer.cpp:485-491`、`:1181`、`:1258`).

## Convergence of user handler exceptions

### Server tool handlers

| What the handler throws | Outcome visible to the peer | Code preserved | Evidence |
|---|---|---|---|
| `McpError` (sync, non-task path) | `CallToolResult.isError = true` with `content[0].text = e.ErrorMessage()` | **dropped** (together with `data`) | `src/server/McpServer.cpp:2633-2636` |
| Any other `std::exception` | Also `isError = true`, text generalized to `Error executing tool <name>`; the original text goes to the `Error` log only | dropped | `:2637-2643`、`:549-551` |
| `...` unknown exception | Same as above | dropped | `:2644-2650` |
| Arguments violating `inputSchema` | `isError = true`, text `invalid arguments: <reason>` | no code at all | `:2617-2623`、`:553` |
| Result violating `outputSchema` | JSON-RPC error `InvalidParams` | preserved | `:2675-2679` |

::: danger
A `McpError` thrown by a tool handler **never** becomes a JSON-RPC error response; the peer only sees an `isError` text. To let the peer read a code you must rely on SDK-issued errors (capability checks, `InvalidParams`, and so on).
:::

### Protocol, resource and prompt handlers

Non-tool paths propagate `McpError` untouched through `promise.set_exception(std::current_exception())` (`src/server/McpServer.cpp:2815`、`:2911`; client side `src/client/McpClient.cpp:747`), and `McpSessionHandler::DeliverResponse` encodes it as a JSON-RPC error: code through the era remapping, `message` from `e.what()`, `data` preserved (`src/protocol/McpSessionHandler.cpp:576-585`). Any non-`McpError` exception converges to `InternalError` plus the fixed text `"internal error"`, with the original `e.what()` going to the log only (`:587-610`). When a handler throws **synchronously** instead of using the promise: `McpError` → `reject(e.Code(), e.what(), e.Data())` (`:491-497`), any other exception → `InternalError` plus `request handler failed` (`:499-513`、`:116-117`).

### Task-backed calls

Tool calls suspended as a task **keep the code**: `McpError` → failure payload `{"code","message"[,"data"]}` (`src/server/McpServer.cpp:2523-2530`; first round of a required task `:2372-2376`), any other exception → `InternalError` plus the generalized text (`:2531-2551`). The **resume** branch differs: `e.what()` of a `std::exception` is written verbatim into the failure payload (`:1725-1736`), and only `...` falls back to the fixed text (`:1737-1745`).

## Symptom → code → likely cause → how to locate it

| Symptom | Code | Likely cause | How to locate it |
|---|---|---|---|
| Tool failed but the peer only sees text | none (`isError`) | Handler exception converged into an `isError` result | Set `MCP_LOG_LEVEL=4` and look for `tool '<name>' failed` |
| Timeout, refused connection and TLS all become internal error in the modern era | -32603 (was -32001/-32003/-32004) | Era remapping | Search for the `internal error code remapped` warning |
| Repeated re-probing or immediate failure after the handshake | -32022 | Declared version not in `supported` | Read `data.supported`/`requested`, see [Protocol versions](/en/learn/protocol-versions) |
| Every HTTP request answered 400 | -32600/-32020 | Envelope missing `id`, missing `Mcp-Method`/`Mcp-Name` header | Status mapping at `src/http/StreamableHttpServerTransport.cpp:116-129` |
| 404 for a method name that is correct | -32601 | Method not in the negotiated era, or extension disabled | `WireValidation::NotInEra` branch at `src/protocol/McpSessionHandler.cpp:414` |
| First request after an idle period fails, retry succeeds | -32009 | Stateful session was reclaimed | `reinit_on_expired_session` (default true), `include/mcp/client/ClientOptions.hpp:67` |
| Long task stream drops mid-flight | -32000 | Read hit the end or the peer closed | Distinguish timeout from EOF with `IsEof()`, see [Transports reference](/en/reference/transports) |
| No result after a cancellation | -32800 | Peer `notifications/cancelled` or local `Cancel` | Check `data.reason`; `src/protocol/McpSessionHandler.cpp:1100` |
| List pagination never finishes | -32005 | Peer `nextCursor` does not converge (>64 pages) | `src/client/McpClient.cpp:169`、`:1392` |
| Connecting to a TLS endpoint always fails | -32004 | OpenSSL not compiled in (`TLS support not compiled in`) | Check `MCP_HAVE_OPENSSL`, see [Installation](/en/guide/installation) |
| MRTR fails after several rounds | -32007 or -32603 | Handler returned an illegal action / `max_rounds` exceeded | `src/client/McpClient.cpp:290`、`:1133` |

## Source files behind this page

- `include/mcp/ErrorCodes.hpp`、`include/mcp/McpError.hpp`、`include/mcp/JsonRpc.hpp`
- `include/mcp/protocol/IncomingRequestMeta.hpp`、`include/mcp/client/RequestHandle.hpp`、`include/mcp/client/McpClient.hpp`、`include/mcp/client/ClientOptions.hpp`
- Protocol layer: `src/protocol/WireCodec.cpp`、`src/protocol/McpSessionHandler.cpp`
- Server and client: `src/server/McpServer.cpp`、`src/client/McpClient.cpp`
- Core: `src/core/JsonParser.cpp`、`src/core/JsonRpc.cpp`、`src/core/JsonValue.cpp`、`src/core/Content.cpp`
- Transport and networking: `src/http/StreamableHttpServerTransport.cpp`、`src/http/StreamableHttpClientTransport.cpp`、`src/transport/detail/net/HttpClient.cpp`、`src/transport/detail/net/TlsSocket.cpp`、`src/transport/detail/net/TcpSocketPosix.cpp`、`src/transport/detail/net/TcpSocketWin32.cpp`
- Other throw sites: `src/detail/AtomicJsonFile.cpp`、`src/ext/DPoP.cpp`
