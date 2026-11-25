# Transports Reference

This page is the **authoritative field table** for the transport layer and the HTTP server substrate; every field, type, and default is checked against the headers. For the conceptual narrative and trade-offs see [Transport concepts](/en/learn/transports); for facade signatures see [Server reference](/en/reference/server) and [Client reference](/en/reference/client); for persistence and processes see [Process and storage reference](/en/reference/process-and-storage). No procedural steps on this page. The 0.3.13 HTTP binding, framing, AutoDetect, and session tightening are in the [0.3.13 migration notes](/en/migration/0.3.13).

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
| `TransportState` | `enum class TransportState { Initial, Connected, Disconnected, Closing }` | `Initial` | Four-state machine (the new `Closing` value is appended, so old enum values are unchanged) backed by `std::atomic<int>` |
| `TransportBase` | `class TransportBase : public ITransport, public std::enable_shared_from_this<TransportBase>` | — | Common base of every session-side transport; owns `channel_`/`session_id_` plus `detail::SessionLifecycle` and a generation |
| `SetConnected` | `void SetConnected()` | — | Only `Initial → Connected` succeeds (CAS); otherwise a Warning is logged; on success the lifecycle advances `Connecting → Open` |
| `SetDisconnected` | `void SetDisconnected()` | — | Repeat calls fire `on_close_` only once; while `Closing`, the close owner fires it after joins |
| `GetState` | `TransportState GetState() const` | `Initial` | Atomic read of the current state |
| `SetOnClose` / `SetOnError` | `void SetOnClose(std::function<void()>)` / `void SetOnError(std::function<void(std::string_view)>)` | unset | Close and error callbacks, written under a lock; read-loop failures surface via `SetOnError` |

### Shutdown order and generation (0.3.13)

All client session transports and the Streamable HTTP server use `detail::SessionLifecycle` (`include/mcp/detail/SessionLifecycle.hpp`) for `Initial → Connecting → Open → Closing → Closed` and generation tracking. One close follows a strict order: enter `Closing` (stop receiving) → close `MessageChannel` to wake waiters → settle pending requests through the existing failure path → close the socket/pipe and interrupt in-flight requests → join with `detail::JoinThreadSafely` (a close from an IO callback detaches instead of self-joining) → fire `on_close_` exactly once.

Callbacks capture the generation at creation time; `Connect` and `Close` are serialized per generation, and message/error/close callbacks from an old generation are dropped once a new generation exists.

::: warning Old behaviour / New behaviour / How to adapt (source compatible, behaviour tightened)
- Old: repeated `Connect()` on one client factory produced independent sessions that kept sending and receiving.
- New: a repeated `Connect()` closes and waits for the previous generation to finish closing before starting the new one; old-generation callbacks no longer deliver messages.
- Affected users: create one transport factory per session when several sessions must coexist; single-session reconnect needs no change.
:::

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

