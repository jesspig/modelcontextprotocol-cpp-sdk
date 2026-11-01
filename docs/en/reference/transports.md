# Transports Reference

This page is the **authoritative field table** for the transport layer and the HTTP server substrate; every field, type, and default is checked against the headers. For the conceptual narrative and trade-offs see [Transport concepts](/en/learn/transports); for facade signatures see [Server reference](/en/reference/server) and [Client reference](/en/reference/client); for persistence and processes see [Process and storage reference](/en/reference/process-and-storage). No procedural steps on this page.

## Transport contract

### `ITransport`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `SessionId` | `virtual std::string_view SessionId() const = 0` | — | Session identifier; stateless implementations return an empty string | `include/mcp/Transport.hpp:21` |
| `GetMessageChannel` | `virtual MessageChannel& GetMessageChannel() = 0` | — | Reference to the inbound message queue | `Transport.hpp:22` |
| `SendMessageAsync` | `virtual void SendMessageAsync(JsonRpcMessage message) = 0` | — | Takes the outbound message by value; non-blocking semantics are up to the implementation | `Transport.hpp:23` |
| `BufferRequestStreamNotification` | `virtual bool BufferRequestStreamNotification(JsonRpcMessage)` | `false` | Buffers a notification into the current request stream; returns whether it was absorbed | `Transport.hpp:24` |
| `RequiresRequestScopedNotifications` | `virtual bool RequiresRequestScopedNotifications() const` | `false` | Whether notifications must be delivered per request scope | `Transport.hpp:25` |
| `Close` | `virtual void Close() = 0` | — | Idempotent; when called from an IO-thread callback it must go through `detail::JoinThreadSafely` | `Transport.hpp:26` |
| `IsStateless` | `virtual bool IsStateless() const` | `false` | Session-semantics decision point | `Transport.hpp:27` |
| `Start` | `virtual void Start()` | empty body | Explicitly starts the reader thread after connecting | `Transport.hpp:28` |

### `TransportState` and `TransportBase`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `TransportState` | `enum class TransportState { Initial, Connected, Disconnected }` | `Initial` | Three-state machine backed by `std::atomic<int>` | `Transport.hpp:31`, `62` |
| `TransportBase` | `class TransportBase : public ITransport, public std::enable_shared_from_this<TransportBase>` | — | Common base of every session-side transport; owns `channel_`/`session_id_` | `Transport.hpp:33`, `60-61` |
| `SetConnected` | `void SetConnected()` | — | Only `Initial → Connected` succeeds (CAS); otherwise a Warning is logged | `Transport.hpp:42`, `80-86` |
| `SetDisconnected` | `void SetDisconnected()` | — | Repeat calls fire `on_close_` only once | `Transport.hpp:43`, `88-93` |
| `GetState` | `TransportState GetState() const` | `Initial` | Atomic read of the current state | `Transport.hpp:44` |
| `SetOnClose` / `SetOnError` | `void SetOnClose(std::function<void()>)` / `void SetOnError(std::function<void(std::string_view)>)` | unset | Close and error callbacks, written under a lock; read-loop failures surface via `SetOnError` | `Transport.hpp:46-53`, `58`, `111-114` |

### `IClientTransport`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `Name` | `virtual std::string_view Name() const = 0` | — | Transport name; each implementation falls back to a built-in default when empty | `Transport.hpp:71` |
| `Connect` | `virtual std::shared_ptr<ITransport> Connect() = 0` | — | Connection factory returning the session object (not a `unique_ptr`) | `Transport.hpp:72` |

