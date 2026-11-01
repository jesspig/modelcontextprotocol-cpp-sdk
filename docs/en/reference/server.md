# Server Facade (`McpServer`) Reference

This page is the **single authoritative source for signatures and field tables** of `McpServer`, `RequestContext<TParams>` and `ServerOptions`; step-by-step usage lives in later how-to pages, which only link here and never duplicate the tables. Protocol semantics: `/en/learn/protocol-versions`; data types: `/en/reference/types`; error codes: `/en/reference/errors`; transport construction: `/en/reference/transports`.

## Factory and lifecycle

| Symbol | Signature/Type | Default/Returns | Notes | Evidence |
|---|---|---|---|---|
| `McpServer::Create` | `static std::unique_ptr<McpServer> Create(std::shared_ptr<ITransport> transport, const ServerOptions& options = {})` | `std::unique_ptr<McpServer>` | Construction already wires inbound handlers, derives capabilities, stamps the negotiated version, calls `transport->Start()` and spawns the message loop; requests can be served without `Run()` | `include/mcp/server/McpServer.hpp:34-36`; `src/server/McpServer.cpp:646-652`, `654-735` |
| `Run` | `void Run()` | — | Blocks on a condition variable until `Close()` clears `running_`; message processing happens on the session thread, `Run()` is only a main-thread park point | `McpServer.hpp:40`; `McpServer.cpp:737-740` |
| `Close` | `void Close()` | — | First waits for every in-flight async call (including URL-elicitation watchdogs), then closes the session handler, finally wakes `Run()` | `McpServer.hpp:41`; `McpServer.cpp:742-756` |
| `IsRunning` | `bool IsRunning() const` | `false` until the first `initialize`/`server/discover` | Means "handshake done and handler alive"; registration APIs use it to decide whether to emit `*/list_changed` | `McpServer.hpp:117`; `McpServer.cpp:3357-3359`, `779-781` |
| `~McpServer` | `virtual ~McpServer() = default` | — | Virtual destructor, safe to destroy through `unique_ptr`; destruction is not equivalent to `Close()` | `McpServer.hpp:38` |

::: warning
The caller owns the transport: `Create()` only stores a `shared_ptr` and `Close()` only closes the session handler, never the transport. The server exposes no `Connect()`; only the client side needs a factory connection.
:::

## Registration API

| Symbol | Signature/Type | Default/Returns | Notes | Evidence |
|---|---|---|---|---|
| `RegisterTool` (object path) | `void RegisterTool(std::shared_ptr<McpServerTool> tool)` | `void` | Inserts into `tools_` (name-ordered map) and invalidates the tool-list cache; capabilities are re-derived afterwards | `McpServer.hpp:43`; `McpServer.cpp:758-782` |
| `RegisterTool` (lambda overload) | `void RegisterTool(std::string_view name, const ToolOptions& options, std::function<CallToolResult(const RequestContext<CallToolRequestParams>&)> fn)` | `void` | Forwards inline to `McpServerTool::Create(name, std::move(fn), options)`; behaviour is identical to the object path | `McpServer.hpp:44-50` |
| `McpServerTool::Create` | `template <typename Callable> static std::shared_ptr<McpServerTool> Create(std::string_view name, Callable&& callable, const ToolOptions& options = {})` | `shared_ptr<McpServerTool>` | Builds `McpServerToolImpl<std::decay_t<Callable>>`; when `options.input_schema` is absent it fills `{"type":"object","properties":{}}` | `include/mcp/server/McpServerTool.hpp:23-27`, `46-60`, `92-102` |
| `McpServerTool` | `virtual const Tool& ProtocolTool() const` / `virtual void InvokeAsync(const RequestContext<CallToolRequestParams>&, std::promise<CallToolResult>)` | pure virtual | Extension point for custom execution; `McpServerToolImpl::InvokeAsync` runs the callable synchronously on the calling thread and funnels exceptions into the promise | `McpServerTool.hpp:18-21`, `75-85` |
| `RegisterResource` | `void RegisterResource(std::string_view name, std::string_view uri, const ResourceOptions&, std::function<ReadResourceResult(const std::string& uri)> handler)` | `void` | Static resource; the handler receives the concrete uri | `McpServer.hpp:55-59`; `McpServer.cpp:784-822` |
| `RegisterResourceTemplate` | `void RegisterResourceTemplate(std::string_view name, std::string_view uri_template, const ResourceOptions&, std::function<ReadResourceResult(const std::string& uri, const std::map<std::string, std::string>& vars)> handler)` | `void` | The URI template is parsed and validated at registration time; the handler also gets the expanded variable map | `McpServer.hpp:61-67`; `McpServer.cpp:824-871` |
| `RegisterPrompt` | `void RegisterPrompt(std::string_view name, const PromptOptions&, std::function<GetPromptResult(const std::string& name, const std::optional<JsonValue>& args)> handler)` | `void` | `PromptOptions::arguments` is passed through to `prompts/list` verbatim | `McpServer.hpp:69-73`; `McpServer.cpp:873-906` |
| `RegisterSkill` | `void RegisterSkill(Skill skill)` | `void` | Deduplicated by `Skill::uri`; advertising the capability additionally requires `enable_skills_extension` plus an existing `resources` capability; emits no `list_changed` | `McpServer.hpp:75`; `McpServer.cpp:908-925`, `1983-1990` |
| `SetSkillsDirectoryRead` | `void SetSkillsDirectoryRead(bool enabled)` | `void` (default `false`) | Sets the skill-directory browsing flag, recorded as `directory_read` in the skills extension settings; changes the `skills/directory` result set | `McpServer.hpp:76`; `McpServer.cpp:927-933`, `1987` |
| `SetCompletionHandler` | `using CompletionHandler = std::function<CompleteResult(const CompleteRequestParams&)>; void SetCompletionHandler(CompletionHandler handler)` | `void` | Without a handler `completion/complete` returns `MethodNotFound` (`"No completion handler registered"`); the capability is advertised solely by `declare_completions` | `McpServer.hpp:86-87`; `McpServer.cpp:3089-3096`, `3111-3114`, `1962-1964` |