::: warning
POSIX environment and PATH behaviour is tightened (source compatible): overrides used to be appended after the inherited environment (possibly duplicating keys), executable lookup read the parent process `getenv("PATH")`, and a directory with the execute bit could be mistaken for an executable. Now inherited entries are deduplicated by key and overrides take effect with the last value for each key; `inherit_environment_variables=false` never reads the parent environment; PATH comes from the final effective environment, an empty segment means the current directory, and directories or non-executable files are rejected. Without a custom environment the system `execvp` semantics still apply, and `argv[0]` plus the explicit working directory are unchanged. Downstream code relying on duplicate keys or the parent PATH should set `PATH` explicitly in `environment_variables` and keep `inherit_environment_variables=true` for the entries it still needs.
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
| `AuthResult` | `struct { bool ok{false}; std::string principal; std::vector<std::string> scopes; std::optional<std::string> resource; }` | `ok=false`, empty `principal` | Return payload of `BearerAuthConfig::verify`; `principal` is the only trusted server-side identity and is never inferred from params |

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
| `host` | `std::string` | `""` | Bind address; the empty value is forwarded to `bind_host` and from 0.3.13 means IPv4 loopback `127.0.0.1` (previously `INADDR_ANY`); explicit `0.0.0.0`/`::` still binds all interfaces; accepts IPv4/IPv6 literals, invalid values throw |
| `allowed_hosts` | `std::vector<std::string>` | `{}` | Host allow-list; when empty only localhost/loopback passes, and a missing `Host` header is rejected outright |
| `allowed_origins` | `std::vector<std::string>` | `{}` | Origin allow-list; requests without an `Origin` header pass |
| `max_request_body_bytes` | `std::optional<std::size_t>` | `detail::kMaxHttpBodyBytes` (4 MiB) | Request-body cap |
| `max_sse_event_bytes` | `std::optional<std::size_t>` | `detail::kMaxSseEventBytes` (1 MiB) | Per-SSE-event cap |
| `max_sse_replay_bytes` | `std::optional<std::size_t>` | `detail::kMaxSseEventBytes` (1 MiB) | Byte budget when the session-mode GET stream replays from `Last-Event-ID`, truncated from the newest event backwards |
| `request_timeout` | `std::optional<std::chrono::milliseconds>` | `nullopt` → 30s (`kDefaultHttpRequestTimeout`) | Deadline for the POST wait on the handler's response; `0` waits forever, expiry answers 504 |
| `bearer_auth` | `std::optional<BearerAuthConfig>` | `std::nullopt` | Without it there is no auth gate and behaviour is unchanged |
| `on_authenticated` | `ServerAuthenticatedCallback` (`void(const ServerRequestContext&)`) | unset | Invoked after authentication and session resolution; in session mode a second principal on the same session is rejected (403) and the callback is not invoked. Use it to forward the principal to `McpServer::SetSessionPrincipal` |
| `resolve_param_annotations` | `std::function<std::vector<McpParamAnnotationInfo>(const std::string& method, const std::string& name)>` | unset | Resolves `Mcp-Param-*` header annotations per method/tool; when unset parameter-header validation is skipped |
| `enable_server_card` | `bool` | `false` | Registers `GET /server-card` and additionally `GET <endpoint>/server-card` for client discovery |
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

The card response is `application/json` with a strong ETag over the canonical body; `If-None-Match` supports weak comparison, lists and `*`, and a hit returns 304 with no body while preserving ETag, `cache-control` and CORS headers. The card handler no longer duplicates `Access-Control-Allow-Origin`: Host/Origin admission, OPTIONS preflight and `Access-Control-Expose-Headers` (now including `ETag`) are all provided by `HttpServerImpl`; no credentialed CORS is added.

The interface and Options are declared in `include/mcp/transport/StreamableHttpServerTransport.hpp`; routing, header validation and the server-card endpoint are in `src/http/StreamableHttpServerTransport.cpp`, and Host/Origin plus binding behaviour in `src/http/HttpServerImpl.cpp`.

With `bearer_auth` enabled, POST/GET validate the token first, then `ServerRequestContext` carries the session state (`sessionful`/`stateless`), principal and authorized scopes. Binding a second principal to the same session is rejected with 403; missing, expired or revoked sessions return 404. Pending entries are registered under an atomic `(session, owner, request id)` key and removed by the same key on cancellation, timeout, completion and concurrent same-ID requests. DELETE authenticates and verifies the owner before querying the store, testing existence or erasing anything: absent/invalid credentials return 401, while cross-owner and unknown sessions share the same 404 `Session expired` response and do not reveal existence. At `MCP_LOG_LEVEL>=3`, lifecycle records contain only redacted fields `correlation=cid-<hex>` and `session=sid-<hex>` (`session=stateless` in stateless mode), never tokens, authorization headers or bodies. Transports without `bearer_auth` keep the legacy behaviour.

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
| `transport_mode` | `HttpTransportMode` | `AutoDetect` | `AutoDetect` falls back to legacy SSE exactly once when the first `initialize` response is 400/404/405 with a body that is not a valid JSON-RPC error envelope; `StreamableHttp`/`Sse` select an implementation directly and never fall back |
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
`AutoDetect` is consumed by the stable facade returned from `StreamableHttpClientTransport::Connect()`: the facade owns the single `MessageChannel` while inner Streamable HTTP/SSE sessions only perform IO, so replacing the inner session never changes the caller's channel. Only the first `initialize` response is classified. `McpClient` disables transport fallback for `ConnectMode::Pin/Legacy` through `ITransportFallbackControl::SetFallbackEnabled(false)` without changing negotiation semantics.
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
| `additional_headers` | `std::map<std::string, std::string>` | `{}` | Static extra headers on GET and POST |
| `auth_challenge_handler` | `std::function<std::string(std::string_view)>` | unset | Always called on 401, on 403 only with `insufficient_scope`; a non-empty result is used by the next request |
| `apply_per_request_headers` | `std::function<void(const HttpRequestHeaderContext&, std::map<std::string, std::string>&)>` | unset | Invoked before every GET/POST send and may override static headers |
| `observe_response_challenge` | `std::function<void(const HttpResponseChallengeContext&)>` | unset | Invoked after each GET/POST response |
| `on_establish_result` | `std::function<void(bool)>` | unset | Internal fallback hook: `true` on the first valid SSE event, `false` when retries are exhausted without establishment; normal users leave it unset |
| `WebSocketClientTransport` | `class WebSocketClientTransport : public IClientTransport` | — | WebSocket client transport; **there is no Options struct**, only two constructor arguments |
| Constructor | `WebSocketClientTransport(std::string url, std::string name = "websocket")` | `name = "websocket"` | `Name()` returns the constructed name verbatim, no fallback logic |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | Accepts `ws`/`wss` only; `wss` uses TLS; 30s connect timeout |