### `MessageChannel`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| Constructor | `explicit MessageChannel(size_t max_buffer = 64)` | `64` | Transport bases actually pass `detail::kChannelCapacity` | `include/mcp/protocol/MessageChannel.hpp:18`, `Transport.hpp:76` |
| `AsyncReceive` | `template <typename Callback> void AsyncReceive(Callback&& cb)` | — | Blocks for a message; after close reports `operation_canceled` | `MessageChannel.hpp:21-38` |
| `Send` | `bool Send(JsonRpcMessage message)` | — | Blocks while the queue is full | `MessageChannel.hpp:40` |
| `TrySend` | `bool TrySend(JsonRpcMessage message)` | — | Returns `false` immediately when full or closed | `MessageChannel.hpp:51` |
| `Close` / `IsOpen` / `Empty` | `void Close()` / `bool IsOpen() const` / `bool Empty() const` | open | Closing wakes both condition variables | `MessageChannel.hpp:61`, `70`, `72` |

## stdio

### `StdioServerTransport`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| Constructors | `StdioServerTransport()` / `explicit StdioServerTransport(std::optional<std::size_t> max_frame_bytes)` | `detail::kMaxStdioFrameBytes` (8 MiB) | The process' own stdin/stdout are the pipe; there is no Options struct | `include/mcp/transport/StdioServerTransport.hpp:18-19`, `33` |
| `Start` / `Close` / `SendMessageAsync` | `override` | — | `Start()` spawns the reader thread; `SendMessageAsync` writes one JSON line | `StdioServerTransport.hpp:22-24` |

### `StdioClientTransport`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| Constructor | `explicit StdioClientTransport(const StdioClientTransportOptions& options)` | — | Stores configuration only; no child process yet | `include/mcp/transport/StdioClientTransport.hpp:33` |
| `Name` | `std::string_view Name() const override` | `"stdio"` | Fallback when `options.name` is empty | `StdioClientTransport.hpp:36`; `src/transport/StdioClientTransport.cpp:186` |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | Spawns the child and returns the session transport; option conflicts are validated here | `StdioClientTransport.hpp:37`; `src/transport/StdioClientTransport.cpp:189-192` |
| `Pid` | `std::optional<int64_t> Pid() const` | `std::nullopt` | Only holds a value after a successful `Connect()` | `StdioClientTransport.hpp:38`, `42` |

### `StdioClientTransportOptions`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `command` | `std::string` | empty (required) | Child executable | `StdioClientTransport.hpp:18` |
| `arguments` | `std::vector<std::string>` | `{}` | Command-line arguments | `StdioClientTransport.hpp:19` |
| `name` | `std::string` | `""` | `Name()` falls back to `"stdio"` when empty | `StdioClientTransport.hpp:20` |
| `working_directory` | `std::string` | `""` | Child working directory | `StdioClientTransport.hpp:21` |
| `inherit_environment_variables` | `bool` | `true` | Inherit the parent environment | `StdioClientTransport.hpp:22` |
| `environment_variables` | `std::map<std::string, std::string>` | `{}` | Additional environment variables | `StdioClientTransport.hpp:23` |
| `stderr_path` | `std::string` | `""` | File path the child's stderr is written to | `StdioClientTransport.hpp:24` |
| `on_stderr` | `std::function<void(std::string_view)>` | unset | Captures stderr into a callback on a dedicated reader thread | `StdioClientTransport.hpp:25`; `src/transport/StdioClientTransport.cpp:203`, `57-61` |
| `shutdown_timeout` | `std::chrono::milliseconds` | `{2000}` | SIGTERM→SIGKILL grace window on close, passed to `ProcessHandle::Terminate` | `StdioClientTransport.hpp:26`; `src/transport/StdioClientTransport.cpp:68-69` |
| `terminate_process_tree` | `bool` | `false` | Terminates the whole process tree (POSIX process group / Windows job object) | `StdioClientTransport.hpp:27`; `src/transport/detail/posix_platform.cpp:350`, `159-161`; `src/transport/detail/win32_platform.cpp:511`, `534-536` |
| `max_frame_bytes` | `std::optional<std::size_t>` | `detail::kMaxStdioFrameBytes` (8 MiB) | Per-frame cap; violations report `stdio frame size exceeds maximum allowed size` via `SetOnError` | `StdioClientTransport.hpp:28`; `src/transport/StdioClientTransport.cpp:138-141`, `151-154` |