## Registration validation and errors

| Trigger | Throws | Message / basis | Evidence |
|---|---|---|---|
| Tool name empty, longer than 128, or containing anything outside `[A-Za-z0-9._-]` | `McpError(InvalidParams)` | `invalid tool name: <name>` | `McpServer.cpp:760-763`, `275-283` |
| Invalid `x-mcp-header` parameter annotations in `ToolOptions::input_schema` | `McpError(InvalidParams)` | `invalid_reason` produced by the annotation parser | `McpServer.cpp:764-767` |
| Duplicate tool name | `McpError(InvalidParams)` | `tool already registered: <name>` | `McpServer.cpp:770-773` |
| Empty `shared_ptr<McpServerTool>` passed in | No throw — null dereference | The implementation performs no null check (`tool->ProtocolTool()`) | `McpServer.cpp:758-759` |
| Empty resource / template / prompt handler | `McpError(InvalidParams)` | `resource handler must not be empty: <name>`, `resource template handler must not be empty: <name>`, `prompt handler must not be empty: <name>` | `McpServer.cpp:790-793`, `832-835`, `880-883` |
| Malformed URI template | `McpError(InvalidParams)` | `invalid resource template '<uri>': <error>` | `McpServer.cpp:836-841` |
| Resource name or uri_pattern (templates included) collides with an existing entry | `McpError(InvalidParams)` | `resource already registered: <name>` / `resource uri already registered: <uri>`; static resources and templates share one conflict table | `McpServer.cpp:805-814`, `854-863` |
| Duplicate prompt name | `McpError(InvalidParams)` | `prompt already registered: <name>` | `McpServer.cpp:893-898` |
| Invalid skill uri | `McpError(InvalidParams)` | `invalid skill uri: <uri>`; the uri must end with `/SKILL.md` and have a `scheme://host/…` shape | `McpServer.cpp:909-912`, `125-139` |
| Duplicate skill uri | `McpError(InvalidParams)` | `skill already registered: <uri>` | `McpServer.cpp:915-919` |
| Empty completion handler | `McpError(InvalidParams)` | `completion handler must not be empty` | `McpServer.cpp:3089-3093` |

## Reverse requests: `Elicit` / `ElicitUrl`

| Symbol | Signature/Type | Default/Returns | Notes | Evidence |
|---|---|---|---|---|
| `Elicit` | `std::future<ElicitResult> Elicit(const ElicitRequestParams& params, std::string_view session_id = {})` | `future<ElicitResult>` | Form-mode reverse request; timeout comes from `options.input_required_config->round_timeout`, otherwise 600s | `McpServer.hpp:78-79`; `McpServer.cpp:1138-1141`, `1173-1177`, `48` |
| `ElicitUrl` | `std::future<ElicitResult> ElicitUrl(const std::string& url, const std::string& message, std::chrono::seconds timeout = std::chrono::seconds(600), std::string_view session_id = {})` | `future<ElicitResult>` | URL mode (SEP-1034); in the legacy era it mints an auto-incremented `elicitationId` (`elicit-<n>`) and waits for `notifications/elicitation/complete`, with the watchdog resolving as `RequestTimeout` | `McpServer.hpp:80-83`; `McpServer.cpp:1193-1214`, `1246-1279` |

