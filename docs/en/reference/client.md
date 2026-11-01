# Client Facade (`McpClient`) Reference

This page is the **single authoritative source for signatures and field tables** of `McpClient`, `ClientOptions`, `RequestHandle`/`RequestOutcome` and `ConnectMode`; step-by-step usage lives in later how-to pages, which only link here and never duplicate the tables. Protocol-version semantics: `/en/learn/protocol-versions`; data types: `/en/reference/types`; error codes: `/en/reference/errors`; transport construction: `/en/reference/transports`.

## Factory and lifecycle

| Symbol | Signature/Type | Default/Returns | Notes | Evidence |
|---|---|---|---|---|
| `McpClient::Create` | `static std::unique_ptr<McpClient> Create(std::shared_ptr<ITransport> transport, const ClientOptions& options = {})` | `std::unique_ptr<McpClient>` | Negotiation runs **synchronously inside construction** (`Auto`: `server/discover` probe first, `initialize` handshake as fallback), so the session is usable on return; a failed negotiation throws straight out of `Create()` | `include/mcp/client/McpClient.hpp:34-36`; `src/client/McpClient.cpp:679-691`, `611-636` |
| `~McpClient` | `~McpClient()` | — | Calls `Close()` and detaches the transport callbacks, so destroying the `unique_ptr` disconnects | `McpClient.hpp:38`; `McpClient.cpp:638-641` |
| `Close` | `void Close()` | — | Closes the session handler, then clears the private-scope response cache and the cached auth context | `McpClient.hpp:150`; `McpClient.cpp:693-698` |
| `GetServerCapabilities` | `const ServerCapabilities& GetServerCapabilities() const` | **returned by reference** | Snapshot of the negotiation outcome; points into internal state and must not outlive the client | `McpClient.hpp:40`; `McpClient.cpp:818-821` |
| `GetServerInfo` | `const Implementation& GetServerInfo() const` | reference | Same as above | `McpClient.hpp:41`; `McpClient.cpp:822-825` |
| `GetInstructions` | `std::optional<std::string> GetInstructions() const` | `nullopt` | The server's `instructions` | `McpClient.hpp:42`; `McpClient.cpp:826-829` |
| `GetNegotiatedProtocolVersion` | `std::string_view GetNegotiatedProtocolVersion() const` | negotiated version | View references the internal string | `McpClient.hpp:43`; `McpClient.cpp:830-833` |
| `IsModernProtocol` | `bool IsModernProtocol() const` | flag from negotiation | Returns the `is_modern` recorded by negotiation — true for a successful `server/discover` **and** for `Pin`, false for every `initialize` path; it does not re-lexicographically test the pinned string (`IsModernProtocolVersion` is `>= 2026-07-28`), so pinning a legacy version still reports `true` | `McpClient.hpp:44`; `McpClient.cpp:834-837`, `429-434`, `414-421`; `include/mcp/ProtocolVersion.hpp:21-22` |

::: info
The client has no `Run()`: `Create()` already starts the session handler's message thread, and notifications plus reverse requests are delivered asynchronously through the handler fields.
:::

## Typed request API: tools, resources, prompts