::: warning
`stderr_path` and `on_stderr` are mutually exclusive: setting both makes `Connect()` throw `McpError(McpErrorCode::InvalidParams, "stderr_path and on_stderr are mutually exclusive")` (`src/transport/StdioClientTransport.cpp:189-192`).
:::

## Streamable HTTP server

### `StreamableHttpServerTransport`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| Constructor | `explicit StreamableHttpServerTransport(StreamableHttpServerOptions options = {})` | default-constructed options | Building it creates the `HttpServer` and registers routes | `include/mcp/transport/StreamableHttpServerTransport.hpp:89-90` |
| `Start` / `Close` / `SendMessageAsync` | `override` | — | Starts and stops the underlying HTTP listener | `StreamableHttpServerTransport.hpp:94-96` |
| `BufferRequestStreamNotification` | `bool BufferRequestStreamNotification(JsonRpcMessage) override` | — | Buffers into the held-open request stream | `StreamableHttpServerTransport.hpp:97` |
| `RequiresRequestScopedNotifications` | `bool ... const override` | `true` | Hard-coded true | `StreamableHttpServerTransport.hpp:98` |
| `IsStateless` | `bool IsStateless() const override` | takes `options_.stateless` | Source of the stateless decision | `StreamableHttpServerTransport.hpp:99` |
| `ValidateMcpHeaders` | `static bool ValidateMcpHeaders(const std::string&, const std::string&, const JsonValue&, std::string& error_out)` | — | SEP-2243 header/body consistency check | `StreamableHttpServerTransport.hpp:101-105` |
| `AuthResult` | `struct { bool ok{false}; std::vector<std::string> scopes; std::optional<std::string> resource; }` | `ok=false` | Return payload of `BearerAuthConfig::verify` | `StreamableHttpServerTransport.hpp:31-35` |

### `StreamableHttpServerOptions`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `port` | `uint16_t` | `kDefaultPort` = `3001` | Listening port | `StreamableHttpServerTransport.hpp:40`, `29` |
| `endpoint` | `std::string` | `"/mcp"` | MCP request path | `StreamableHttpServerTransport.hpp:41` |
| `stateless` | `bool` | `true` | Stateless mode (from 2026-07-28) | `StreamableHttpServerTransport.hpp:43` |
| `enable_legacy_sse` | `bool` | `false` | When off, no GET route is registered: listen stream and event replay are unavailable | `StreamableHttpServerTransport.hpp:45` |
| `listen_hold_open` | `bool` | `false` | On modern versions, lets `subscriptions/listen` hold the POST response stream open and carry the listen stream over it | `StreamableHttpServerTransport.hpp:47`; `src/http/StreamableHttpServerTransport.cpp:1286-1293` |
| `sse_keep_alive_ms` | `int` | `15000` | SSE comment-frame interval; `0` disables | `StreamableHttpServerTransport.hpp:49` |
| `event_store` | `std::shared_ptr<EventStore>` | `nullptr` | Built-in in-process `EventStore` when null | `StreamableHttpServerTransport.hpp:51` |
| `session_store` | `std::shared_ptr<SessionStore>` | `nullptr` | Session mode only; inject `InMemorySessionStore` etc. for cross-instance adoption | `StreamableHttpServerTransport.hpp:53` |
| `server_name` | `std::string` | `"mcp-server"` | Name used by discovery | `StreamableHttpServerTransport.hpp:55` |
| `server_version` | `std::string` | `kSdkVersion` | Version used by discovery | `StreamableHttpServerTransport.hpp:56` |
| `host` | `std::string` | `""` | Bind address; empty = IPv4 `INADDR_ANY`, accepts IPv4/IPv6 literals, invalid values throw | `StreamableHttpServerTransport.hpp:58`; `src/http/HttpServerImpl.cpp:173-197` |
| `allowed_hosts` | `std::vector<std::string>` | `{}` | Host allow-list; when empty only localhost/loopback passes, and a missing `Host` header is rejected outright | `StreamableHttpServerTransport.hpp:60`; `src/http/HttpServerImpl.cpp:766-785` |
| `allowed_origins` | `std::vector<std::string>` | `{}` | Origin allow-list; requests without an `Origin` header pass | `StreamableHttpServerTransport.hpp:62`; `src/http/HttpServerImpl.cpp:787-797` |
| `max_request_body_bytes` | `std::optional<std::size_t>` | `detail::kMaxHttpBodyBytes` (4 MiB) | Request-body cap | `StreamableHttpServerTransport.hpp:64` |
| `max_sse_event_bytes` | `std::optional<std::size_t>` | `detail::kMaxSseEventBytes` (1 MiB) | Per-SSE-event cap | `StreamableHttpServerTransport.hpp:65` |
| `bearer_auth` | `std::optional<BearerAuthConfig>` | `std::nullopt` | Without it there is no auth gate and behaviour is unchanged | `StreamableHttpServerTransport.hpp:77` |
| `resolve_param_annotations` | `std::function<std::vector<McpParamAnnotationInfo>(const std::string& method, const std::string& name)>` | unset | Resolves `Mcp-Param-*` header annotations per method/tool; when unset parameter-header validation is skipped | `StreamableHttpServerTransport.hpp:79-81`; `src/http/StreamableHttpServerTransport.cpp:1016-1023` |
| `enable_server_card` | `bool` | `false` | Registers the `GET /server-card` discovery endpoint | `StreamableHttpServerTransport.hpp:83`; `src/http/StreamableHttpServerTransport.cpp:564-568` |
| `server_card` | `std::optional<ext::ServerCard>` | `std::nullopt` | `BuildDefaultServerCard()` is used when absent | `StreamableHttpServerTransport.hpp:84`; `src/http/StreamableHttpServerTransport.cpp:751` |

