# Transports Reference

This page is the **authoritative field table** for the transport layer and the HTTP server substrate; every field, type, and default is checked against the headers. For the conceptual narrative and trade-offs see [Transport concepts](/en/learn/transports); for facade signatures see [Server reference](/en/reference/server) and [Client reference](/en/reference/client); for persistence and processes see [Process and storage reference](/en/reference/process-and-storage). No procedural steps on this page.

## Transport contract

### `ITransport`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `SessionId` | `virtual std::string_view SessionId() const = 0` | — | Session identifier; stateless implementations return an empty string |
| `GetMessageChannel` | `virtual MessageChannel& GetMessageChannel() = 0` | — | Reference to the inbound message queue |
| `SendMessageAsync` | `virtual void SendMessageAsync(JsonRpcMessage message) = 0` | — | Takes the outbound message by value; non-blocking semantics are up to the implementation |
| `BufferRequestStreamNotification` | `virtual bool BufferRequestStreamNotification(JsonRpcMessage)` | `false` | Buffers a notification into the current request stream; returns whether it was absorbed |
| `RequiresRequestScopedNotifications` | `virtual bool RequiresRequestScopedNotifications() const` | `false` | Whether notifications must be delivered per request scope |
| `Close` | `virtual void Close() = 0` | — | Idempotent; when called from an IO-thread callback it must go through `detail::JoinThreadSafely` |
| `IsStateless` | `virtual bool IsStateless() const` | `false` | Session-semantics decision point |
| `Start` | `virtual void Start()` | empty body | Explicitly starts the reader thread after connecting |

### `TransportState` and `TransportBase`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `TransportState` | `enum class TransportState { Initial, Connected, Disconnected }` | `Initial` | Three-state machine backed by `std::atomic<int>` |
| `TransportBase` | `class TransportBase : public ITransport, public std::enable_shared_from_this<TransportBase>` | — | Common base of every session-side transport; owns `channel_`/`session_id_` |
| `SetConnected` | `void SetConnected()` | — | Only `Initial → Connected` succeeds (CAS); otherwise a Warning is logged |
| `SetDisconnected` | `void SetDisconnected()` | — | Repeat calls fire `on_close_` only once |
| `GetState` | `TransportState GetState() const` | `Initial` | Atomic read of the current state |
| `SetOnClose` / `SetOnError` | `void SetOnClose(std::function<void()>)` / `void SetOnError(std::function<void(std::string_view)>)` | unset | Close and error callbacks, written under a lock; read-loop failures surface via `SetOnError` |

### `IClientTransport`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `Name` | `virtual std::string_view Name() const = 0` | — | Transport name; each implementation falls back to a built-in default when empty |
| `Connect` | `virtual std::shared_ptr<ITransport> Connect() = 0` | — | Connection factory returning the session object (not a `unique_ptr`) |

The three interfaces above are all declared in `include/mcp/Transport.hpp`.

### `MessageChannel`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| Constructor | `explicit MessageChannel(size_t max_buffer = 64)` | `64` | Transport bases actually pass `detail::kChannelCapacity` |
| `AsyncReceive` | `template <typename Callback> void AsyncReceive(Callback&& cb)` | — | Blocks for a message; after close reports `operation_canceled` |
| `Send` | `bool Send(JsonRpcMessage message)` | — | Blocks while the queue is full |
| `TrySend` | `bool TrySend(JsonRpcMessage message)` | — | Returns `false` immediately when full or closed |
| `Close` / `IsOpen` / `Empty` | `void Close()` / `bool IsOpen() const` / `bool Empty() const` | open | Closing wakes both condition variables |

Declared in `include/mcp/protocol/MessageChannel.hpp`.

## stdio

### `StdioServerTransport`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| Constructors | `StdioServerTransport()` / `explicit StdioServerTransport(std::optional<std::size_t> max_frame_bytes)` | `detail::kMaxStdioFrameBytes` (8 MiB) | The process' own stdin/stdout are the pipe; there is no Options struct |
| `Start` / `Close` / `SendMessageAsync` | `override` | — | `Start()` spawns the reader thread; `SendMessageAsync` writes one JSON line |