The trailing session argument (both APIs) is resolved by `EffectiveSessionId()` in this order: explicit non-empty `session_id` > global capabilities already set (returns the empty string, i.e. use global) > exactly one non-empty entry in `client_sessions_`, which becomes the fallback > otherwise the empty string (session capabilities unreadable). Under stateful HTTP capabilities land only in `client_sessions_`, so a no-argument call falls into the "global empty" branch and wrongly throws `-32021`; therefore tool handlers must pass `RequestContext::SessionId()`. Evidence: `src/server/McpServer.cpp:1060-1068`, `1140`, `1198`.

Protocol-era differences (important):

- **Modern era** (negotiated version `>= 2026-07-28`): neither call **sends** `elicitation/create`; both throw the implementation-internal `ElicitationInputRequired` carrying `inputRequests["elicitation"]`. The three `HandleCallTool` paths catch it and turn it into `CallToolResult::input_required`, fulfilled by the client's `SetElicitationHandler` / `SetUrlElicitationHandler`. The `timeout` argument of `ElicitUrl` is ignored on this branch. Evidence: `McpServer.cpp:1142-1149`, `1199-1212`, `2630-2632`.
- **Legacy era**: client capabilities are checked first; a missing declaration throws `McpError(MissingRequiredClientCapability)` with `data.requiredCapabilities.elicitation` containing an empty `form`/`url` object. For form mode "declared" means `elicitation.form` present **or** `url` not declared; URL mode requires `elicitation.url`. Evidence: `McpServer.cpp:1151-1167`.
- If the response carries a known JSON-RPC error code, the `Elicit` future resolves by throwing `McpError(code, message)` (`McpServer.cpp:1179-1188`).

::: warning
`ElicitationInputRequired` exists only inside the anonymous namespace of `src/server/McpServer.cpp:100-105` and appears in no exported header: user code must not catch it by name and may only observe the `input_required` field of the tool result.
:::

## Notification and push API

| Symbol | Signature/Type | Default/Returns | Notes | Evidence |
|---|---|---|---|---|
| `SendToolListChanged` | `void SendToolListChanged()` | — | Delegates to `SendListChangedNotification(tools/list_changed)` | `McpServer.hpp:89`; `McpServer.cpp:935-937` |
| `SendResourceListChanged` | `void SendResourceListChanged()` | — | Same, `resources/list_changed` | `McpServer.hpp:90`; `McpServer.cpp:939-941` |
| `SendPromptListChanged` | `void SendPromptListChanged()` | — | Same, `prompts/list_changed` | `McpServer.hpp:92`; `McpServer.cpp:949-951` |
| `SendResourceUpdated` | `void SendResourceUpdated(const std::string& uri)` | — | Delivered only to subscribers of that uri via `NotifySubscribers`; modern-era only | `McpServer.hpp:91`; `McpServer.cpp:943-947` |
| (routing rule) | internal `SendListChangedNotification` | — | Modern era → `NotifySubscribers`; legacy era → broadcast `SendNotification` | `McpServer.cpp:953-959` |
| `SendLoggingMessage` | `void SendLoggingMessage(LoggingLevel level, std::string_view data)` | — | **Returns immediately without sending in the modern era**; in the legacy era it is filtered by the level recorded through `logging/setLevel` | `McpServer.hpp:93`; `McpServer.cpp:961-972` |
| `SendLoggingMessage` (overload) | `void SendLoggingMessage(LoggingLevel level, std::string_view data, std::optional<LoggingLevel> min_level)` | `min_level = nullopt` | With `min_level` set, that bound alone filters and sends (overriding the `setLevel` record); with `nullopt` it falls back to the two-argument behaviour | `McpServer.hpp:94`; `McpServer.cpp:974-982` |
| `SendTaskStatus` | `void SendTaskStatus(std::string_view task_id, TaskStatus status)` | — | Looks the owner session up in `task_store`, then routes through subscriptions or broadcast according to that session's negotiated version; needs `enable_tasks_extension` + `task_store` for any usable task | `McpServer.hpp:96`; `McpServer.cpp:1004-1012`, `285-303` |
| `SendProgress` | `void SendProgress(const ProgressToken& token, double progress, std::optional<double> total = std::nullopt, std::optional<std::string> message = std::nullopt)` | — | Modern era first tries the request-stream buffer (`BufferRequestStreamNotification`); otherwise sends the global `notifications/progress` | `McpServer.hpp:97-99`; `McpServer.cpp:1090-1110` |
| `SetCacheable` | `void SetCacheable(std::function<std::optional<CacheHint>(std::string_view method)> resolver)` | — | Replaces the cache hook at runtime, taking precedence over `ServerOptions::cache_hints`; hints are still validated in the modern era | `McpServer.hpp:95`; `McpServer.cpp:640-644`, `594-621` |