### `StreamableHttpServerOptions::BearerAuthConfig`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `verify` | `std::function<AuthResult(const std::string& token)>` | unset (required) | Token validation; `ok=false` → 401 `invalid_token` | `StreamableHttpServerTransport.hpp:67` |
| `resource_metadata_url` | `std::string` | `""` | Protected-resource metadata URL, also written into `WWW-Authenticate` | `StreamableHttpServerTransport.hpp:68` |
| `scopes_supported` | `std::vector<std::string>` | `{}` | Scopes advertised in the metadata document | `StreamableHttpServerTransport.hpp:69` |
| `required_scopes` | `std::vector<std::string>` | `{}` | Empty disables scope checks; insufficient → 403 `insufficient_scope` | `StreamableHttpServerTransport.hpp:70` |
| `authorization_servers` | `std::vector<std::string>` | `{}` | Written into the metadata document | `StreamableHttpServerTransport.hpp:71` |
| `serve_metadata_endpoint` | `bool` | `true` | Registers the unauthenticated `GET /.well-known/oauth-protected-resource` | `StreamableHttpServerTransport.hpp:72` |
| `expected_resource` | `std::string` | `""` | Expected audience; empty disables the check | `StreamableHttpServerTransport.hpp:73` |
| `validate_token_resource` | `bool` | `true` | Audience-validation switch | `StreamableHttpServerTransport.hpp:74` |
| `enable_dpop` | `bool` | `false` | DPoP proof validation (experimental, requires OpenSSL) | `StreamableHttpServerTransport.hpp:75` |

## Streamable HTTP client

### `StreamableHttpClientTransport`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| Constructor | `explicit StreamableHttpClientTransport(const HttpClientTransportOptions& options)` | — | Stores configuration only | `include/mcp/transport/StreamableHttpClientTransport.hpp:65` |
| `Name` | `std::string_view Name() const override` | `"streamable-http"` | Fallback when `name` is empty | `StreamableHttpClientTransport.hpp:68`; `src/http/StreamableHttpClientTransport.cpp:2279` |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | Returns the session transport | `StreamableHttpClientTransport.hpp:69` |