### `StdioClientTransport`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| Constructor | `explicit StdioClientTransport(const StdioClientTransportOptions& options)` | — | Stores configuration only; no child process yet |
| `Name` | `std::string_view Name() const override` | `"stdio"` | Fallback when `options.name` is empty |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | Spawns the child and returns the session transport; option conflicts are validated here |
| `Pid` | `std::optional<int64_t> Pid() const` | `std::nullopt` | Only holds a value after a successful `Connect()` |

### `StdioClientTransportOptions`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `command` | `std::string` | empty (required) | Child executable |
| `arguments` | `std::vector<std::string>` | `{}` | Command-line arguments |
| `name` | `std::string` | `""` | `Name()` falls back to `"stdio"` when empty |
| `working_directory` | `std::string` | `""` | Child working directory |
| `inherit_environment_variables` | `bool` | `true` | Inherit the parent environment |
| `environment_variables` | `std::map<std::string, std::string>` | `{}` | Additional environment variables |
| `stderr_path` | `std::string` | `""` | File path the child's stderr is written to |
| `on_stderr` | `std::function<void(std::string_view)>` | unset | Captures stderr into a callback on a dedicated reader thread |
| `shutdown_timeout` | `std::chrono::milliseconds` | `{2000}` | SIGTERM→SIGKILL grace window on close, passed to `ProcessHandle::Terminate` |
| `terminate_process_tree` | `bool` | `false` | Terminates the whole process tree (POSIX process group / Windows job object) |
| `max_frame_bytes` | `std::optional<std::size_t>` | `detail::kMaxStdioFrameBytes` (8 MiB) | Per-frame cap; violations report `stdio frame size exceeds maximum allowed size` via `SetOnError` |

::: warning
`stderr_path` and `on_stderr` are mutually exclusive: setting both makes `Connect()` throw `McpError(McpErrorCode::InvalidParams, "stderr_path and on_stderr are mutually exclusive")`.
:::

The interfaces are declared in `include/mcp/transport/StdioServerTransport.hpp` and `include/mcp/transport/StdioClientTransport.hpp`; the spawning and frame-reading behaviour is in `src/transport/StdioClientTransport.cpp`, and process-tree termination in `src/transport/detail/posix_platform.cpp` and `src/transport/detail/win32_platform.cpp`.

## Streamable HTTP server

### `StreamableHttpServerTransport`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| Constructor | `explicit StreamableHttpServerTransport(StreamableHttpServerOptions options = {})` | default-constructed options | Building it creates the `HttpServer` and registers routes |
| `Start` / `Close` / `SendMessageAsync` | `override` | — | Starts and stops the underlying HTTP listener |
| `BufferRequestStreamNotification` | `bool BufferRequestStreamNotification(JsonRpcMessage) override` | — | Buffers into the held-open request stream |
| `RequiresRequestScopedNotifications` | `bool ... const override` | `true` | Hard-coded true |
| `IsStateless` | `bool IsStateless() const override` | takes `options_.stateless` | Source of the stateless decision |
| `ValidateMcpHeaders` | `static bool ValidateMcpHeaders(const std::string&, const std::string&, const JsonValue&, std::string& error_out)` | — | SEP-2243 header/body consistency check |
| `AuthResult` | `struct { bool ok{false}; std::vector<std::string> scopes; std::optional<std::string> resource; }` | `ok=false` | Return payload of `BearerAuthConfig::verify` |