`TaskStatus` values: `Pending`, `Working`, `Completed`, `Failed`, `Cancelled`, `InputRequired` (`include/mcp/server/McpTaskStore.hpp:16-23`).

## Client state queries

| Symbol | Signature/Type | Default/Returns | Notes | Evidence |
|---|---|---|---|---|
| `GetClientCapabilities` | `std::shared_ptr<const ClientCapabilities> GetClientCapabilities() const` | `nullptr` before handshake | Global slot; written both by `initialize` (empty session id) and by modern `server/discover` (`_meta.clientCapabilities`) | `McpServer.hpp:106`; `McpServer.cpp:3296-3299`, `3190-3201`, `3150-3157` |
| `GetClientCapabilities` (per session) | `GetClientCapabilities(std::string_view session_id) const` | `nullptr` for unknown sessions | Looks `client_sessions_` up exactly and applies **no** `EffectiveSessionId` fallback | `McpServer.hpp:107-108`; `McpServer.cpp:3301-3304`, `1028-1035` |
| `GetClientInfo` / `GetClientInfo(session_id)` | `std::shared_ptr<const Implementation>` | `nullptr` before handshake | The global slot is written only by `initialize`; with a modern direct connection (discover) that carries no `clientInfo` it stays `nullptr`, so read `_meta` from the `RequestContext` instead | `McpServer.hpp:109-111`; `McpServer.cpp:3306-3314`, `3194-3200` |
| `GetCapabilities` | `ServerCapabilities GetCapabilities() const` | **returned by value** | Copy taken under the lock; callers must not hold a reference; capabilities have no manual assignment entry point | `McpServer.hpp:114`; `McpServer.cpp:3325-3328` |
| `GetNegotiatedProtocolVersion` | `std::string GetNegotiatedProtocolVersion() const` / `(std::string_view session_id) const` | **empty string** when nothing was negotiated and `protocol_version` is unset | The per-session overload falls back to the global version for unknown sessions; `IsModernProtocolVersion("")` is false, so an empty string is treated as legacy. A legacy `initialize` without a version declaration yields `kDefaultNegotiatedProtocolVersion` (`2025-03-26`); a declared version that is absent from the supported table or belongs to the modern era yields `kLegacyProtocolVersion` (`2025-11-25`) | `McpServer.hpp:112-113`; `McpServer.cpp:3316-3324`, `3213-3223`; `src/protocol/McpSessionHandler.cpp:681-689` |
| `IsMrtrSupported` | `bool IsMrtrSupported() const` | `false` | Non-stateless transport and the session resolved by `EffectiveSessionId({})` declares `elicitation` | `McpServer.hpp:116`; `McpServer.cpp:3351-3355` |
| `GetServerCard` | `ext::ServerCard GetServerCard() const` | value | Throws `McpError(HandlerError, "server card is disabled")` when `enable_server_card=false`; without `server_card` a card is synthesized from `server_info`/`kSdkVersion`/the full version table with a placeholder endpoint URL | `McpServer.hpp:115`; `McpServer.cpp:3330-3349` |
| `SessionId` | `const std::string& SessionId() const` | random 8-byte hex generated at construction | **Instance-level** identifier used only to anchor task ownership: task ids look like `task-<session>-<16-byte hex>` | `McpServer.hpp:118`; `McpServer.cpp:654-660`, `1015-1017` |
| `RequestContext::SessionId` | `std::string_view SessionId() const` | empty string for stdio / stateless HTTP | **Request-level** MCP session id (stateful HTTP: the `mcp-session-id` header value) | `include/mcp/server/RequestContext.hpp:62`; `src/server/McpServer.cpp:2317-2319` |
| `GetSessionHandler` | `McpSessionHandler& GetSessionHandler()` | reference | Escape hatch exposing the session handler; ordinary code does not need it | `McpServer.hpp:120` |