### `HttpClientTransportOptions`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `endpoint` | `std::string` | empty (required) | Server URL; `https` uses TLS | `StreamableHttpClientTransport.hpp:45` |
| `transport_mode` | `HttpTransportMode` | `AutoDetect` | Declared-only field: nothing in `src/` reads it; actual probing happens in `McpClient` version negotiation | `StreamableHttpClientTransport.hpp:46`; `src/client/McpClient.cpp:490` |
| `name` | `std::string` | `""` | Falls back to `"streamable-http"` | `StreamableHttpClientTransport.hpp:47` |
| `known_session_id` | `std::string` | `""` | Resumes an existing session | `StreamableHttpClientTransport.hpp:48` |
| `additional_headers` | `std::map<std::string, std::string>` | `{}` | Static extra headers | `StreamableHttpClientTransport.hpp:49` |
| `auth_challenge_handler` | `std::function<std::string(std::string_view www_authenticate)>` | unset | Always called on 401, on 403 only with `insufficient_scope`; a non-empty return retries exactly once | `StreamableHttpClientTransport.hpp:50` |
| `apply_per_request_headers` | `std::function<void(const HttpRequestHeaderContext&, std::map<std::string, std::string>&)>` | unset | Invoked before every send, writing into the mutable `headers` | `StreamableHttpClientTransport.hpp:51-53` |
| `observe_response_challenge` | `std::function<void(const HttpResponseChallengeContext&)>` | unset | Invoked once per response (2xx and SSE included) | `StreamableHttpClientTransport.hpp:54-55` |
| `enable_listen_stream` | `std::optional<bool>` | unset → `!stateless` | GET listen-stream switch | `StreamableHttpClientTransport.hpp:56`; `src/http/StreamableHttpClientTransport.cpp:153` |
| `stateless` | `bool` | `false` | Purely stateless peers: no `Mcp-Session-Id`/`Last-Event-ID`, no DELETE | `StreamableHttpClientTransport.hpp:57` |
| `max_sse_event_bytes` | `std::optional<std::size_t>` | `detail::kMaxSseEventBytes` (1 MiB) | Per-event cap | `StreamableHttpClientTransport.hpp:58` |
| `redirect_policy` | `detail::HttpRedirectPolicy` | `SameOriginOnly` | Redirect policy (the enum lives in namespace `mcp::detail`) | `StreamableHttpClientTransport.hpp:59`; `include/mcp/transport/detail/Limits.hpp:8-12` |
| `max_redirects` | `int` | `detail::kMaxHttpRedirects` = `5` | Maximum hop count | `StreamableHttpClientTransport.hpp:60`; `Limits.hpp:20` |

### Request purposes and callback contexts

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `HttpRequestPurpose` | `enum class { MessagePost, ListenStream, ResumeStream, SessionDelete, ToolAnnotationsRefresh }` | `MessagePost` | Distinguishes the five outgoing request kinds | `StreamableHttpClientTransport.hpp:22-28`, `33` |
| `HttpRequestHeaderContext` | `struct { std::string_view http_method; std::string_view url; HttpRequestPurpose purpose; std::optional<RequestId> request_id; int attempt_index; }` | `attempt_index = 0` | Context handed to the per-request header callback | `StreamableHttpClientTransport.hpp:30-36` |
| `HttpResponseChallengeContext` | `struct { int status_code; std::string_view www_authenticate; std::string_view dpop_nonce; }` | `status_code = 0` | Argument of the challenge-observation callback | `StreamableHttpClientTransport.hpp:38-42` |

### `HttpTransportMode`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `AutoDetect` / `StreamableHttp` / `Sse` | `enum class HttpTransportMode` | `AutoDetect` | Value type of `transport_mode`, declared in `include/mcp/transport/HttpTransportMode.hpp:7-11` | `HttpTransportMode.hpp:7-11` |

::: info
`transport_mode` and `HttpTransportMode` currently appear only in public headers and tests (`tests/unit/StreamableHttpTransportTests.cpp:68`, `93`); no production code path consumes them, so do not infer connection behaviour from them.
:::