### `StreamableHttpServerOptions`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `port` | `uint16_t` | `kDefaultPort` = `3001` | Listening port |
| `endpoint` | `std::string` | `"/mcp"` | MCP request path |
| `stateless` | `bool` | `true` | Stateless mode (from 2026-07-28) |
| `enable_legacy_sse` | `bool` | `false` | When off, no GET route is registered: listen stream and event replay are unavailable |
| `listen_hold_open` | `bool` | `false` | On modern versions, lets `subscriptions/listen` hold the POST response stream open and carry the listen stream over it |
| `sse_keep_alive_ms` | `int` | `15000` | SSE comment-frame interval; `0` disables |
| `event_store` | `std::shared_ptr<EventStore>` | `nullptr` | Built-in in-process `EventStore` when null |
| `session_store` | `std::shared_ptr<SessionStore>` | `nullptr` | Session mode only; inject `InMemorySessionStore` etc. for cross-instance adoption |
| `server_name` | `std::string` | `"mcp-server"` | Name used by discovery |
| `server_version` | `std::string` | `kSdkVersion` | Version used by discovery |
| `host` | `std::string` | `""` | Bind address; empty = IPv4 `INADDR_ANY`, accepts IPv4/IPv6 literals, invalid values throw |
| `allowed_hosts` | `std::vector<std::string>` | `{}` | Host allow-list; when empty only localhost/loopback passes, and a missing `Host` header is rejected outright |
| `allowed_origins` | `std::vector<std::string>` | `{}` | Origin allow-list; requests without an `Origin` header pass |
| `max_request_body_bytes` | `std::optional<std::size_t>` | `detail::kMaxHttpBodyBytes` (4 MiB) | Request-body cap |
| `max_sse_event_bytes` | `std::optional<std::size_t>` | `detail::kMaxSseEventBytes` (1 MiB) | Per-SSE-event cap |
| `bearer_auth` | `std::optional<BearerAuthConfig>` | `std::nullopt` | Without it there is no auth gate and behaviour is unchanged |
| `resolve_param_annotations` | `std::function<std::vector<McpParamAnnotationInfo>(const std::string& method, const std::string& name)>` | unset | Resolves `Mcp-Param-*` header annotations per method/tool; when unset parameter-header validation is skipped |
| `enable_server_card` | `bool` | `false` | Registers the `GET /server-card` discovery endpoint |
| `server_card` | `std::optional<ext::ServerCard>` | `std::nullopt` | `BuildDefaultServerCard()` is used when absent |

### `StreamableHttpServerOptions::BearerAuthConfig`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `verify` | `std::function<AuthResult(const std::string& token)>` | unset (required) | Token validation; `ok=false` → 401 `invalid_token` |
| `resource_metadata_url` | `std::string` | `""` | Protected-resource metadata URL, also written into `WWW-Authenticate` |
| `scopes_supported` | `std::vector<std::string>` | `{}` | Scopes advertised in the metadata document |
| `required_scopes` | `std::vector<std::string>` | `{}` | Empty disables scope checks; insufficient → 403 `insufficient_scope` |
| `authorization_servers` | `std::vector<std::string>` | `{}` | Written into the metadata document |
| `serve_metadata_endpoint` | `bool` | `true` | Registers the unauthenticated `GET /.well-known/oauth-protected-resource` |
| `expected_resource` | `std::string` | `""` | Expected audience; empty disables the check |
| `validate_token_resource` | `bool` | `true` | Audience-validation switch |
| `enable_dpop` | `bool` | `false` | DPoP proof validation (experimental, requires OpenSSL) |

The interface and Options are declared in `include/mcp/transport/StreamableHttpServerTransport.hpp`; routing, header validation and the server-card endpoint are in `src/http/StreamableHttpServerTransport.cpp`, and Host/Origin plus binding behaviour in `src/http/HttpServerImpl.cpp`.

## Streamable HTTP client

### `StreamableHttpClientTransport`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| Constructor | `explicit StreamableHttpClientTransport(const HttpClientTransportOptions& options)` | — | Stores configuration only |
| `Name` | `std::string_view Name() const override` | `"streamable-http"` | Fallback when `name` is empty |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | Returns the session transport |