::: danger
`McpServer::SessionId()` and `RequestContext::SessionId()` are unrelated and must never be interchanged: the first is a per-process random instance identifier (task owner), the second is the protocol session of the current request. Passing `McpServer::SessionId()` to `Elicit`/`ElicitUrl` resolves the wrong session capabilities.
:::

## `RequestContext<TParams>` members

| Symbol | Signature/Type | Default/Returns | Notes | Evidence |
|---|---|---|---|---|
| `Params` | `const TParams& Params() const` | reference | Deserialized request parameters (`CallToolRequestParams` on the tool path) | `RequestContext.hpp:58` |
| `Server` | `McpServer& Server() const` | reference | Entry point to the reverse-call facade (`Elicit`, `SendProgress`, …) | `RequestContext.hpp:59` |
| `GetRequest` | `const mcp::JsonRpcRequest& GetRequest() const` | reference | Raw JSON-RPC request including `id`/`method`/`params`/`meta` | `RequestContext.hpp:60` |
| `LogLevel` | `std::optional<LoggingLevel> LogLevel() const` | `nullopt` without `_meta.logLevel` | Read from `_meta["io.modelcontextprotocol/logLevel"]`; integer and string forms both parse, parse failures are silently ignored | `RequestContext.hpp:43-55`, `61` |
| `SessionId` | `std::string_view SessionId() const` | empty string | Request-level session id; the object holds a `std::string` copy, so asynchronous handlers may read it safely | `RequestContext.hpp:62`, `84` |
| `Log` | `void Log(LoggingLevel level, std::string_view data) const` | — | Two gates: if a per-request `logLevel` is required but absent, the message is dropped; anything below `LogLevel()` is dropped | `RequestContext.hpp:64-70` |
| `IsCancellationRequested` | `bool IsCancellationRequested() const` | `false` without a flag | Cooperative cancellation; `notifications/cancelled` and `tasks/cancel` set the same flag, so plain tool handlers can poll it too | `RequestContext.hpp:72-74`; `McpServer.cpp:2313-2319` |

`using LogFn = std::function<void(LoggingLevel, std::string_view)>` (`RequestContext.hpp:25`). The constructor is public but exists for SDK use: `HandleCallTool` and friends inject `log_fn`, the cancellation flag, `require_request_log_level` (true in the modern era) and the session id. User code must never build a `RequestContext`; obtain one from the handler signature (`RequestContext.hpp:27-41`).

## `ServerOptions` field by field