## SSE and WebSocket clients

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `SseClientTransport` | `class MCP_API SseClientTransport : public IClientTransport` | — | Legacy SSE client transport | `include/mcp/transport/SseClientTransport.hpp:21` |
| Constructor | `explicit SseClientTransport(std::string_view server_url, std::string_view name = {})` / `explicit SseClientTransport(SseClientTransportOptions options)` | `name` empty → `"sse"` | Both overloads normalise an empty name to `"sse"` | `SseClientTransport.hpp:23-24`; `src/transport/SseClientTransport.cpp:369`, `375` |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | Establishes the long-lived SSE connection | `SseClientTransport.hpp:28` |
| `SseClientTransportOptions::server_url` | `std::string` | empty (required) | SSE endpoint | `SseClientTransport.hpp:14` |
| `SseClientTransportOptions::name` | `std::string` | `"sse"` | The only name field with an in-struct default | `SseClientTransport.hpp:15` |
| `max_sse_event_bytes` | `std::optional<std::size_t>` | `detail::kMaxSseEventBytes` | Per-event cap | `SseClientTransport.hpp:16` |
| `redirect_policy` | `detail::HttpRedirectPolicy` | `SameOriginOnly` | Redirect policy | `SseClientTransport.hpp:17` |
| `max_redirects` | `int` | `5` | Maximum hop count | `SseClientTransport.hpp:18` |
| `WebSocketClientTransport` | `class WebSocketClientTransport : public IClientTransport` | — | WebSocket client transport; **there is no Options struct**, only two constructor arguments | `include/mcp/transport/WebSocketClientTransport.hpp:11`, `13` |
| Constructor | `WebSocketClientTransport(std::string url, std::string name = "websocket")` | `name = "websocket"` | `Name()` returns the constructed name verbatim, no fallback logic | `WebSocketClientTransport.hpp:13`, `16` |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | Accepts `ws`/`wss` only; `wss` uses TLS; 30s connect timeout | `WebSocketClientTransport.hpp:17`; `src/transport/detail/net/WebSocketClient.cpp:67-76` |

## In-process transport

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `InMemoryTransport` | `class MCP_API InMemoryTransport` | — | The class body holds only the nested `Pair` and the static `CreatePair()`, with no instance members: use it as a namespace | `include/mcp/transport/InMemoryTransport.hpp:10-18` |
| `InMemoryTransport::Pair` | `struct { std::shared_ptr<ITransport> client; std::shared_ptr<ITransport> server; }` | — | **Data carrier for the two endpoint handles**; it does not build the channels itself | `InMemoryTransport.hpp:12-15` |
| `CreatePair` | `static Pair CreatePair()` | — | **Factory function**: cross-links two `MessageChannel`s and returns the ready `Pair`; the implementation type `InMemoryTransportImpl` lives in an anonymous namespace and is not public API | `InMemoryTransport.hpp:17`; `src/transport/InMemoryTransport.cpp:42-53` |

::: tip
One-line difference: `Pair` is only the return type (not a factory, and it cannot create channels by itself); the single entry point is the static `InMemoryTransport::CreatePair()`.
:::

## Request cancellation

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `IRequestCanceller` | `class MCP_API IRequestCanceller` | — | Standalone interface; it does not touch the `ITransport` vtable | `include/mcp/transport/IRequestCanceller.hpp:10` |
| `CancelRequest` | `virtual void CancelRequest(const RequestId& id, std::string_view reason) = 0` | — | Modern Streamable HTTP uses it to close the request-scoped stream | `IRequestCanceller.hpp:13` |

## HTTP server substrate