### `HttpClientTransportOptions`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `endpoint` | `std::string` | empty (required) | Server URL; `https` uses TLS |
| `transport_mode` | `HttpTransportMode` | `AutoDetect` | Declared-only field: nothing in `src/` reads it; actual probing happens in `McpClient` version negotiation |
| `name` | `std::string` | `""` | Falls back to `"streamable-http"` |
| `known_session_id` | `std::string` | `""` | Resumes an existing session |
| `additional_headers` | `std::map<std::string, std::string>` | `{}` | Static extra headers |
| `auth_challenge_handler` | `std::function<std::string(std::string_view www_authenticate)>` | unset | Always called on 401, on 403 only with `insufficient_scope`; a non-empty return retries exactly once |
| `apply_per_request_headers` | `std::function<void(const HttpRequestHeaderContext&, std::map<std::string, std::string>&)>` | unset | Invoked before every send, writing into the mutable `headers` |
| `observe_response_challenge` | `std::function<void(const HttpResponseChallengeContext&)>` | unset | Invoked once per response (2xx and SSE included) |
| `enable_listen_stream` | `std::optional<bool>` | unset → `!stateless` | GET listen-stream switch |
| `stateless` | `bool` | `false` | Purely stateless peers: no `Mcp-Session-Id`/`Last-Event-ID`, no DELETE |
| `max_sse_event_bytes` | `std::optional<std::size_t>` | `detail::kMaxSseEventBytes` (1 MiB) | Per-event cap |
| `redirect_policy` | `detail::HttpRedirectPolicy` | `SameOriginOnly` | Redirect policy (the enum lives in namespace `mcp::detail`) |
| `max_redirects` | `int` | `detail::kMaxHttpRedirects` = `5` | Maximum hop count |

### Request purposes and callback contexts

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `HttpRequestPurpose` | `enum class { MessagePost, ListenStream, ResumeStream, SessionDelete, ToolAnnotationsRefresh }` | `MessagePost` | Distinguishes the five outgoing request kinds |
| `HttpRequestHeaderContext` | `struct { std::string_view http_method; std::string_view url; HttpRequestPurpose purpose; std::optional<RequestId> request_id; int attempt_index; }` | `attempt_index = 0` | Context handed to the per-request header callback |
| `HttpResponseChallengeContext` | `struct { int status_code; std::string_view www_authenticate; std::string_view dpop_nonce; }` | `status_code = 0` | Argument of the challenge-observation callback |

### `HttpTransportMode`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `AutoDetect` / `StreamableHttp` / `Sse` | `enum class HttpTransportMode` | `AutoDetect` | Value type of `transport_mode`, declared in `include/mcp/transport/HttpTransportMode.hpp` |

::: info
`transport_mode` and `HttpTransportMode` currently appear only in public headers and `tests/unit/StreamableHttpTransportTests.cpp`; no production code path consumes them, so do not infer connection behaviour from them.
:::

The interface and Options are declared in `include/mcp/transport/StreamableHttpClientTransport.hpp`; POST, listen stream and replay are in `src/http/StreamableHttpClientTransport.cpp`, and the redirect constants in `include/mcp/transport/detail/Limits.hpp`.

## SSE and WebSocket clients

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `SseClientTransport` | `class MCP_API SseClientTransport : public IClientTransport` | — | Legacy SSE client transport |
| Constructor | `explicit SseClientTransport(std::string_view server_url, std::string_view name = {})` / `explicit SseClientTransport(SseClientTransportOptions options)` | `name` empty → `"sse"` | Both overloads normalise an empty name to `"sse"` |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | Establishes the long-lived SSE connection |
| `SseClientTransportOptions::server_url` | `std::string` | empty (required) | SSE endpoint |
| `SseClientTransportOptions::name` | `std::string` | `"sse"` | The only name field with an in-struct default |
| `max_sse_event_bytes` | `std::optional<std::size_t>` | `detail::kMaxSseEventBytes` | Per-event cap |
| `redirect_policy` | `detail::HttpRedirectPolicy` | `SameOriginOnly` | Redirect policy |
| `max_redirects` | `int` | `5` | Maximum hop count |
| `WebSocketClientTransport` | `class WebSocketClientTransport : public IClientTransport` | — | WebSocket client transport; **there is no Options struct**, only two constructor arguments |
| Constructor | `WebSocketClientTransport(std::string url, std::string name = "websocket")` | `name = "websocket"` | `Name()` returns the constructed name verbatim, no fallback logic |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | Accepts `ws`/`wss` only; `wss` uses TLS; 30s connect timeout |