| Symbol | Signature/Type | Default/Returns | Notes | Evidence |
|---|---|---|---|---|
| `ListTools` | `ListToolsResult ListTools(std::optional<std::string> cursor = std::nullopt)` | one page | Consults the response cache first (a hit sends no request) and carries etag; on `notModified` the cached body is returned | `McpClient.hpp:46-47`; `McpClient.cpp:1215-1235` |
| `ListToolsAll` | `ListToolsResult ListToolsAll()` | `next_cursor` empty | Loops `ListTools` and aggregates every page | `McpClient.hpp:48`; `McpClient.cpp:1237-1251` |
| `SearchTools` | `ListToolsResult SearchTools(std::optional<std::string> query = std::nullopt, std::optional<int64_t> page_size = std::nullopt)` | aggregated result | Sends `tools/list` page by page directly; `query`/`pageSize` are only transmitted **when `enable_progressive_discovery` is set**; bypasses the cache | `McpClient.hpp:49-51`; `McpClient.cpp:1252-1279` |
| `CallTool` | `CallToolResult CallTool(std::string_view name, std::optional<JsonValue> arguments = std::nullopt, const RequestOptions& options = {})` | `CallToolResult` | Runs through the MRTR fulfilment loop; a task handle from the peer is polled to a terminal state before returning; on `HeaderMismatch` it clears the cache, re-fetches `tools/list` and retries the call verbatim once | `McpClient.hpp:52-55`; `McpClient.cpp:1312-1357`, `1280-1310` |
| `ListResources` / `ListResourcesAll` | `ListResourcesResult (std::optional<std::string> cursor = std::nullopt)` / `ListResourcesResult ()` | tool semantics | Cache + etag; `All` aggregates pages | `McpClient.hpp:57-59`; `McpClient.cpp:1359-1395` |
| `ListResourceTemplates` / `ListResourceTemplatesAll` | as above | as above | Result field is `resource_templates` | `McpClient.hpp:60-62`; `McpClient.cpp:1397-1433` |
| `ReadResource` | `ReadResourceResult ReadResource(std::string_view uri, const CacheableRequestOptions& options = {})` | `ReadResourceResult` | The only resource method taking `CacheableRequestOptions`: `cache_mode` decides cache reads/writes, `max_age_ms` bounds freshness; runs through MRTR | `McpClient.hpp:63-65`; `McpClient.cpp:1435-1470` |
| `SubscribeResource` / `UnsubscribeResource` | `EmptyResult SubscribeResource(std::string_view uri)` / `EmptyResult UnsubscribeResource(std::string_view uri)` | `EmptyResult` | Resource-level subscription (distinct from `SubscribeAsync`'s notification filter) | `McpClient.hpp:66-67`; `McpClient.cpp:1472-1488` |
| `ListPrompts` / `ListPromptsAll` | `ListPromptsResult (std::optional<std::string> cursor = std::nullopt)` / `ListPromptsResult ()` | as above | Cache + etag + page aggregation | `McpClient.hpp:69-71`; `McpClient.cpp:1490-1525` |
| `GetPrompt` | `GetPromptResult GetPrompt(std::string_view name, std::optional<JsonValue> arguments = std::nullopt, const RequestOptions& options = {})` | `GetPromptResult` | Runs through MRTR; a task handle from the peer is polled to a terminal state | `McpClient.hpp:72-75`; `McpClient.cpp:1527-1557` |
| `Complete` | `CompleteResult Complete(const CompleteRequestParams& params)` | `CompleteResult` | `completion/complete`, fixed 60s timeout, no MRTR | `McpClient.hpp:87`; `McpClient.cpp:1815-1821` |

## Typed request API: skills, tasks, discovery

| Symbol | Signature/Type | Default/Returns | Notes | Evidence |
|---|---|---|---|---|
| `ListSkills` | `SkillsListResult ListSkills(std::optional<std::string> cursor = std::nullopt)` | paginated result | With `enable_skills_extension=false` throws `McpError(MethodNotFound, "skills extension is disabled")` | `McpClient.hpp:77-78`; `McpClient.cpp:1560-1592` |
| `GetSkill` | `SkillsGetResult GetSkill(std::string_view uri, const CacheableRequestOptions& options = {})` | `SkillsGetResult` | Same gate; runs through MRTR and the cache | `McpClient.hpp:79-81`; `McpClient.cpp:1594-1632` |
| `ReadDirectory` | `ReadDirectoryResult ReadDirectory(std::string_view uri, std::optional<std::string> cursor = std::nullopt, const CacheableRequestOptions& options = {})` | entry list | Same gate; the server additionally needs `SetSkillsDirectoryRead(true)` | `McpClient.hpp:82-85`; `McpClient.cpp:1634-1679` |
| `CallToolAsTask` | `GetTaskResult CallToolAsTask(std::string_view name, std::optional<JsonValue> arguments = std::nullopt, const RequestOptions& options = {})` | task handle (`task_id`/`status`) | When the peer did not task-ify the call it **degrades** to `task_id="<none>"`, `status="completed"` and `result` holding the raw tool-result JSON | `McpClient.hpp:89-92`; `McpClient.cpp:1681-1735` |
| `GetTask` | `GetTaskResult GetTask(std::string_view task_id)` | `GetTaskResult` | Fixed 600s timeout; **does not check** `enable_tasks_extension` | `McpClient.hpp:94`; `McpClient.cpp:1737-1745` |
| `UpdateTask` | `UpdateTaskResult UpdateTask(std::string_view task_id, std::optional<JsonValue> result = std::nullopt, std::optional<JsonValue> input_responses = std::nullopt)` | `EmptyResult` alias | Requires `enable_tasks_extension`; `result` and `input_responses` are checked for mutual exclusion by the server | `McpClient.hpp:95-98`; `McpClient.cpp:1746-1763` |
| `CancelTask` | `CancelTaskResult CancelTask(std::string_view task_id, std::optional<std::string> reason = std::nullopt)` | `EmptyResult` alias | **Does not check** `enable_tasks_extension` | `McpClient.hpp:99-101`; `McpClient.cpp:1764-1776` |
| `PollTaskToCompletion` | `GetTaskResult PollTaskToCompletion(const std::string& task_id, std::chrono::milliseconds poll_interval = std::chrono::milliseconds(500), std::chrono::seconds timeout = std::chrono::seconds(300))` | terminal `GetTaskResult` | Requires `enable_tasks_extension`; the server's `poll_interval_ms` wins, capped at 300s; terminal states are `completed`/`failed`/`cancelled`; exhausting the budget throws `InternalError` | `McpClient.hpp:103-106`; `McpClient.cpp:1777-1814`, `21-25` |
| `Discover` | `DiscoverResult Discover()` | this answer | Re-sends `server/discover` and **does not change** the negotiated version | `McpClient.hpp:111`; `McpClient.cpp:1831-1836` |

## Request option types

| Field | Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `RequestOptions::meta` | `std::optional<JsonValue>` | `nullopt` | Sent as the `_meta` extension; a contained `progressToken` becomes the progress key | `include/mcp/McpTypes.hpp:398`; `McpClient.cpp:937-968` |
| `RequestOptions::read_timeout_ms` | `std::optional<int64_t>` | `nullopt` | Read only by `ReadResource`/`GetSkill`/`ReadDirectory`/`GetPrompt`, as the timeout of the current MRTR round | `McpTypes.hpp:399`; `McpClient.cpp:1457-1462`, `1546-1550` |
| `RequestOptions::input_responses` | `std::optional<JsonValue>` | `nullopt` | Manually fulfils `input_required` (used when auto-fulfilment is off) | `McpTypes.hpp:400` |
| `RequestOptions::request_state` | `std::optional<std::string>` | `nullopt` | Echoes back the `requestState` issued by the server | `McpTypes.hpp:401` |
| `RequestOptions::on_progress` | `std::function<void(const ProgressNotificationParams&)>` | empty | Non-empty registers a progress callback automatically and detaches it on return | `McpTypes.hpp:402`; `McpClient.hpp:27`, `McpClient.cpp:937-973` |
| `RequestOptions::timeout` | `std::chrono::milliseconds` | `kDefaultRequestTimeout` (60000ms) | **Read only by the bare `SendRequest` (`RequestHandle` overload)**; typed methods each have a fixed source | `McpTypes.hpp:26`, `403`; `McpClient.cpp:1857-1858` |
| `RequestOptions::cancel_on_timeout` | `bool` | `true` | On timeout sends `notifications/cancelled` to the peer | `McpTypes.hpp:404`; `McpClient.cpp:1864` |
| `RequestOptions::cancel_token` | `CancelToken` | default-constructed | Shares the cancellation channel with `RequestHandle::Cancel` | `McpTypes.hpp:405`; `McpClient.cpp:1867-1872` |
| `CacheableRequestOptions::cache_mode` | `std::optional<std::string>` | `nullopt` → `"use"` | `"bypass"` skips both cache read and write; `"refresh"` skips the read but still writes | `McpTypes.hpp:408-411`; `McpClient.cpp:1441-1468` |
| `CacheableRequestOptions::max_age_ms` | `std::optional<int64_t>` | `nullopt` | Freshness bound to accept | `McpTypes.hpp:410`; `McpClient.cpp:1445-1451` |

## Raw interface and cancellation

| Symbol | Signature/Type | Default/Returns | Notes | Evidence |
|---|---|---|---|---|
| `SendRequest` (handle overload) | `RequestHandle SendRequest(std::string_view method, JsonValue params, const RequestOptions& options)` | `RequestHandle` | **None of the three parameters has a default argument**; bypasses MRTR and never throws on protocol errors — the error lands in the future as `ErrorData` | `McpClient.hpp:130-133`; `McpClient.cpp:1844-1882` |
| `SendRequest` (legacy overload) | `std::future<JsonValue> SendRequest(std::string_view method, JsonValue params, const RequestMeta& meta = {}, std::chrono::milliseconds timeout = kDefaultRequestTimeout)` | `future<JsonValue>` | Forwards to `SendRequestRaw`, keeping the old behaviour; a completely empty `meta` is replaced by the client's own `_meta` | `McpClient.hpp:134-138`; `McpClient.cpp:1884-1903` |
| `SendRequestRaw` | `std::future<JsonValue> SendRequestRaw(std::string_view method, JsonValue params, const RequestMeta& meta = {}, std::chrono::milliseconds timeout = kDefaultRequestTimeout)` | `future<JsonValue>` | Equivalent to the row above, only more explicit by name | `McpClient.hpp:139-143`; `McpClient.cpp:1893-1903` |
| `SendNotification` | `void SendNotification(std::string_view method, JsonValue params = {})` | — | Emits an outgoing notification, no acknowledgement | `McpClient.hpp:144`; `McpClient.cpp:1905-1907` |
| `CancelRequest` | `void CancelRequest(const RequestId& id, std::string_view reason = {})` | — | Sends `notifications/cancelled` for that id | `McpClient.hpp:145`; `McpClient.cpp:1909-1911` |
| `RequestHandle::id` | `RequestId` (`std::variant<int64_t, std::string>`) | allocated id | Can be handed to `CancelRequest` directly | `include/mcp/client/RequestHandle.hpp:15`; `include/mcp/JsonRpc.hpp:15` |
| `RequestHandle::future` | `std::future<RequestOutcome>` | `RequestOutcome = std::variant<JsonValue, ErrorData>` | The variant is defined at `include/mcp/protocol/IncomingRequestMeta.hpp:29` | `include/mcp/client/RequestHandle.hpp:16`; `IncomingRequestMeta.hpp:29` |
| `RequestHandle::Cancel` | `void Cancel(std::string_view reason = {})` | — | Silent no-op when no cancel function is bound; the handle is move-only (copy deleted) | `RequestHandle.hpp:34-36`, `29-32` |

## MRTR multi-round fulfilment

| Field | Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `ClientOptions::input_required_config` | `std::optional<InputRequiredConfig>` | **already set by default** (`InputRequiredConfig{}`) | Setting `nullopt` disables auto-fulfilment and the `input_required` result reaches the caller untouched | `include/mcp/client/ClientOptions.hpp:55-63`; `McpClient.cpp:991-993` |
| `input_required_config->auto_fulfill` | `bool` | `true` | When false no handler is invoked at all | `ClientOptions.hpp:56`; `McpClient.cpp:991-993` |
| `input_required_config->max_rounds` | `int` | `10` | The loop bound is `round <= max_rounds`, exceeding it throws `InternalError` (`"MRTR: exceeded max_rounds (n)"`) | `ClientOptions.hpp:57`; `McpClient.cpp:1076`, `1086`, `1133-1134` |
| `input_required_config->round_timeout` | `std::chrono::seconds` | `600` | Per-round timeout, narrowed by the caller-supplied timeout | `ClientOptions.hpp:58`; `McpClient.cpp:1077-1081` |
| `input_required_config->max_total_timeout` | `std::chrono::seconds` | `0` (unlimited) | Hard budget for the whole multi-round flow; exhausting it throws `RequestTimeout` (`"MRTR: exceeded max_total_timeout"`) | `ClientOptions.hpp:60`; `McpClient.cpp:1082`, `1088-1097` |

Behavioural highlights (fulfilment lives inside the implementation; users only see configuration and results):

- Only `resultType == "input_required"` enters fulfilment; anything else returns immediately (`McpClient.cpp:986-989`).
- A missing handler means **no partial fulfilment**: `elicitation/create` without `elicitation_handler` (non-url mode), `sampling/createMessage` without `sampling_handler`, `roots/list` without `roots_handler` all make the round unfulfillable and the `input_required` result goes back to the caller (`McpClient.cpp:997-1009`, `1113-1130`).
- URL mode always uses the `url_elicitation_handler_` slot, and it is **not** presence-gated: with no handler set the SDK synthesizes `ElicitResult{action="decline"}` instead of throwing (`McpClient.cpp:296-306`); only a handler returning an action outside `accept`/`decline`/`cancel` raises `HandlerError` (`McpClient.cpp:285-294`). The `applyDefaults` back-fill only affects form mode (`McpClient.cpp:239-283`, `1024-1032`).
- A response carrying only `requestState` without `inputRequests` is treated as a "state round": exponential backoff 50ms→250ms, then retry (`McpClient.cpp:1013-1017`, `1116-1121`).
- An unknown `inputRequests.method` throws `HandlerError` (`"unsupported input request method: <method>"`) (`McpClient.cpp:1041-1044`).
- On `SessionExpired` with `reinit_on_expired_session=true` the client re-negotiates and replays exactly once; with the flag off it rethrows (`McpClient.cpp:1051-1067`, `1137-1143`). This recovery **covers the MRTR path only** — the `DoSendRequest` family (list methods, `Complete`, `GetTask`, …) and the bare `SendRequest` do not get it.

## Passive handlers (server-initiated)

| Symbol | Signature/Type | Default/Returns | Notes | Evidence |
|---|---|---|---|---|
| `SetElicitationHandler` | `void SetElicitationHandler(ElicitationHandler handler)`, `ElicitationHandler = std::function<ElicitResult(const ElicitRequestParams&)>` | unset means no auto-fulfilment | **Returns `ElicitResult` (not void)**; `action` is usually `"accept"`/`"decline"`/`"cancel"` | `McpClient.hpp:117`; `ClientOptions.hpp:26-27`; `McpClient.cpp:885-887` |
| `SetUrlElicitationHandler` | `void SetUrlElicitationHandler(UrlElicitationHandler handler)`, `UrlElicitationHandler = std::function<ElicitResult(const ElicitRequestParams&)>` | as above | Also returns `ElicitResult`; `params.url`/`params.elicitation_id` are read from there | `McpClient.hpp:118`; `ClientOptions.hpp:29-30`; `McpClient.cpp:889-891` |
| `SetNotificationHandler` | `void SetNotificationHandler(std::string_view method, ClientNotificationHandler handler)`, `ClientNotificationHandler = std::function<void(const JsonRpcNotification&)>` | — | Special case: `method` equal to `subscriptions/acknowledged` does not go to the session handler but is stored as the user ack callback | `McpClient.hpp:122-124`, `29-30`; `McpClient.cpp:905-918` |
| `SetLoggingHandler` | `void SetLoggingHandler(std::function<void(const LoggingMessageNotificationParams&)> handler)` | — | Registering takes over `notifications/message`; a throwing handler only produces an internal log entry and does not affect the connection | `McpClient.hpp:126`; `McpClient.cpp:920-935` |
| `SubscribeAsync` | `void SubscribeAsync(const SubscriptionsListenRequestParams& params = {})` | — | Reuses or mints the `_meta` subscription id, sends `subscriptions/listen` and waits for the acknowledged frame (5000ms cap); not receiving it throws `InternalError`; on non-modern versions the server answers `MethodNotFound` | `McpClient.hpp:128`; `McpClient.cpp:1927-1990`, `23`, `32-36` |
| `SetLoggingLevel` | `void SetLoggingLevel(LoggingLevel level)` | — | Sends `logging/setLevel` (60s timeout); **modern versions throw `ProtocolViolation`**, pointing to the per-request `logLevel` instead | `McpClient.hpp:146`; `McpClient.cpp:1913-1925` |
| `SendRootsListChanged` | `void SendRootsListChanged()` | — | Throws `ProtocolViolation` on modern versions (method removed) and also on versions below `2025-06-18` | `McpClient.hpp:148`; `McpClient.cpp:1992-2016` |

## Deprecated but still callable

The four symbols below carry `[[deprecated]]` in the header: they **still exist and are still callable**, new code should simply not use them (older docs describing them as "removed/unavailable" are wrong).

| Symbol | Attribute and signature | Replacement | Actual behaviour | Evidence |
|---|---|---|---|---|
| `Ping` | `[[deprecated("Ping is deprecated in 2026-07-28 protocol version")]] EmptyResult Ping()` | any lightweight typed call, or `GetTask` | Still callable: 10s timeout, the response payload is ignored and an empty result is returned | `McpClient.hpp:108-109`; `McpClient.cpp:1822-1830`, `22` |
| `SetSamplingHandler` | `[[deprecated("Use SetElicitationHandler instead (SEP-2577)")]] void SetSamplingHandler(SamplingHandler)` | `SetElicitationHandler` (fulfils `sampling/createMessage` inside MRTR) | Still callable: installs the `sampling/createMessage` inbound handler; without a handler that entry answers `MethodNotFound` (`"sampling not supported"`) | `McpClient.hpp:113-114`; `McpClient.cpp:838-860` |
| `SetRootsHandler` | `[[deprecated("Roots are deprecated in 2026-07-28 (SEP-2577)")]] void SetRootsHandler(RootsHandler)` | `SetElicitationHandler`; `SendRootsListChanged` is no longer needed | Still callable: installs the `roots/list` inbound handler; without a handler it answers `MethodNotFound` (`"roots not supported"`) | `McpClient.hpp:115-116`; `McpClient.cpp:862-883` |
| `SetLegacyUrlElicitationHandler` | `[[deprecated("use ElicitResult-returning handler")]] void SetLegacyUrlElicitationHandler(std::function<void(const ElicitRequestParams&)>)` | `SetUrlElicitationHandler` | Still callable: wraps the void callback into a `UrlElicitationHandler` that always returns `action="accept"` | `McpClient.hpp:119-121`; `McpClient.cpp:893-903` |

## `ClientOptions` field by field

| Field | Type | Default | Notes / prerequisites | Evidence |
|---|---|---|---|---|
| `client_info` | `Implementation` | `{"mcp-cpp-client", std::string(kSdkVersion)}` | The version is expressed through the symbol (`kSdkVersion` is defined at `include/mcp/McpVersion.hpp:7`), never as a hard-coded number | `ClientOptions.hpp:39`; `McpVersion.hpp:7` |
| `capabilities` | `std::optional<ClientCapabilities>` | `nullopt` | Overwritten during construction by `DeriveClientCapabilities()` (see the passive handlers and the four handler fields) | `ClientOptions.hpp:40`; `McpClient.cpp:611-622`, `360-392` |
| `elicitation_handler` | `std::optional<ElicitationHandler>` | `nullopt` | Equivalent to calling `SetElicitationHandler` right after construction; derives the `elicitation.form` capability | `ClientOptions.hpp:42`; `McpClient.cpp:620`, `373-377` |
| `url_elicitation_handler` | `std::optional<UrlElicitationHandler>` | `nullopt` | Equivalent to calling `SetUrlElicitationHandler` right after construction; derives the `elicitation.url` capability | `ClientOptions.hpp:43`; `McpClient.cpp:621`, `378-382` |
| `sampling_handler` | `std::optional<SamplingHandler>` | `nullopt` | Derives the `sampling` capability and installs the inbound handler during construction; the type is deprecated by SEP-2577 | `ClientOptions.hpp:44`; `McpClient.cpp:618`, `634`, `383-385` |
| `roots_handler` | `std::optional<RootsHandler>` | `nullopt` | Derives the `roots.list_changed` capability and installs the handler; the type is deprecated | `ClientOptions.hpp:45`; `McpClient.cpp:619`, `635`, `386-390` |
| `connect_mode` | `ConnectMode` | `ConnectMode::Auto` | Three modes, see the next section | `ClientOptions.hpp:47`; `McpClient.cpp:411-473` |
| `pin_protocol_version` | `std::optional<std::string>` | `nullopt` | Read only by `Pin`; when unset `kLatestProtocolVersion` is used | `ClientOptions.hpp:48`; `McpClient.cpp:429-434` |
| `initialization_timeout` | `std::chrono::seconds` | `60` | Bounds the `initialize` handshake only (the `Legacy` mode and the `Auto` fallback path) | `ClientOptions.hpp:50`; `McpClient.cpp:415-417`, `461-463`, `587-605` |
| `discover_probe_timeout` | `std::chrono::seconds` | `5` | Bounds the `server/discover` probe of `Auto` mode only | `ClientOptions.hpp:51`; `McpClient.cpp:444-446`, `475-500` |
| `max_total_timeout` | `std::chrono::seconds` | `0` (unlimited) | Handed to the session handler as the per-request total budget (the deadline progress renewal may never cross) | `ClientOptions.hpp:53`; `McpClient.cpp:627` |
| `input_required_config` | `std::optional<InputRequiredConfig>` | `InputRequiredConfig{}` (already set) | MRTR behaviour bundle, item by item in the table above | `ClientOptions.hpp:55-63` |
| `extensions` | `std::optional<JsonValue>` | `nullopt` | Merged into the outgoing `_meta.clientCapabilities.extensions`; to accept task results in the 2026 era it must contain `"io.modelcontextprotocol/tasks"` | `ClientOptions.hpp:65`; `McpClient.cpp:394-409`, `181-193` |
| `reinit_on_expired_session` | `bool` | `true` | Effective on the MRTR path only (see that section) | `ClientOptions.hpp:67`; `McpClient.cpp:1059-1066` |
| `span_handler` | `SpanHandler` (`std::function<void(const SpanEvent&)>`) | empty | Handed to the session handler | `ClientOptions.hpp:69`; `McpClient.cpp:628-630`; `include/mcp/SpanHooks.hpp:32` |
| `enable_tasks_extension` | `bool` | `false` | Gates `UpdateTask`/`PollTaskToCompletion`; together with `extensions` it decides whether task results are accepted | `ClientOptions.hpp:71`; `McpClient.cpp:1751-1753`, `1782-1784`, `1286-1290` |
| `enable_skills_extension` | `bool` | `false` | Gates `ListSkills`/`GetSkill`/`ReadDirectory` | `ClientOptions.hpp:72`; `McpClient.cpp:1563-1565`, `1597-1599`, `1638-1640` |
| `enable_progressive_discovery` | `bool` | `false` | Decides whether `SearchTools` transmits `query`/`pageSize` | `ClientOptions.hpp:73`; `McpClient.cpp:1263-1266` |
| `enable_etag_cache` | `bool` | **`true`** | Opposite of the server default; additionally requires the process-wide `IsEtagCacheEnabled()` to be true (that switch is rewritten by `ServerOptions::enable_etag_cache`) | `ClientOptions.hpp:75`; `McpClient.cpp:1228`, `1463-1467` |
| `cache_auth_context` | `std::function<std::string()>` | empty | Any change of its return value clears the private-scope cache, giving per-tenant isolation | `ClientOptions.hpp:77`; `McpClient.cpp:1165-1176` |
| `on_transport_close` | `std::function<void()>` | empty | Attached only when the transport derives from `TransportBase`, otherwise a Warning is logged and it is ignored; exceptions inside the callback are swallowed | `ClientOptions.hpp:79`; `McpClient.cpp:643-661` |
| `on_transport_error` | `std::function<void(std::string_view)>` | empty | Same as above, exceptions swallowed too | `ClientOptions.hpp:80`; `McpClient.cpp:662-670` |
| `DeriveClientCapabilities` | `std::optional<ClientCapabilities> DeriveClientCapabilities(const ClientOptions&)` (free function) | `nullopt` when everything is empty | Public derivation entry point, usable to self-check the declared set | `ClientOptions.hpp:84-85`; `McpClient.cpp:360-392` |

## Version negotiation and `ConnectMode`

| Mode | Value | Behaviour | Evidence |
|---|---|---|---|
| `Auto` | `ConnectMode::Auto` (default) | `server/discover` probe first (`discover_probe_timeout`); on success it is a modern direct connection with the version picked by `SelectSharedVersion`; on failure it falls back to the `initialize` handshake | `ClientOptions.hpp:32-36`, `47`; `McpClient.cpp:444-473` |
| `Legacy` | `ConnectMode::Legacy` | `initialize` + `notifications/initialized` only, `is_modern=false` | `ClientOptions.hpp:34`; `McpClient.cpp:414-427`, `587-609` |
| `Pin` | `ConnectMode::Pin` | Sends nothing: sets the version to `pin_protocol_version` (or `kLatestProtocolVersion`) and synthesizes a `discover` result | `ClientOptions.hpp:35`, `48`; `McpClient.cpp:429-442` |

`Auto` handles probe failures per transport class (`include/mcp/client/ClientOptions.hpp:32-36`; `src/client/McpClient.cpp:174-179`, `475-590`):

- **stdio-like** (RTTI name matching `InMemoryTransportImpl` / `StdioClientSessionTransport`): a probe timeout or exception is always read as "the peer has no discover support" and falls back to `initialize` silently.
- **network transports**: a probe timeout throws `RequestTimeout` (`"server/discover timed out"`) and any exception throws `ConnectionClosed` (`"server/discover failed: …"`); no fallback.
- On `-32022` (`UnsupportedProtocolVersion`) whose `data.supported` contains `kLatestProtocolVersion`, one corrective retry is made; a second rejection is a hard error (`McpError(UnsupportedProtocolVersion)`).
- On `-32022` where `supported` lacks the latest version but holds another modern version, `UnsupportedProtocolVersion` is thrown directly; when `supported` contains no modern version at all, it falls back to `initialize`.
- A `discover` answer that fails to parse, or shares no version with the client's declared set, is likewise treated as a probe failure (`McpClient.cpp:576-584`).

## Error semantics and timeout ownership

| Situation | Observable behaviour | Evidence |
|---|---|---|
| Typed methods on the `DoSendRequest` path (list methods, `Complete`, `GetTask`/`UpdateTask`/`CancelTask`, `Ping`, `Discover`, `SetLoggingLevel`) receive a protocol error | Throws `McpError(code, message)`; **the error `data` is discarded** | `McpClient.cpp:62-90` |
| MRTR path (`CallTool`, `GetPrompt`, `ReadResource`, `GetSkill`, `ReadDirectory`, `CallToolAsTask`) receives a protocol error | Throws `McpError(code, message, data)`, keeping `data` | `McpClient.cpp:1101-1108` |
| Bare `SendRequest` (`RequestHandle` overload) receives a protocol error | **No throw**: the `future` yields the `ErrorData` alternative of `RequestOutcome` | `McpClient.cpp:1853-1864` |
| `CallTool` gets a task handle | Automatic `PollTaskToCompletion`; `failed`/`cancelled` are rethrown as `InternalError`; in the modern era without the tasks extension declared it throws `HandlerError` | `McpClient.cpp:1280-1310` |
| Pagination does not converge | `ListToolsAll`/`SearchTools`/`ListResourcesAll`/`ListResourceTemplatesAll`/`ListPromptsAll` throw `ProtocolViolation` past 64 pages (`"list pagination did not converge within 64 pages"`) | `McpClient.cpp:52`, `143-171`, `1240-1249` |
| Extension flag missing | `ListSkills`/`GetSkill`/`ReadDirectory` throw `MethodNotFound`; `UpdateTask`/`PollTaskToCompletion` throw `MethodNotFound` as well | `McpClient.cpp:1563`, `1597`, `1638`, `1751`, `1782` |
| Timeout ownership | Typed methods **ignore** `RequestOptions::timeout`: `CallTool`/`CallToolAsTask` use `input_required_config->round_timeout` (default 600s), `GetPrompt`/`ReadResource`/`GetSkill`/`ReadDirectory` use `read_timeout_ms` else 60s, list methods plus `Complete`/`Discover` use 60s, `GetTask`/`UpdateTask`/`CancelTask` use 600s, `Ping` uses 10s | `McpClient.cpp:21-22`, `1335-1342`, `1546-1550`, `1704-1708`, `1815-1835` |

::: tip
For a strict single-request timeout, or to obtain `ErrorData`, use the `SendRequest(method, params, RequestOptions{...})` handle overload; the typed methods' timeout semantics are fixed per the table above and cannot be overridden through `RequestOptions::timeout`.
:::

## Skill verification helpers

Public free functions for consistency checks before and after adopting a skill (`include/mcp/client/SkillVerify.hpp`):

| Symbol | Signature/Type | Default/Returns | Notes | Evidence |
|---|---|---|---|---|
| `kMaxSkillFiles` | `constexpr size_t` | `512` | Per-skill file count limit | `SkillVerify.hpp:12` |
| `kMaxSkillTotalBytes` | `constexpr int64_t` | `16 * 1024 * 1024` | Per-skill total byte limit | `SkillVerify.hpp:13` |
| `CheckSkillLimits` | `bool CheckSkillLimits(const Skill&, std::string& error_out)` | `false` + reason | Validates file count and total size | `SkillVerify.hpp:15` |
| `VerifyFileBytes` | `bool VerifyFileBytes(const SkillResource&, std::string_view bytes, std::string& error_out)` | `false` + reason | Compares digest and length | `SkillVerify.hpp:16` |
| `VerifyFrontmatter` | `bool VerifyFrontmatter(const Skill& advertised, const JsonValue& fetched, std::string& error_out)` | `false` + reason | Compares advertised and fetched frontmatter | `SkillVerify.hpp:17` |
| `IsApprovalStillValid` | `bool IsApprovalStillValid(const Skill& approved, const Skill& current)` | `bool` | Whether an already approved skill is still valid | `SkillVerify.hpp:18` |
| `ApprovalKey` | `std::string ApprovalKey(const std::string& host_label, const Skill&)` | key string | Key for approval records | `SkillVerify.hpp:19` |

## Internal symbols (not user API — never cite them as interface)

- `SendRequestWithMrtr` / `SendRequestWithMrtrOnce` / `RecoverExpiredSession` / `WireClientHandlers` / `CacheIfHinted` / `MaybeAttachEtag` / `NegotiationResult negotiation_` are all private `McpClient` members (`McpClient.hpp:152-226`); MRTR surfaces to users only as `ClientOptions::input_required_config` and the returned `input_required` result.
- `VersionNegotiation::Negotiate/ProbeDiscover/HandshakeInitialize` and `NegotiationResult` are declared in a public header (`include/mcp/client/VersionNegotiation.hpp:9-36`) but belong to the negotiation machinery; users should influence them only through `ConnectMode` and the timeout fields.
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