`HttpServer`/`EventStore`/`SessionStore` are the substrate of the Streamable HTTP server and can also be used standalone; `FileEventStore`, `FileTaskStore` and `Process` are covered by the [Process and storage reference](/en/reference/process-and-storage).

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `HttpRequest` | `struct { std::string method; std::string path; std::unordered_map<std::string, std::string> headers; std::string body; }` | — | Inbound request view | `include/mcp/http/HttpServer.hpp:20-25` |
| `HttpResponse` | `struct` with `status_code`/`status_text`/`headers`/`body`/`is_sse`/`sse_close_after_write`/`sse_client_tag` | `200`, `"OK"`, rest empty/`false` | The last two fields control directed SSE writes and close-after-write | `HttpServer.hpp:27-35` |
| `HttpHandler` | `std::function<void(const HttpRequest&, HttpResponse&)>` | — | Route body | `HttpServer.hpp:37` |
| `HttpServerOptions` | `struct` with `on_request`/`on_connect`/`on_disconnect`/`allowed_hosts`/`allowed_origins`/`sse_keep_alive_ms`/`bind_host`/`max_request_body_bytes` | `0`, `""`, `kMaxHttpBodyBytes` | The transport forwards `host`/`allowed_*` here unchanged | `HttpServer.hpp:43-56`; `src/http/StreamableHttpServerTransport.cpp:500-507` |
| `HttpServer` | `HttpServer(uint16_t port, const HttpServerOptions& options = {})`, plus `void Start()` / `void Stop()` | default-constructed options | Not an `ITransport`; independent lifecycle | `HttpServer.hpp:61-62`, `65`, `67` |
| `SetHandler` | `void SetHandler(std::string_view method, std::string_view path, HttpHandler handler)` | — | Exact `(method, path)` match | `HttpServer.hpp:69-70` |
| `AddSseClient` / `RemoveSseClient` / `SseClientCount` | `SseClientId AddSseClient(send_fn[, std::string tag])` / `void RemoveSseClient(SseClientId)` / `std::size_t SseClientCount() const` | no tag | `SseClientId = uint64_t`; the tagged overload enables directed broadcast and filtering | `HttpServer.hpp:72-76`, `80` |
| `BroadcastSse` | `void BroadcastSse(std::string_view event[, const std::function<bool(const std::string&)>& accept])` | all clients | Predicate form filters by tag | `HttpServer.hpp:77-79` |

### `EventStore`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `Append` | `virtual uint64_t Append(std::string_view session_id, std::string event_data)` | — | Returns a monotonically increasing event id (starting at `1`) | `include/mcp/http/EventStore.hpp:20`, `37` |
| `GetEventsSince` | `virtual std::vector<std::pair<uint64_t, std::string>> GetEventsSince(std::string_view, uint64_t) const` | — | Replays events after `Last-Event-ID` | `EventStore.hpp:22-23` |
| `Clear` | `virtual void Clear(std::string_view session_id)` | — | Reclaims memory when a session ends | `EventStore.hpp:25` |
| `kMaxEventsPerSession` | `static constexpr size_t` | `10000` | Per-session ring cap | `EventStore.hpp:27` |

### `SessionStore` and `InMemorySessionStore`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `SessionRecord` | `struct { std::string protocol_version; int64_t created_at_ms{0}; int64_t last_seen_ms{0}; }` | `0` | Minimal payload for cross-process adoption | `include/mcp/http/SessionStore.hpp:17-21` |
| `Save` / `Load` / `Remove` | pure virtual `void` / `std::optional<SessionRecord>` / `void` | — | The three mandatory operations | `SessionStore.hpp:27-29` |
| `TrySave` | `virtual bool TrySave(const std::string&, const SessionRecord&)` | delegates to `Save`, returns `true` | Non-throwing path when the store is full | `SessionStore.hpp:31-34` |
| `Size` / `SweepExpired` | `virtual size_t() const` / `virtual size_t()` | `0` | Optional; the base returns `0` | `SessionStore.hpp:36`, `38` |
| `InMemorySessionStore` | `class InMemorySessionStore : public SessionStore` | — | In-process implementation; `Save` throws `std::runtime_error("session store is full")` when full | `SessionStore.hpp:41`, `43-46` |
| Expiry rule | `now - max(last_seen_ms, created_at_ms) > detail::kSessionIdleTimeoutMs` | 30 minutes | `Load` deletes expired entries and returns `nullopt` | `SessionStore.hpp:62-73`, `96-105`; `Limits.hpp:24` |

## Limits and redirect policy