The interfaces are declared in `include/mcp/transport/SseClientTransport.hpp` and `include/mcp/transport/WebSocketClientTransport.hpp`; the implementations are `src/transport/SseClientTransport.cpp` and `src/transport/detail/net/WebSocketClient.cpp`.

## In-process transport

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `InMemoryTransport` | `class MCP_API InMemoryTransport` | — | The class body holds only the nested `Pair` and the static `CreatePair()`, with no instance members: use it as a namespace |
| `InMemoryTransport::Pair` | `struct { std::shared_ptr<ITransport> client; std::shared_ptr<ITransport> server; }` | — | **Data carrier for the two endpoint handles**; it does not build the channels itself |
| `CreatePair` | `static Pair CreatePair()` | — | **Factory function**: cross-links two `MessageChannel`s and returns the ready `Pair`; the implementation type `InMemoryTransportImpl` lives in an anonymous namespace and is not public API |

::: tip
One-line difference: `Pair` is only the return type (not a factory, and it cannot create channels by itself); the single entry point is the static `InMemoryTransport::CreatePair()`.
:::

The interface is declared in `include/mcp/transport/InMemoryTransport.hpp`; the channel cross-linking is in `src/transport/InMemoryTransport.cpp`.

## Request cancellation

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `IRequestCanceller` | `class MCP_API IRequestCanceller` | — | Standalone interface; it does not touch the `ITransport` vtable |
| `CancelRequest` | `virtual void CancelRequest(const RequestId& id, std::string_view reason) = 0` | — | Modern Streamable HTTP uses it to close the request-scoped stream |

Declared in `include/mcp/transport/IRequestCanceller.hpp`.

## HTTP server substrate

`HttpServer`/`EventStore`/`SessionStore` are the substrate of the Streamable HTTP server and can also be used standalone; `FileEventStore`, `FileTaskStore` and `Process` are covered by the [Process and storage reference](/en/reference/process-and-storage).

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `HttpRequest` | `struct { std::string method; std::string path; std::unordered_map<std::string, std::string> headers; std::string body; }` | — | Inbound request view |
| `HttpResponse` | `struct` with `status_code`/`status_text`/`headers`/`body`/`is_sse`/`sse_close_after_write`/`sse_client_tag` | `200`, `"OK"`, rest empty/`false` | The last two fields control directed SSE writes and close-after-write |
| `HttpHandler` | `std::function<void(const HttpRequest&, HttpResponse&)>` | — | Route body |
| `HttpServerOptions` | `struct` with `on_request`/`on_connect`/`on_disconnect`/`allowed_hosts`/`allowed_origins`/`sse_keep_alive_ms`/`bind_host`/`max_request_body_bytes` | `0`, `""`, `kMaxHttpBodyBytes` | The transport forwards `host`/`allowed_*` here unchanged |
| `HttpServer` | `HttpServer(uint16_t port, const HttpServerOptions& options = {})`, plus `void Start()` / `void Stop()` | default-constructed options | Not an `ITransport`; independent lifecycle |
| `SetHandler` | `void SetHandler(std::string_view method, std::string_view path, HttpHandler handler)` | — | Exact `(method, path)` match |
| `AddSseClient` / `RemoveSseClient` / `SseClientCount` | `SseClientId AddSseClient(send_fn[, std::string tag])` / `void RemoveSseClient(SseClientId)` / `std::size_t SseClientCount() const` | no tag | `SseClientId = uint64_t`; the tagged overload enables directed broadcast and filtering |
| `BroadcastSse` | `void BroadcastSse(std::string_view event[, const std::function<bool(const std::string&)>& accept])` | all clients | Predicate form filters by tag |