The interfaces are declared in `include/mcp/transport/SseClientTransport.hpp` and `include/mcp/transport/WebSocketClientTransport.hpp`; the implementations are `src/transport/SseClientTransport.cpp` and `src/transport/detail/net/WebSocketClient.cpp`.

::: warning
WebSocket handshake behaviour is tightened (source compatible; public signatures unchanged): `Connect()` used to mark the transport Connected immediately and send before 101 without a bound, failing silently. Now the client stays Connecting until the status line, `Upgrade`, `Connection` and `Sec-WebSocket-Accept` all validate, then enters Open and marks the transport Connected; pre-101 messages are queued in order with a message cap of `kMaxWebSocketHandshakeQueueMessages` (64) and a serialized-byte cap of `kMaxWebSocketHandshakeQueueBytes` (1 MiB), with explicit errors through `SetOnError` when full or when the handshake fails, and new messages are rejected in Closing/Closed; failures, timeouts and closes reach one terminal state and notify once. A single deadline covers DNS/TCP/TLS/request write/response read/redirects, and close or timeout interrupts blocking reads before join. Redirects allow only the same host with the same port or a `ws:80 → wss:443` upgrade, at most `kMaxWebSocketRedirects` (5) hops; downgrades, cross-host, other cross-port, userinfo and malformed Locations are rejected, each hop uses a fresh Sec-WebSocket-Key and TLS verification is never relaxed. Downstream code relying on cross-host/downgrade redirects or unbounded pre-101 sends must configure a legal URL and honour the bounded queue errors; constructors and `Connect()` are unchanged.
:::

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
| `HttpServerOptions` | `struct` with `on_request`/`on_connect`/`on_disconnect`/`allowed_hosts`/`allowed_origins`/`sse_keep_alive_ms`/`bind_host`/`max_request_body_bytes`/`header_read_budget` | `0`, `""` (= `127.0.0.1` loopback; explicit `0.0.0.0`/`::` still binds all interfaces), `kMaxHttpBodyBytes`, `header_read_budget = nullopt` | The transport forwards `host`/`allowed_*` here unchanged |
| `HttpServer` | `HttpServer(uint16_t port, const HttpServerOptions& options = {})`, plus `void Start()` / `void Stop()` | default-constructed options | Not an `ITransport`; independent lifecycle |
| `SetHandler` | `void SetHandler(std::string_view method, std::string_view path, HttpHandler handler)` | — | Exact `(method, path)` match |
| `AddSseClient` / `RemoveSseClient` / `SseClientCount` | `SseClientId AddSseClient(send_fn[, std::string tag])` / `void RemoveSseClient(SseClientId)` / `std::size_t SseClientCount() const` | no tag | `SseClientId = uint64_t`; the tagged overload enables directed broadcast and filtering |
| `BroadcastSse` | `void BroadcastSse(std::string_view event[, const std::function<bool(const std::string&)>& accept])` | all clients | Predicate form filters by tag |

The interface is declared in `include/mcp/http/HttpServer.hpp`; `src/http/HttpServer.cpp` forwards, and `src/http/HttpServerImpl.cpp` does the scheduling.

