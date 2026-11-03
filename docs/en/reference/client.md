# Client Facade (`McpClient`) Reference

This page is the **single authoritative source for signatures and field tables** of `McpClient`, `ClientOptions`, `RequestHandle`/`RequestOutcome` and `ConnectMode`; step-by-step usage lives in later how-to pages, which only link here and never duplicate the tables. Protocol-version semantics: `/en/learn/protocol-versions`; data types: `/en/reference/types`; error codes: `/en/reference/errors`; transport construction: `/en/reference/transports`.

## Factory and lifecycle

| Symbol | Signature/Type | Default/Returns | Notes |
|---|---|---|---|
| `McpClient::Create` | `static std::unique_ptr<McpClient> Create(std::shared_ptr<ITransport> transport, const ClientOptions& options = {})` | `std::unique_ptr<McpClient>` | Negotiation runs **synchronously inside construction** (`Auto`: `server/discover` probe first, `initialize` handshake as fallback), so the session is usable on return; a failed negotiation throws straight out of `Create()` |
| `~McpClient` | `~McpClient()` | — | Calls `Close()` and detaches the transport callbacks, so destroying the `unique_ptr` disconnects |
| `Close` | `void Close()` | — | Closes the session handler, then clears the private-scope response cache and the cached auth context |
| `GetServerCapabilities` | `const ServerCapabilities& GetServerCapabilities() const` | **returned by reference** | Snapshot of the negotiation outcome; points into internal state and must not outlive the client |
| `GetServerInfo` | `const Implementation& GetServerInfo() const` | reference | Same as above |
| `GetInstructions` | `std::optional<std::string> GetInstructions() const` | `nullopt` | The server's `instructions` |
| `GetNegotiatedProtocolVersion` | `std::string_view GetNegotiatedProtocolVersion() const` | negotiated version | View references the internal string |
| `IsModernProtocol` | `bool IsModernProtocol() const` | flag from negotiation | Returns the `is_modern` recorded by negotiation — true for a successful `server/discover` **and** for `Pin`, false for every `initialize` path; it does not re-lexicographically test the pinned string (`IsModernProtocolVersion` is `>= 2026-07-28`), so pinning a legacy version still reports `true` |

::: info
The client has no `Run()`: `Create()` already starts the session handler's message thread, and notifications plus reverse requests are delivered asynchronously through the handler fields.
:::

## Typed request API: tools, resources, prompts