The interface is declared in `include/mcp/http/HttpServer.hpp`; `src/http/HttpServer.cpp` forwards, and `src/http/HttpServerImpl.cpp` does the scheduling.

### `EventStore`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `Append` | `virtual uint64_t Append(std::string_view session_id, std::string event_data)` | — | Returns a monotonically increasing event id (starting at `1`) |
| `GetEventsSince` | `virtual std::vector<std::pair<uint64_t, std::string>> GetEventsSince(std::string_view, uint64_t) const` | — | Replays events after `Last-Event-ID` |
| `Clear` | `virtual void Clear(std::string_view session_id)` | — | Reclaims memory when a session ends |
| `kMaxEventsPerSession` | `static constexpr size_t` | `10000` | Per-session ring cap |

Declared in `include/mcp/http/EventStore.hpp`.

### `SessionStore` and `InMemorySessionStore`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `SessionRecord` | `struct { std::string protocol_version; int64_t created_at_ms{0}; int64_t last_seen_ms{0}; }` | `0` | Minimal payload for cross-process adoption |
| `Save` / `Load` / `Remove` | pure virtual `void` / `std::optional<SessionRecord>` / `void` | — | The three mandatory operations |
| `TrySave` | `virtual bool TrySave(const std::string&, const SessionRecord&)` | delegates to `Save`, returns `true` | Non-throwing path when the store is full |
| `Size` / `SweepExpired` | `virtual size_t() const` / `virtual size_t()` | `0` | Optional; the base returns `0` |
| `InMemorySessionStore` | `class InMemorySessionStore : public SessionStore` | — | In-process implementation; `Save` throws `std::runtime_error("session store is full")` when full |
| Expiry rule | `now - max(last_seen_ms, created_at_ms) > detail::kSessionIdleTimeoutMs` | 30 minutes | `Load` deletes expired entries and returns `nullopt` |

Declared in `include/mcp/http/SessionStore.hpp`; the idle timeout and capacity constants are in `include/mcp/transport/detail/Limits.hpp`.

## Limits and redirect policy

`include/mcp/transport/detail/Limits.hpp` lives in namespace `mcp::detail`: it is referenced directly by public Options, but it is not a stable API — compare against these defaults before changing a value.

| Symbol | Type | Value | Notes |
|---|---|---|---|
| `kMaxHttpBodyBytes` | `size_t` | 4 MiB | Default HTTP request-body cap |
| `kMaxSseEventBytes` | `size_t` | 1 MiB | Default per-SSE-event cap |
| `kMaxStdioFrameBytes` | `size_t` | 8 MiB | Default stdio frame cap |
| `kMaxHttpRedirects` | `int` | `5` | Default redirect hop budget |
| `kChannelCapacity` | `size_t` | `64` | `MessageChannel` depth |
| `kMaxHttpSessions` | `size_t` | `10000` | Session-store cap (full → 503 / `TrySave` failure) |
| `kSessionIdleTimeoutMs` | `int64_t` | `1800000` | Session idle expiry |
| `HttpRedirectPolicy` | `enum class { SameOriginOnly, FollowAll, NoFollow }` | `SameOriginOnly` | Must be spelled `mcp::detail::HttpRedirectPolicy` |

## TLS and OpenSSL (optional)