| Field | Type | Default | Notes / prerequisites | Evidence |
|---|---|---|---|---|
| `server_info` | `std::optional<Implementation>` | `nullopt` → `{"mcp-server", kSdkVersion}` | Server identity in the `initialize`/`discover` response body | `ServerOptions.hpp:30`; `McpServer.cpp:635-638` |
| `protocol_version` | `std::optional<std::string>` | `nullopt` | Pins the wire codec version and overrides the negotiation outcome (written per session); does not change the capability set | `ServerOptions.hpp:31`; `McpServer.cpp:662-663`, `682-684`, `3159-3165`, `3209-3211` |
| `server_instructions` | `std::optional<std::string>` | `nullopt` | Sent as `instructions` with `initialize`/`discover` | `ServerOptions.hpp:32`; `McpServer.cpp:3230-3232`, `3174-3176` |
| `initialization_timeout` | `std::chrono::seconds` | `60` | **The server implementation currently has no read site at all** (only the client's field of the same name is effective); do not rely on it to bound the handshake | `ServerOptions.hpp:34`; no reference anywhere under `src/server` |
| `cache_hints` | `std::optional<std::map<std::string, CacheHint, std::less<>>>` | `nullopt` | Static per-method map of `ttlMs`/`cacheScope`; in the modern era a missing or invalid value throws `InvalidParams` | `ServerOptions.hpp:37`; `McpServer.cpp:608-614`, `66-88` |
| `cache_hint_resolver` | `std::function<std::optional<CacheHint>(std::string_view method)>` | empty | Takes precedence over `cache_hints`; replaceable at runtime via `SetCacheable` | `ServerOptions.hpp:76`; `McpServer.cpp:677-680`, `598-607` |
| `request_state_verifier` | `std::function<bool(std::string_view)>` | empty | MRTR `requestState` verifier; when set, the built-in `request_state_key` verifier is not installed | `ServerOptions.hpp:39`; `McpServer.cpp:686-687` |
| `request_state_key` | `std::optional<std::string>` | `nullopt` | HMAC key: setting it installs the built-in verifier and signs `iat` into the state when returning `input_required` | `ServerOptions.hpp:41`; `McpServer.cpp:688-696`, `2403-2410`, `2661-2667` |
| `request_state_ttl` | `std::chrono::seconds` | `0` (no expiry check) | Applies only to the built-in verifier, comparing the payload `iat` | `ServerOptions.hpp:43`; `McpServer.cpp:690-695` |
| `input_required_config` | `std::optional<InputRequiredConfig>` | `nullopt` | MRTR/elicitation parameter bundle | `ServerOptions.hpp:45-49` |
| `input_required_config->max_rounds` | `int` | `10` | **No read site on the server** (the round cap only applies to the client MRTR loop) | `ServerOptions.hpp:46`; no reference in `src/server` |
| `input_required_config->round_timeout` | `std::chrono::seconds` | `600` | Used as the `Elicit` wait timeout; overrides the 600s default when configured | `ServerOptions.hpp:47`; `McpServer.cpp:1173-1177` |
| `declare_logging` | `bool` | `false` | Advertises the `logging` capability even with no handler yet | `ServerOptions.hpp:51`; `McpServer.cpp:1959-1961` |
| `declare_completions` | `bool` | `false` | Advertises the `completions` capability regardless of whether a handler is registered | `ServerOptions.hpp:52`; `McpServer.cpp:1962-1964` |
| `on_method_called` | `std::function<void(std::string_view method)>` | empty | Chained together with `on_request`; both fire | `ServerOptions.hpp:54`; `McpServer.cpp:698-704` |
| `on_client_connected` | `std::function<void(const Implementation& client_info)>` | empty | Fires once `initialize` exposes `clientInfo` | `ServerOptions.hpp:55`; `McpServer.cpp:3204-3206` |
| `on_initialized` | `std::function<void()>` | empty | Fires on the first `notifications/initialized` for that session (duplicates do not re-fire); exceptions inside the callback are swallowed and logged | `ServerOptions.hpp:56`; `McpServer.cpp:1414-1424` |
| `on_protocol_error` | `std::function<void(std::string_view error)>` | empty | Chained with `on_error`, receives only the error message | `ServerOptions.hpp:57`; `McpServer.cpp:708-714` |
| `on_request` | `std::function<void(std::string_view method, const JsonRpcRequest&)>` | empty | Every inbound request | `ServerOptions.hpp:59`; `McpServer.cpp:698-704` |
| `on_response` | `std::function<void(const JsonRpcResponse&)>` | empty | Every outbound success response | `ServerOptions.hpp:60`; `McpServer.cpp:705-707` |
| `on_error` | `std::function<void(const JsonRpcErrorResponse&)>` | empty | Every outbound error response | `ServerOptions.hpp:61`; `McpServer.cpp:708-714` |
| `on_notification` | `std::function<void(const JsonRpcNotification&)>` | empty | Every inbound notification | `ServerOptions.hpp:62`; `McpServer.cpp:715-717` |
| `span_handler` | `SpanHandler` (`std::function<void(const SpanEvent&)>`) | empty | Observability hook, moved into the session handler | `ServerOptions.hpp:64`; `include/mcp/SpanHooks.hpp:32`, `McpServer.cpp:718-720` |
| `incoming_filters` / `outgoing_filters` | `std::shared_ptr<FilterPipeline>` | `nullptr` | Message pipelines injected once, at session-handler construction | `ServerOptions.hpp:66-67`; `McpServer.cpp:664-667` |
| `on_transport_close` | `std::function<void()>` | empty | Attached only when the transport is a `TransportBase` subclass (`dynamic_cast` failure is silently ignored) | `ServerOptions.hpp:69`; `McpServer.cpp:721-728` |
| `on_transport_error` | `std::function<void(std::string_view)>` | empty | Same as above | `ServerOptions.hpp:70`; `McpServer.cpp:721-728` |
| `max_tool_input_elements` | `std::optional<std::size_t>` | `nullopt` (unlimited) | Objects counted by keys, arrays by length, scalars as 1; exceeding it yields an `is_error` tool result rather than a protocol error | `ServerOptions.hpp:72`; `McpServer.cpp:2323-2328`, `563-568` |
| `supported_protocol_versions` | `std::optional<std::vector<std::string>>` | `nullopt`/empty → full `kProtocolVersions` | Narrows the `discover` answer, the `initialize` candidate set and the error `data.supported` simultaneously | `ServerOptions.hpp:74`; `McpServer.cpp:624-633`, `676`, `3216-3221` |
| `subscriptions_enabled` | `bool` | `true` | When `false`, `subscriptions/listen` returns `MethodNotFound` (`"subscriptions are disabled"`); non-modern versions already return `MethodNotFound` | `ServerOptions.hpp:75`; `McpServer.cpp:3239-3249` |
| `task_store` | `std::shared_ptr<IMcpTaskStore>` | `nullptr` | Persistence backend for task execution; without it no task handle is ever produced | `ServerOptions.hpp:78`; `McpServer.cpp:2342-2348`, `2414` |
| `enable_tasks_extension` | `bool` | `false` | Declares the extension and admits `tasks/*`; with a `task_store` but this flag off, `tasks/*` is rejected | `ServerOptions.hpp:80`; `McpServer.cpp:1965-1970`, `1514`, `1556`, `1796`, `2347-2348` |
| `enable_skills_extension` | `bool` | `false` | Admits `skills/list`, `skills/get`, `skills/directory` (otherwise `MethodNotFound`) | `ServerOptions.hpp:81`; `McpServer.cpp:1983-1990`, `2954`, `2995`, `3038` |
| `enable_apps_extension` | `bool` | `false` | Declares the apps extension and admits apps-related requests | `ServerOptions.hpp:82`; `McpServer.cpp:1971-1976`, `2766` |
| `enable_server_card` | `bool` | `false` | Declares the server-card extension; required for `GetServerCard()` to work | `ServerOptions.hpp:83`; `McpServer.cpp:1977-1982`, `3330-3333` |
| `server_card` | `std::optional<ext::ServerCard>` | `nullopt` | Used verbatim (after validation) when provided; otherwise a default card is synthesized | `ServerOptions.hpp:84`; `McpServer.cpp:3335-3348` |
| `enable_etag_cache` | `bool` | `false` | **Process-wide side effect**: the constructor calls `SetEtagCacheEnabled`, changing etag behaviour of every client/server in the same process | `ServerOptions.hpp:85`; `McpServer.cpp:661`, `include/mcp/McpTypes.hpp:263-264` |
| `enable_progressive_discovery` | `bool` | `false` | When off, `tools/list` serves a whole-table cache and ignores `query`/`pageSize`; when on, filtering and paging apply | `ServerOptions.hpp:86`; `McpServer.cpp:2014-2043` |
| `enable_file_input` | `bool` | `false` | Expands `FileRef` entries (`file://`, `resource://`) inside `arguments`; 8MB per slice, 256KB inline (`kFileInputMaxBytes`/`kFileInputInlineMaxBytes`) | `ServerOptions.hpp:87`; `McpServer.cpp:2153-2212`, `503-504`, `527-547` |
| `enable_triggers_events` | `bool` | `false` | `EmitTriggerEvent` returns silently while this is off (register/unregister are not gated) | `ServerOptions.hpp:88`; `McpServer.cpp:1122-1136` |
| `TasksImplicitlyEnabled()` | `[[deprecated]] bool` (method) | computed | Deprecated implicit derivation (`enable_tasks_extension \|\| task_store`); set `enable_tasks_extension` explicitly instead | `ServerOptions.hpp:90-92` |
| `SkillsImplicitlyEnabled()` | `[[deprecated]] bool` (method) | `enable_skills_extension` | Deprecated; set `enable_skills_extension` explicitly instead | `ServerOptions.hpp:93-95` |

## Capability derivation

Capabilities have no assignment entry point and are derived purely from registrations and flags (re-run after every registration: `McpServer.cpp:777-778`, `817-818`, `866-867`, `901-902`, `923-924`, `932`):

| Capability | Trigger | Evidence |
|---|---|---|
| `tools.list_changed` | `tools_` non-empty | `McpServer.cpp:1946-1949` |
| `resources.subscribe` + `list_changed` | `resources_` non-empty (static and templates share the table) | `McpServer.cpp:1950-1954` |
| `prompts.list_changed` | `prompts_` non-empty | `McpServer.cpp:1955-1958` |
| `logging` | `declare_logging` only | `McpServer.cpp:1959-1961` |
| `completions` | `declare_completions` only | `McpServer.cpp:1962-1964` |
| `extensions["io.modelcontextprotocol/tasks"]` | `enable_tasks_extension` | `McpServer.cpp:1965-1970` |
| `extensions[apps]` / `extensions[server-card]` | `enable_apps_extension` / `enable_server_card` (ids: `ext::kAppsExtensionId`, `ext::kServerCardExtensionId`) | `McpServer.cpp:1971-1982` |
| `extensions[skills]` | `enable_skills_extension` **and** at least one registered skill **and** an existing `resources` capability; `directory_read` comes from `SetSkillsDirectoryRead` | `McpServer.cpp:1983-1990` |

## Error semantics of tool calls

Based on the synchronous path in `src/server/McpServer.cpp` (no `task_store`, or the tool is not task-declared):

| Situation | Observable behaviour | Evidence |
|---|---|---|
| Handler throws `McpError` | Result has `is_error=true` with a single `text` content equal to `ErrorMessage()`; **the error code and `data` are discarded** | `McpServer.cpp:2633-2636`, `496-501` |
| Handler throws another `std::exception` or an unknown type | Outgoing text is generalized to `Error executing tool <name>`; the original `what()` goes only to internal logging (logging is off by default, so it is invisible externally) | `McpServer.cpp:2637-2650`, `549-551` |
| Handler throws `ElicitationInputRequired` (raised by `Elicit`/`ElicitUrl` in the modern era) | Converted into a `resultType=input_required` result whose `inputRequests["elicitation"]` carries the original arguments | `McpServer.cpp:2630-2632`, `1142-1149` |
| `arguments` violate `input_schema` | The SDK validates against the JSON Schema itself and returns `is_error` + `"invalid arguments: <reason>"`; the handler never runs | `McpServer.cpp:2616-2623`, `553-561`; task paths `2359-2362`, `2508-2515` |
| Argument element count exceeds `max_tool_input_elements` | `is_error` + `"tool arguments exceed maximum input element count"` | `McpServer.cpp:2323-2328` |
| Unknown tool name | Protocol error `InvalidParams`, `tool not found: <name>` (not an `is_error` result) | `McpServer.cpp:2145-2151` |
| Legacy era, handshake incomplete | Protocol error `InvalidRequest`, `Server not initialized`; the modern era skips this check | `McpServer.cpp:587-592`, `2086` |
| Handler exception on the task path | No `is_error`: `McpError` goes through `FailTask` keeping code/message/data, other exceptions become `InternalError` + `Error executing tool <name>` | `McpServer.cpp:2523-2552`, `2494-2504` |

## Internal symbols (not user API — never cite them as interface)

- `ElicitationInputRequired`, `ToolExecutionFailureMessage`, `IsValidToolName`, `IsSkillEntryUri`, `kElicitTimeout` (600s) and `kFileInputMaxBytes` (8MB) live in the anonymous namespace of `src/server/McpServer.cpp` and are exported by no header.
- `detail::MintRequestState` / `detail::VerifyRequestState` / `detail::DecodeRequestStatePayload` (`include/mcp/server/RequestState.hpp:43-98`) are internal `mcp::detail` HMAC/base64url helpers; users only configure `request_state_key`/`request_state_verifier`.
- The `Wire*`, `Handle*`, `EffectiveSessionId` and `ClientCapabilitiesFor` members of `McpServer` are private (`McpServer.hpp:122-188`) and are not part of any public contract.
- `ResolveToolParamAnnotations` is a public method but returns `std::vector<detail::McpParamAnnotation>` (`McpServer.hpp:52-53`); it is an advanced facility that ordinary registrations never need.

## Source files behind this page

- `include/mcp/server/McpServer.hpp`
- `include/mcp/server/ServerOptions.hpp`
- `include/mcp/server/RequestContext.hpp`
- `include/mcp/server/McpServerTool.hpp`
- `include/mcp/server/McpTaskStore.hpp`
- `include/mcp/server/RequestState.hpp` (implementation-internal)
- `include/mcp/McpTypes.hpp` (`ToolOptions`, `ResourceOptions`, `PromptOptions`, `CallToolResult`, `Skill`, `ElicitRequestParams`, `ElicitResult`)
- `include/mcp/Meta.hpp` (`RequestMeta`, `CacheHint`, `LoggingLevel`, `ProgressToken`)
- `include/mcp/Capabilities.hpp`, `include/mcp/SpanHooks.hpp`, `include/mcp/McpVersion.hpp`, `include/mcp/ProtocolVersion.hpp`
- `src/server/McpServer.cpp`