| Symbol | Signature/Type | Default/Returns | Notes |
|---|---|---|---|
| `ListTools` | `ListToolsResult ListTools(std::optional<std::string> cursor = std::nullopt)` | one page | Consults the response cache first (a hit sends no request) and carries etag; on `notModified` the cached body is returned |
| `ListToolsAll` | `ListToolsResult ListToolsAll()` | `next_cursor` empty | Loops `ListTools` and aggregates every page |
| `SearchTools` | `ListToolsResult SearchTools(std::optional<std::string> query = std::nullopt, std::optional<int64_t> page_size = std::nullopt)` | aggregated result | Sends `tools/list` page by page directly; `query`/`pageSize` are only transmitted **when `enable_progressive_discovery` is set**; bypasses the cache |
| `CallTool` | `CallToolResult CallTool(std::string_view name, std::optional<JsonValue> arguments = std::nullopt, const RequestOptions& options = {})` | `CallToolResult` | Runs through the MRTR fulfilment loop; a task handle from the peer is polled to a terminal state before returning; on `HeaderMismatch` it clears the cache, re-fetches `tools/list` and retries the call verbatim once |
| `ListResources` / `ListResourcesAll` | `ListResourcesResult (std::optional<std::string> cursor = std::nullopt)` / `ListResourcesResult ()` | tool semantics | Cache + etag; `All` aggregates pages |
| `ListResourceTemplates` / `ListResourceTemplatesAll` | as above | as above | Result field is `resource_templates` |
| `ReadResource` | `ReadResourceResult ReadResource(std::string_view uri, const CacheableRequestOptions& options = {})` | `ReadResourceResult` | The only resource method taking `CacheableRequestOptions`: `cache_mode` decides cache reads/writes, `max_age_ms` bounds freshness; runs through MRTR |
| `SubscribeResource` / `UnsubscribeResource` | `EmptyResult SubscribeResource(std::string_view uri)` / `EmptyResult UnsubscribeResource(std::string_view uri)` | `EmptyResult` | Resource-level subscription (distinct from `SubscribeAsync`'s notification filter) |
| `ListPrompts` / `ListPromptsAll` | `ListPromptsResult (std::optional<std::string> cursor = std::nullopt)` / `ListPromptsResult ()` | as above | Cache + etag + page aggregation |
| `GetPrompt` | `GetPromptResult GetPrompt(std::string_view name, std::optional<JsonValue> arguments = std::nullopt, const RequestOptions& options = {})` | `GetPromptResult` | Runs through MRTR; a task handle from the peer is polled to a terminal state |
| `Complete` | `CompleteResult Complete(const CompleteRequestParams& params)` | `CompleteResult` | `completion/complete`, fixed 60s timeout, no MRTR |

## Typed request API: skills, tasks, discovery

| Symbol | Signature/Type | Default/Returns | Notes |
|---|---|---|---|
| `ListSkills` | `SkillsListResult ListSkills(std::optional<std::string> cursor = std::nullopt)` | paginated result | With `enable_skills_extension=false` throws `McpError(MethodNotFound, "skills extension is disabled")` |
| `GetSkill` | `SkillsGetResult GetSkill(std::string_view uri, const CacheableRequestOptions& options = {})` | `SkillsGetResult` | Same gate; runs through MRTR and the cache |
| `ReadDirectory` | `ReadDirectoryResult ReadDirectory(std::string_view uri, std::optional<std::string> cursor = std::nullopt, const CacheableRequestOptions& options = {})` | entry list | Same gate; the server additionally needs `SetSkillsDirectoryRead(true)` |
| `CallToolAsTask` | `GetTaskResult CallToolAsTask(std::string_view name, std::optional<JsonValue> arguments = std::nullopt, const RequestOptions& options = {})` | task handle (`task_id`/`status`) | When the peer did not task-ify the call it **degrades** to `task_id="<none>"`, `status="completed"` and `result` holding the raw tool-result JSON |
| `GetTask` | `GetTaskResult GetTask(std::string_view task_id)` | `GetTaskResult` | Fixed 600s timeout; **does not check** `enable_tasks_extension` |
| `UpdateTask` | `UpdateTaskResult UpdateTask(std::string_view task_id, std::optional<JsonValue> result = std::nullopt, std::optional<JsonValue> input_responses = std::nullopt)` | `EmptyResult` alias | Requires `enable_tasks_extension`; `result` and `input_responses` are checked for mutual exclusion by the server |
| `CancelTask` | `CancelTaskResult CancelTask(std::string_view task_id, std::optional<std::string> reason = std::nullopt)` | `EmptyResult` alias | **Does not check** `enable_tasks_extension` |
| `PollTaskToCompletion` | `GetTaskResult PollTaskToCompletion(const std::string& task_id, std::chrono::milliseconds poll_interval = std::chrono::milliseconds(500), std::chrono::seconds timeout = std::chrono::seconds(300))` | terminal `GetTaskResult` | Requires `enable_tasks_extension`; the server's `poll_interval_ms` wins, capped at 300s; terminal states are `completed`/`failed`/`cancelled`; exhausting the budget throws `InternalError` |
| `Discover` | `DiscoverResult Discover()` | this answer | Re-sends `server/discover` and **does not change** the negotiated version |

## Request option types

| Field | Type | Default | Notes |
|---|---|---|---|
| `RequestOptions::meta` | `std::optional<JsonValue>` | `nullopt` | Sent as the `_meta` extension; a contained `progressToken` becomes the progress key |
| `RequestOptions::read_timeout_ms` | `std::optional<int64_t>` | `nullopt` | Read only by `ReadResource`/`GetSkill`/`ReadDirectory`/`GetPrompt`, as the timeout of the current MRTR round |
| `RequestOptions::input_responses` | `std::optional<JsonValue>` | `nullopt` | Manually fulfils `input_required` (used when auto-fulfilment is off) |
| `RequestOptions::request_state` | `std::optional<std::string>` | `nullopt` | Echoes back the `requestState` issued by the server |
| `RequestOptions::on_progress` | `std::function<void(const ProgressNotificationParams&)>` | empty | Non-empty registers a progress callback automatically and detaches it on return |
| `RequestOptions::timeout` | `std::chrono::milliseconds` | `kDefaultRequestTimeout` (60000ms) | **Read only by the bare `SendRequest` (`RequestHandle` overload)**; typed methods each have a fixed source |
| `RequestOptions::cancel_on_timeout` | `bool` | `true` | On timeout sends `notifications/cancelled` to the peer |
| `RequestOptions::cancel_token` | `CancelToken` | default-constructed | Shares the cancellation channel with `RequestHandle::Cancel` |
| `CacheableRequestOptions::cache_mode` | `std::optional<std::string>` | `nullopt` → `"use"` | `"bypass"` skips both cache read and write; `"refresh"` skips the read but still writes |
| `CacheableRequestOptions::max_age_ms` | `std::optional<int64_t>` | `nullopt` | Freshness bound to accept |

Declaration sites: the fields above are declared in `include/mcp/McpTypes.hpp`, while the progress callback and the raw interface are registered in `include/mcp/client/McpClient.hpp`.

## Raw interface and cancellation

| Symbol | Signature/Type | Default/Returns | Notes |
|---|---|---|---|
| `SendRequest` (handle overload) | `RequestHandle SendRequest(std::string_view method, JsonValue params, const RequestOptions& options)` | `RequestHandle` | **None of the three parameters has a default argument**; bypasses MRTR and never throws on protocol errors — the error lands in the future as `ErrorData` |
| `SendRequest` (legacy overload) | `std::future<JsonValue> SendRequest(std::string_view method, JsonValue params, const RequestMeta& meta = {}, std::chrono::milliseconds timeout = kDefaultRequestTimeout)` | `future<JsonValue>` | Forwards to `SendRequestRaw`, keeping the old behaviour; a completely empty `meta` is replaced by the client's own `_meta` |
| `SendRequestRaw` | `std::future<JsonValue> SendRequestRaw(std::string_view method, JsonValue params, const RequestMeta& meta = {}, std::chrono::milliseconds timeout = kDefaultRequestTimeout)` | `future<JsonValue>` | Equivalent to the row above, only more explicit by name |
| `SendNotification` | `void SendNotification(std::string_view method, JsonValue params = {})` | — | Emits an outgoing notification, no acknowledgement |
| `CancelRequest` | `void CancelRequest(const RequestId& id, std::string_view reason = {})` | — | Sends `notifications/cancelled` for that id |
| `RequestHandle::id` | `RequestId` (`std::variant<int64_t, std::string>`) | allocated id | Can be handed to `CancelRequest` directly; `RequestId` is defined in `include/mcp/JsonRpc.hpp` |
| `RequestHandle::future` | `std::future<RequestOutcome>` | `RequestOutcome = std::variant<JsonValue, ErrorData>` | The variant is defined in `include/mcp/protocol/IncomingRequestMeta.hpp` |
| `RequestHandle::Cancel` | `void Cancel(std::string_view reason = {})` | — | Silent no-op when no cancel function is bound; the handle is move-only (copy deleted) |

## MRTR multi-round fulfilment

| Field | Type | Default | Notes |
|---|---|---|---|
| `ClientOptions::input_required_config` | `std::optional<InputRequiredConfig>` | **already set by default** (`InputRequiredConfig{}`) | Setting `nullopt` disables auto-fulfilment and the `input_required` result reaches the caller untouched |
| `input_required_config->auto_fulfill` | `bool` | `true` | When false no handler is invoked at all |
| `input_required_config->max_rounds` | `int` | `10` | The loop bound is `round <= max_rounds`, exceeding it throws `InternalError` (`"MRTR: exceeded max_rounds (n)"`) |
| `input_required_config->round_timeout` | `std::chrono::seconds` | `600` | Per-round timeout, narrowed by the caller-supplied timeout |
| `input_required_config->max_total_timeout` | `std::chrono::seconds` | `0` (unlimited) | Hard budget for the whole multi-round flow; exhausting it throws `RequestTimeout` (`"MRTR: exceeded max_total_timeout"`) |

Behavioural highlights (every decision below is implemented in `src/client/McpClient.cpp`; users only see configuration and results):

- Only `resultType == "input_required"` enters fulfilment; anything else returns immediately.
- A missing handler means **no partial fulfilment**: `elicitation/create` without `elicitation_handler` (non-url mode), `sampling/createMessage` without `sampling_handler`, `roots/list` without `roots_handler` all make the round unfulfillable and the `input_required` result goes back to the caller.
- URL mode always uses the `url_elicitation_handler` slot, and it is **not** presence-gated: with no handler set the SDK synthesizes `ElicitResult{action="decline"}` instead of throwing; only a handler returning an action outside `accept`/`decline`/`cancel` raises `HandlerError`. The presence check in front of auto-fulfilment gates form mode only (`params.mode != "url"` plus a missing form handler makes the round unfulfillable), so URL mode is not gated by it; the `applyDefaults` back-fill likewise affects form mode only.
- A response carrying only `requestState` without `inputRequests` is treated as a "state round": exponential backoff 50ms→250ms, then retry.
- An unknown `inputRequests.method` throws `HandlerError` (`"unsupported input request method: <method>"`).
- On `SessionExpired` with `reinit_on_expired_session=true` the client re-negotiates and replays exactly once; with the flag off it rethrows. This recovery **covers the MRTR path only** — the `DoSendRequest` family (list methods, `Complete`, `GetTask`, …) and the bare `SendRequest` do not get it.

## Passive handlers (server-initiated)

| Symbol | Signature/Type | Default/Returns | Notes |
|---|---|---|---|
| `SetElicitationHandler` | `void SetElicitationHandler(ElicitationHandler handler)`, `ElicitationHandler = std::function<ElicitResult(const ElicitRequestParams&)>` | unset means no auto-fulfilment | **Returns `ElicitResult` (not void)**; `action` is usually `"accept"`/`"decline"`/`"cancel"`; the handler types are declared in `include/mcp/client/ClientOptions.hpp` |
| `SetUrlElicitationHandler` | `void SetUrlElicitationHandler(UrlElicitationHandler handler)`, `UrlElicitationHandler = std::function<ElicitResult(const ElicitRequestParams&)>` | as above | Also returns `ElicitResult`; `params.url`/`params.elicitation_id` are read from there |
| `SetNotificationHandler` | `void SetNotificationHandler(std::string_view method, ClientNotificationHandler handler)`, `ClientNotificationHandler = std::function<void(const JsonRpcNotification&)>` | — | Special case: `method` equal to `subscriptions/acknowledged` does not go to the session handler but is stored as the user ack callback |
| `SetLoggingHandler` | `void SetLoggingHandler(std::function<void(const LoggingMessageNotificationParams&)> handler)` | — | Registering takes over `notifications/message`; a throwing handler only produces an internal log entry and does not affect the connection |
| `SubscribeAsync` | `void SubscribeAsync(const SubscriptionsListenRequestParams& params = {})` | — | Reuses or mints the `_meta` subscription id, sends `subscriptions/listen` and waits for the acknowledged frame (5000ms cap); not receiving it throws `InternalError`; on non-modern versions the server answers `MethodNotFound` |
| `SetLoggingLevel` | `void SetLoggingLevel(LoggingLevel level)` | — | Sends `logging/setLevel` (60s timeout); **modern versions throw `ProtocolViolation`**, pointing to the per-request `logLevel` instead |
| `SendRootsListChanged` | `void SendRootsListChanged()` | — | Throws `ProtocolViolation` on modern versions (method removed) and also on versions below `2025-06-18` |

## Deprecated but still callable

The four symbols below carry `[[deprecated]]` in the header: they **still exist and are still callable**, new code should simply not use them (older docs describing them as "removed/unavailable" are wrong).

| Symbol | Attribute and signature | Replacement | Actual behaviour |
|---|---|---|---|
| `Ping` | `[[deprecated("Ping is deprecated in 2026-07-28 protocol version")]] EmptyResult Ping()` | any lightweight typed call, or `GetTask` | Still callable: 10s timeout, the response payload is ignored and an empty result is returned |
| `SetSamplingHandler` | `[[deprecated("Use SetElicitationHandler instead (SEP-2577)")]] void SetSamplingHandler(SamplingHandler)` | `SetElicitationHandler` (fulfils `sampling/createMessage` inside MRTR) | Still callable: installs the `sampling/createMessage` inbound handler; without a handler that entry answers `MethodNotFound` (`"sampling not supported"`) |
| `SetRootsHandler` | `[[deprecated("Roots are deprecated in 2026-07-28 (SEP-2577)")]] void SetRootsHandler(RootsHandler)` | `SetElicitationHandler`; `SendRootsListChanged` is no longer needed | Still callable: installs the `roots/list` inbound handler; without a handler it answers `MethodNotFound` (`"roots not supported"`) |
| `SetLegacyUrlElicitationHandler` | `[[deprecated("use ElicitResult-returning handler")]] void SetLegacyUrlElicitationHandler(std::function<void(const ElicitRequestParams&)>)` | `SetUrlElicitationHandler` | Still callable: wraps the void callback into a `UrlElicitationHandler` that always returns `action="accept"` |

The `[[deprecated]]` attributes and the declarations of all four are in `include/mcp/client/McpClient.hpp`.

## `ClientOptions` field by field

| Field | Type | Default | Notes / prerequisites |
|---|---|---|---|
| `client_info` | `Implementation` | `{"mcp-cpp-client", std::string(kSdkVersion)}` | The version is expressed through the symbol (`kSdkVersion` is defined in `include/mcp/McpVersion.hpp`), never as a hard-coded number |
| `capabilities` | `std::optional<ClientCapabilities>` | `nullopt` | Overwritten during construction by `DeriveClientCapabilities()` (see the passive handlers and the four handler fields) |
| `elicitation_handler` | `std::optional<ElicitationHandler>` | `nullopt` | Equivalent to calling `SetElicitationHandler` right after construction; derives the `elicitation.form` capability |
| `url_elicitation_handler` | `std::optional<UrlElicitationHandler>` | `nullopt` | Equivalent to calling `SetUrlElicitationHandler` right after construction; derives the `elicitation.url` capability |
| `sampling_handler` | `std::optional<SamplingHandler>` | `nullopt` | Derives the `sampling` capability and installs the inbound handler during construction; the type is deprecated by SEP-2577 |
| `roots_handler` | `std::optional<RootsHandler>` | `nullopt` | Derives the `roots.list_changed` capability and installs the handler; the type is deprecated |
| `connect_mode` | `ConnectMode` | `ConnectMode::Auto` | Three modes, see the next section |
| `pin_protocol_version` | `std::optional<std::string>` | `nullopt` | Read only by `Pin`; when unset `kLatestProtocolVersion` is used |
| `initialization_timeout` | `std::chrono::seconds` | `60` | Bounds the `initialize` handshake only (the `Legacy` mode and the `Auto` fallback path) |
| `discover_probe_timeout` | `std::chrono::seconds` | `5` | Bounds the `server/discover` probe of `Auto` mode only |
| `max_total_timeout` | `std::chrono::seconds` | `0` (unlimited) | Handed to the session handler as the per-request total budget (the deadline progress renewal may never cross) |
| `input_required_config` | `std::optional<InputRequiredConfig>` | `InputRequiredConfig{}` (already set) | MRTR behaviour bundle, item by item in the table above |
| `extensions` | `std::optional<JsonValue>` | `nullopt` | Merged into the outgoing `_meta.clientCapabilities.extensions`; to accept task results in the 2026 era it must contain `"io.modelcontextprotocol/tasks"` |
| `reinit_on_expired_session` | `bool` | `true` | Effective on the MRTR path only (see that section) |
| `span_handler` | `SpanHandler` (`std::function<void(const SpanEvent&)>`) | empty | Handed to the session handler; `SpanHandler` is defined in `include/mcp/SpanHooks.hpp` |
| `enable_tasks_extension` | `bool` | `false` | Gates `UpdateTask`/`PollTaskToCompletion`; together with `extensions` it decides whether task results are accepted |
| `enable_skills_extension` | `bool` | `false` | Gates `ListSkills`/`GetSkill`/`ReadDirectory` |
| `enable_progressive_discovery` | `bool` | `false` | Decides whether `SearchTools` transmits `query`/`pageSize` |
| `enable_etag_cache` | `bool` | **`true`** | Opposite of the server default; additionally requires the process-wide `IsEtagCacheEnabled()` to be true (that switch is rewritten by `ServerOptions::enable_etag_cache`) |
| `cache_auth_context` | `std::function<std::string()>` | empty | Any change of its return value clears the private-scope cache, giving per-tenant isolation |
| `on_transport_close` | `std::function<void()>` | empty | Attached only when the transport derives from `TransportBase`, otherwise a Warning is logged and it is ignored; exceptions inside the callback are swallowed |
| `on_transport_error` | `std::function<void(std::string_view)>` | empty | Same as above, exceptions swallowed too |
| `DeriveClientCapabilities` | `std::optional<ClientCapabilities> DeriveClientCapabilities(const ClientOptions&)` (free function) | `nullopt` when everything is empty | Public derivation entry point, usable to self-check the declared set |

Every field is declared in `include/mcp/client/ClientOptions.hpp`; the construction-time effects are in `src/client/McpClient.cpp`.

## Version negotiation and `ConnectMode`

| Mode | Value | Behaviour |
|---|---|---|
| `Auto` | `ConnectMode::Auto` (default) | `server/discover` probe first (`discover_probe_timeout`); on success it is a modern direct connection with the version picked by `SelectSharedVersion`; on failure it falls back to the `initialize` handshake |
| `Legacy` | `ConnectMode::Legacy` | `initialize` + `notifications/initialized` only, `is_modern=false` |
| `Pin` | `ConnectMode::Pin` | Sends nothing: sets the version to `pin_protocol_version` (or `kLatestProtocolVersion`) and synthesizes a `discover` result |

`Auto` handles probe failures per transport class (`ConnectMode` is enumerated in `include/mcp/client/ClientOptions.hpp`, the classification logic lives in `src/client/McpClient.cpp`):

- **stdio-like** (RTTI name matching `InMemoryTransportImpl` / `StdioClientSessionTransport`): a probe timeout or exception is always read as "the peer has no discover support" and falls back to `initialize` silently.
- **network transports**: a probe timeout throws `RequestTimeout` (`"server/discover timed out"`) and any exception throws `ConnectionClosed` (`"server/discover failed: …"`); no fallback.
- On `-32022` (`UnsupportedProtocolVersion`) whose `data.supported` contains `kLatestProtocolVersion`, one corrective retry is made; a second rejection is a hard error (`McpError(UnsupportedProtocolVersion)`).
- On `-32022` where `supported` lacks the latest version but holds another modern version, `UnsupportedProtocolVersion` is thrown directly; when `supported` contains no modern version at all, it falls back to `initialize`.
- A `discover` answer that fails to parse, or shares no version with the client's declared set, is likewise treated as a probe failure.

## Error semantics and timeout ownership

| Situation | Observable behaviour |
|---|---|
| Typed methods on the `DoSendRequest` path (list methods, `Complete`, `GetTask`/`UpdateTask`/`CancelTask`, `Ping`, `Discover`, `SetLoggingLevel`) receive a protocol error | Throws `McpError(code, message)`; **the error `data` is discarded** |
| MRTR path (`CallTool`, `GetPrompt`, `ReadResource`, `GetSkill`, `ReadDirectory`, `CallToolAsTask`) receives a protocol error | Throws `McpError(code, message, data)`, keeping `data` |
| Bare `SendRequest` (`RequestHandle` overload) receives a protocol error | **No throw**: the `future` yields the `ErrorData` alternative of `RequestOutcome` |
| `CallTool` gets a task handle | Automatic `PollTaskToCompletion`; `failed`/`cancelled` are rethrown as `InternalError`; in the modern era without the tasks extension declared it throws `HandlerError` |
| Pagination does not converge | `ListToolsAll`/`SearchTools`/`ListResourcesAll`/`ListResourceTemplatesAll`/`ListPromptsAll` throw `ProtocolViolation` past 64 pages (`"list pagination did not converge within 64 pages"`) |
| Extension flag missing | `ListSkills`/`GetSkill`/`ReadDirectory` throw `MethodNotFound`; `UpdateTask`/`PollTaskToCompletion` throw `MethodNotFound` as well |
| Timeout ownership | Typed methods **ignore** `RequestOptions::timeout`: `CallTool`/`CallToolAsTask` use `input_required_config->round_timeout` (default 600s), `GetPrompt`/`ReadResource`/`GetSkill`/`ReadDirectory` use `read_timeout_ms` else 60s, list methods plus `Complete`/`Discover` use 60s, `GetTask`/`UpdateTask`/`CancelTask` use 600s, `Ping` uses 10s |

All of these decisions are implemented in `src/client/McpClient.cpp`.

::: tip
For a strict single-request timeout, or to obtain `ErrorData`, use the `SendRequest(method, params, RequestOptions{...})` handle overload; the typed methods' timeout semantics are fixed per the table above and cannot be overridden through `RequestOptions::timeout`.
:::

## Skill verification helpers

Public free functions for consistency checks before and after adopting a skill, all declared in `include/mcp/client/SkillVerify.hpp`:

| Symbol | Signature/Type | Default/Returns | Notes |
|---|---|---|---|
| `kMaxSkillFiles` | `constexpr size_t` | `512` | Per-skill file count limit |
| `kMaxSkillTotalBytes` | `constexpr int64_t` | `16 * 1024 * 1024` | Per-skill total byte limit |
| `CheckSkillLimits` | `bool CheckSkillLimits(const Skill&, std::string& error_out)` | `false` + reason | Validates file count and total size |
| `VerifyFileBytes` | `bool VerifyFileBytes(const SkillResource&, std::string_view bytes, std::string& error_out)` | `false` + reason | Compares digest and length |
| `VerifyFrontmatter` | `bool VerifyFrontmatter(const Skill& advertised, const JsonValue& fetched, std::string& error_out)` | `false` + reason | Compares advertised and fetched frontmatter |
| `IsApprovalStillValid` | `bool IsApprovalStillValid(const Skill& approved, const Skill& current)` | `bool` | Whether an already approved skill is still valid |
| `ApprovalKey` | `std::string ApprovalKey(const std::string& host_label, const Skill&)` | key string | Key for approval records |

## Internal symbols (not user API — never cite them as interface)

- `SendRequestWithMrtr` / `SendRequestWithMrtrOnce` / `RecoverExpiredSession` / `WireClientHandlers` / `CacheIfHinted` / `MaybeAttachEtag` / `NegotiationResult negotiation_` are all private `McpClient` members (declared in `include/mcp/client/McpClient.hpp`); MRTR surfaces to users only as `ClientOptions::input_required_config` and the returned `input_required` result.
- `VersionNegotiation::Negotiate/ProbeDiscover/HandshakeInitialize` and `NegotiationResult` are declared in a public header (`include/mcp/client/VersionNegotiation.hpp`) but belong to the negotiation machinery; users should influence them only through `ConnectMode` and the timeout fields.
- `detail::ResponseCache`, `detail::Base64Url`, `detail::JoinThreadSafely` and other `detail` namespace symbols are not public contracts; neither docs nor code may reference them as user API.
- Constants such as `kElicitTimeout`, `kMaxListPages`, `kMrtrStateOnlyBackoff*` and `kSubscriptionAckTimeout` live in the anonymous namespace inside `src/` and may only be cited as implementation-internal values when describing behaviour.

Task-shaped guides: [/en/howto/connect-to-a-server](/en/howto/connect-to-a-server), [/en/howto/call-tools-and-read-resources](/en/howto/call-tools-and-read-resources), [/en/howto/cancel-a-request](/en/howto/cancel-a-request).

## Source files behind this page

- `include/mcp/client/McpClient.hpp`
- `include/mcp/client/ClientOptions.hpp`
- `include/mcp/client/RequestHandle.hpp`
- `include/mcp/client/VersionNegotiation.hpp`
- `include/mcp/client/SkillVerify.hpp`
- `include/mcp/protocol/IncomingRequestMeta.hpp` (`RequestOutcome`, `IncomingRequestMeta`)
- `include/mcp/McpTypes.hpp` (`RequestOptions`, `CacheableRequestOptions`, the `*Result`/`*RequestParams` types)
- `include/mcp/Meta.hpp` (`RequestMeta`), `include/mcp/JsonRpc.hpp` (`RequestId`, `ErrorData`)
- `include/mcp/Capabilities.hpp`, `include/mcp/SpanHooks.hpp`, `include/mcp/McpVersion.hpp`, `include/mcp/ProtocolVersion.hpp`
- `src/client/McpClient.cpp`