| Symbol | Form | Default | Notes |
|---|---|---|---|
| `MCP_WITH_OPENSSL` | CMake cache entry `AUTO\|ON\|OFF` | `AUTO` | `AUTO` follows `find_package`; `ON` fails configure when missing; `OFF` force-disables |
| `MCP_HAVE_OPENSSL` | compile-time macro | undefined = no TLS | Applied uniformly by the INTERFACE target `mcp-openssl` to `mcp-transport`/`mcp-client`/`mcp-ext` |
| `MCP_OPENSSL_EXPECTED_MAJOR` | CMake cache entry | `""` | CI pin of the major version; supported majors are 3.x and 4.x, anything else fails configure |
| `https`/`wss` connections | runtime behaviour | — | Without OpenSSL the SDK throws `McpError(TlsHandshakeFailed, "TLS support not compiled in")` |
| PKCE randomness | compile-time fallback | — | `RAND_bytes` with OpenSSL, `BCryptGenRandom` on Windows otherwise, `random_device` elsewhere; SHA-256 is always the built-in implementation |
| TLS parameters | internal implementation, not public API | TLS 1.2 minimum | System trust-store verification; `TlsSocket`/`TcpSocket`/`HttpClient`/`WebSocketClient` all live under `src/transport/detail/net/` and must not be referenced as user API |

The cache entries and detection are in `cmake/FetchDependencies.cmake`, `cmake/OpenSslVersion.cmake` and `CMakeLists.txt`; the runtime handshake is in `src/transport/detail/net/TlsSocket.cpp` and the PKCE randomness fallback in `src/client/auth/OAuthClientProvider.cpp`.

## stateful vs stateless

| Scenario | `IsStateless()` | Session and notifications |
|---|---|---|
| stdio / InMemory / SSE / WebSocket | `false` (`TransportBase` is always `false`) | One long-lived session, notifications share the channel |
| Streamable HTTP server (default `stateless = true`) | `true` | Each request stands alone with a synchronous JSON response (deadline `kStatelessTimeout` = 30s); the zero-listener wait is skipped |
| Streamable HTTP session mode (`stateless = false`) | `false` | Sends `Mcp-Session-Id`, supports the GET listen stream and `EventStore` replay |

The decision entry points are `include/mcp/Transport.hpp` and `include/mcp/transport/StreamableHttpServerTransport.hpp`; the stateless branch is implemented in `src/http/StreamableHttpServerTransport.cpp`.

::: tip
For the conceptual narrative, mode selection, and negotiation details see [Transport concepts](/en/learn/transports); the corrective retry rules for version negotiation are in [Protocol versions](/en/learn/protocol-versions).
:::

## Source files behind this page

- Contract: `include/mcp/Transport.hpp`, `include/mcp/protocol/MessageChannel.hpp`, `include/mcp/transport/IRequestCanceller.hpp`
- stdio: `include/mcp/transport/StdioServerTransport.hpp`, `include/mcp/transport/StdioClientTransport.hpp`
- Streamable HTTP: `include/mcp/transport/StreamableHttpServerTransport.hpp`, `include/mcp/transport/StreamableHttpClientTransport.hpp`, `include/mcp/transport/HttpTransportMode.hpp`
- Other clients and in-process: `include/mcp/transport/SseClientTransport.hpp`, `include/mcp/transport/WebSocketClientTransport.hpp`, `include/mcp/transport/InMemoryTransport.hpp`
- HTTP substrate and limits: `include/mcp/http/HttpServer.hpp`, `include/mcp/http/EventStore.hpp`, `include/mcp/http/SessionStore.hpp`, `include/mcp/transport/detail/Limits.hpp` (the last one is referenced by public Options and is not a stable API)
- Behaviour evidence: `src/transport/StdioClientTransport.cpp`, `src/http/StreamableHttpServerTransport.cpp`, `src/http/StreamableHttpClientTransport.cpp`, `src/http/HttpServerImpl.cpp`, `src/transport/detail/net/TlsSocket.cpp`, `cmake/FetchDependencies.cmake`, `cmake/OpenSslVersion.cmake`, `CMakeLists.txt`; task-shaped guides: [/en/howto/run-a-streamable-http-server](/en/howto/run-a-streamable-http-server), [/en/howto/connect-to-a-server](/en/howto/connect-to-a-server), [/en/howto/manage-subprocesses](/en/howto/manage-subprocesses)