::: warning HTTP/SSE/202/bind tightening (source compatible, unchanged API signatures)
- Old behavior: an empty `bind_host` bound `INADDR_ANY` (all interfaces); request/response `Content-Length` used naive multiply-add with duplicate headers resolved by last value; each SSE path split on `\n\n` independently, recognizing only CRLF/LF with inconsistent handling of a trailing `\r` across chunks; HTTP 202 was only logged, so a request's pending could hang.
- New behavior: an empty `bind_host` binds only the IPv4 loopback `127.0.0.1`, while the default server card endpoint stays `localhost`; `Content-Length` checks multiplication overflow before comparing with the body limit and rejects empty, non-numeric, duplicate/conflicting and out-of-`size_t` values; `Transfer-Encoding` together with `Content-Length` is rejected, and chunk size overflow plus cumulative body limits are unified; SSE uses one incremental parser where CRLF/LF/CR/mixed endings and a trailing `\r` split across chunks are equivalent, blank lines dispatch events and the event limit is unchanged; 202 acknowledges notifications or client JSON-RPC responses (an empty body completes immediately), while a request carrying `method` that receives 202 immediately gets `ProtocolViolation (-32005)` on its pending and the connection is closed/drained without replay.
- How to adapt: set `HttpServerOptions::bind_host = "0.0.0.0"` (or `"::"` for IPv6) explicitly when all-interface listening is required; make servers answer requests with a real JSON-RPC response/error instead of relying on a later GET/SSE after 202; send a single valid `Content-Length` and never combine it with `Transfer-Encoding`; make SSE servers emit standard CRLF/LF blank-line framing instead of relying on bare CR or trailing-CR chunk boundaries. Constructor and option field signatures are unchanged.
:::

::: warning Single legacy SSE fallback for AutoDetect (source compatible, unchanged API signatures)
- Old behavior: `transport_mode` had no runtime reader, `AutoDetect` always used Streamable HTTP, and a 400/404/405 with a non-protocol body only produced an error.
- New behavior: when the first `initialize` response is 400/404/405 and the body is not a valid JSON-RPC error envelope, `AutoDetect` stops sending, fully closes the old Streamable HTTP session (POST/listen/resume/DELETE/threads), then creates a legacy SSE session with the same endpoint, extra headers, per-request headers, challenge callbacks, redirect policy and size limits and replays that initialize message exactly once. Other statuses, valid error envelopes and later responses never fall back. If SSE cannot be established, pending ends in bounded fashion with `ConnectionClosed`, one error is reported and there is no retry loop. Explicit `StreamableHttp`/`Sse` modes and `ConnectMode::Pin/Legacy` never fall back.
- Related SSE tightening (same compatibility surface): messages sent before the `endpoint` event used to be dropped, and any response body chunk reset the reconnect budget. Messages now wait for the `endpoint` event and are then sent in order (so fallback replay cannot be lost), and only a valid `endpoint`/`message` event resets the reconnect budget, so non-SSE error bodies cannot reconnect forever.
- How to adapt: downstream code that must not fall back should set `transport_mode` to `StreamableHttp` or `Sse`, use `ConnectMode::Pin/Legacy` (the client disables transport fallback), or call `ITransportFallbackControl::SetFallbackEnabled(false)` on the returned transport; code that expected an immediate error on a text 400/404/405 body must expect one replayed legacy SSE initialization instead; code that relied on pre-endpoint sends being dropped or on error-body chunk resets must wait for the endpoint event before sending and have the server emit valid SSE frames. All new `SseClientTransportOptions` fields have defaults, so existing constructors and aggregate initialization remain valid.
:::

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
| `kMaxWebSocketRedirects` | `int` | `5` | WebSocket redirect hop cap |
| `kMaxWebSocketHandshakeQueueMessages` | `size_t` | `64` | Pre-101 queued message cap |
| `kMaxWebSocketHandshakeQueueBytes` | `size_t` | 1 MiB | Pre-101 queued serialized-byte cap |
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
| Streamable HTTP server (default `stateless = true`) | `true` | Each request stands alone with a synchronous JSON response (deadline `kDefaultHttpRequestTimeout` = 30s, overridable via `request_timeout`); the zero-listener wait is skipped |
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