`include/mcp/transport/detail/Limits.hpp` lives in namespace `mcp::detail`: it is referenced directly by public Options, but it is not a stable API — compare against these defaults before changing a value.

| Symbol | Type | Value | Notes | Evidence |
|---|---|---|---|---|
| `kMaxHttpBodyBytes` | `size_t` | 4 MiB | Default HTTP request-body cap | `Limits.hpp:15` |
| `kMaxSseEventBytes` | `size_t` | 1 MiB | Default per-SSE-event cap | `Limits.hpp:17` |
| `kMaxStdioFrameBytes` | `size_t` | 8 MiB | Default stdio frame cap | `Limits.hpp:19` |
| `kMaxHttpRedirects` | `int` | `5` | Default redirect hop budget | `Limits.hpp:20` |
| `kChannelCapacity` | `size_t` | `64` | `MessageChannel` depth | `Limits.hpp:22` |
| `kMaxHttpSessions` | `size_t` | `10000` | Session-store cap (full → 503 / `TrySave` failure) | `Limits.hpp:23` |
| `kSessionIdleTimeoutMs` | `int64_t` | `1800000` | Session idle expiry | `Limits.hpp:24` |
| `HttpRedirectPolicy` | `enum class { SameOriginOnly, FollowAll, NoFollow }` | `SameOriginOnly` | Must be spelled `mcp::detail::HttpRedirectPolicy` | `Limits.hpp:8-12` |

## TLS and OpenSSL (optional)

| Symbol | Form | Default | Notes | Evidence |
|---|---|---|---|---|
| `MCP_WITH_OPENSSL` | CMake cache entry `AUTO|ON|OFF` | `AUTO` | `AUTO` follows `find_package`; `ON` fails configure when missing; `OFF` force-disables | `cmake/FetchDependencies.cmake:7-16`, `27-43` |
| `MCP_HAVE_OPENSSL` | compile-time macro | undefined = no TLS | Applied uniformly by the INTERFACE target `mcp-openssl` to `mcp-transport`/`mcp-client`/`mcp-ext` | `CMakeLists.txt:226`, `229-237` |
| `MCP_OPENSSL_EXPECTED_MAJOR` | CMake cache entry | `""` | CI pin of the major version; supported majors are 3.x and 4.x, anything else fails configure | `FetchDependencies.cmake:11-12`; `cmake/OpenSslVersion.cmake:8-12` |
| `https`/`wss` connections | runtime behaviour | — | Without OpenSSL the SDK throws `McpError(TlsHandshakeFailed, "TLS support not compiled in")` | `src/transport/detail/net/TlsSocket.cpp:279` |
| PKCE randomness | compile-time fallback | — | `RAND_bytes` with OpenSSL, `BCryptGenRandom` on Windows otherwise, `random_device` elsewhere; SHA-256 is always the built-in implementation | `src/client/auth/OAuthClientProvider.cpp:291-301` |
| TLS parameters | internal implementation, not public API | TLS 1.2 minimum | System trust-store verification; `TlsSocket`/`TcpSocket`/`HttpClient`/`WebSocketClient` all live under `src/transport/detail/net/` and must not be referenced as user API | `src/transport/detail/net/TlsSocket.cpp:54-55` |

## stateful vs stateless

| Scenario | `IsStateless()` | Session and notifications | Evidence |
|---|---|---|---|
| stdio / InMemory / SSE / WebSocket | `false` (`TransportBase` is always `false`) | One long-lived session, notifications share the channel | `Transport.hpp:40` |
| Streamable HTTP server (default `stateless = true`) | `true` | Each request stands alone with a synchronous JSON response (deadline `kStatelessTimeout` = 30s); the zero-listener wait is skipped | `StreamableHttpServerTransport.hpp:43`, `99`; `src/http/StreamableHttpServerTransport.cpp:38`, `1263-1272`, `1319`, `1511` |
| Streamable HTTP session mode (`stateless = false`) | `false` | Sends `Mcp-Session-Id`, supports the GET listen stream and `EventStore` replay | `StreamableHttpServerTransport.hpp:43`, `51` |

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
