# Server Facade (`McpServer`) Reference

This page is the **single authoritative source for signatures and field tables** of `McpServer`, `RequestContext<TParams>` and `ServerOptions`; step-by-step usage lives in later how-to pages, which only link here and never duplicate the tables. Protocol semantics: `/en/learn/protocol-versions`; data types: `/en/reference/types`; error codes: `/en/reference/errors`; transport construction: `/en/reference/transports`.

## Factory and lifecycle

| Symbol | Signature/Type | Default/Returns | Notes |
|---|---|---|---|
| `McpServer::Create` | `static std::unique_ptr<McpServer> Create(std::shared_ptr<ITransport> transport, const ServerOptions& options = {})` | `std::unique_ptr<McpServer>` | Construction already wires inbound handlers, derives capabilities, stamps the negotiated version, calls `transport->Start()` and spawns the message loop; requests can be served without `Run()` |
| `Run` | `void Run()` | — | Blocks on a condition variable until `Close()` clears `running_`; message processing happens on the session thread, `Run()` is only a main-thread park point |
| `Close` | `void Close()` | — | First waits for every in-flight async call (including URL-elicitation watchdogs), then closes the session handler, finally wakes `Run()` |
| `IsRunning` | `bool IsRunning() const` | `false` until the first `initialize`/`server/discover` | Means "handshake done and handler alive"; registration APIs use it to decide whether to emit `*/list_changed` |
| `~McpServer` | `virtual ~McpServer() = default` | — | Virtual destructor, safe to destroy through `unique_ptr`; destruction is not equivalent to `Close()` |

Declarations live in `include/mcp/server/McpServer.hpp`; construction and lifecycle are implemented in `src/server/McpServer.cpp`.

::: warning
The caller owns the transport: `Create()` only stores a `shared_ptr` and `Close()` only closes the session handler, never the transport. The server exposes no `Connect()`; only the client side needs a factory connection.
:::

## Registration API

| Symbol | Signature/Type | Default/Returns | Notes |
|---|---|---|---|
| `RegisterTool` (object path) | `void RegisterTool(std::shared_ptr<McpServerTool> tool)` | `void` | Inserts into `tools_` (name-ordered map) and invalidates the tool-list cache; capabilities are re-derived afterwards |
| `RegisterTool` (lambda overload) | `void RegisterTool(std::string_view name, const ToolOptions& options, std::function<CallToolResult(const RequestContext<CallToolRequestParams>&)> fn)` | `void` | Forwards inline to `McpServerTool::Create(name, std::move(fn), options)`; behaviour is identical to the object path |
| `McpServerTool::Create` | `template <typename Callable> static std::shared_ptr<McpServerTool> Create(std::string_view name, Callable&& callable, const ToolOptions& options = {})` | `shared_ptr<McpServerTool>` | Builds `McpServerToolImpl<std::decay_t<Callable>>`; when `options.input_schema` is absent it fills `{"type":"object","properties":{}}` |
| `McpServerTool` | `virtual const Tool& ProtocolTool() const` / `virtual void InvokeAsync(const RequestContext<CallToolRequestParams>&, std::promise<CallToolResult>)` | pure virtual | Extension point for custom execution; `McpServerToolImpl::InvokeAsync` runs the callable synchronously on the calling thread and funnels exceptions into the promise |
| `RegisterResource` | `void RegisterResource(std::string_view name, std::string_view uri, const ResourceOptions&, std::function<ReadResourceResult(const std::string& uri)> handler)` | `void` | Static resource; the handler receives the concrete uri |
| `RegisterResourceTemplate` | `void RegisterResourceTemplate(std::string_view name, std::string_view uri_template, const ResourceOptions&, std::function<ReadResourceResult(const std::string& uri, const std::map<std::string, std::string>& vars)> handler)` | `void` | The URI template is parsed and validated at registration time; the handler also gets the expanded variable map |
| `RegisterPrompt` | `void RegisterPrompt(std::string_view name, const PromptOptions&, std::function<GetPromptResult(const std::string& name, const std::optional<JsonValue>& args)> handler)` | `void` | `PromptOptions::arguments` is passed through to `prompts/list` verbatim |
| `RegisterSkill` | `void RegisterSkill(Skill skill)` | `void` | Deduplicated by `Skill::uri`; advertising the capability additionally requires `enable_skills_extension` plus an existing `resources` capability; emits no `list_changed` |
| `SetSkillsDirectoryRead` | `void SetSkillsDirectoryRead(bool enabled)` | `void` (default `false`) | Sets the skill-directory browsing flag, recorded as `directory_read` in the skills extension settings; changes the `skills/directory` result set |
| `SetCompletionHandler` | `using CompletionHandler = std::function<CompleteResult(const CompleteRequestParams&)>; void SetCompletionHandler(CompletionHandler handler)` | `void` | Without a handler `completion/complete` returns `MethodNotFound` (`"No completion handler registered"`); the capability is advertised solely by `declare_completions` |

The registration methods are declared in `include/mcp/server/McpServer.hpp` and the tool object contract in `include/mcp/server/McpServerTool.hpp`; insertion and cache invalidation are in `src/server/McpServer.cpp`.

## Registration validation and errors

| Trigger | Throws | Message / basis |
|---|---|---|
| Tool name empty, longer than 128, or containing anything outside `[A-Za-z0-9._-]` | `McpError(InvalidParams)` | `invalid tool name: <name>` |
| Invalid `x-mcp-header` parameter annotations in `ToolOptions::input_schema` | `McpError(InvalidParams)` | `invalid_reason` produced by the annotation parser |
| Duplicate tool name | `McpError(InvalidParams)` | `tool already registered: <name>` |
| Empty `shared_ptr<McpServerTool>` passed in | No throw — null dereference | The implementation performs no null check (`tool->ProtocolTool()`) |
| Empty resource / template / prompt handler | `McpError(InvalidParams)` | `resource handler must not be empty: <name>`, `resource template handler must not be empty: <name>`, `prompt handler must not be empty: <name>` |
| Malformed URI template | `McpError(InvalidParams)` | `invalid resource template '<uri>': <error>` |
| Resource name or uri_pattern (templates included) collides with an existing entry | `McpError(InvalidParams)` | `resource already registered: <name>` / `resource uri already registered: <uri>`; static resources and templates share one conflict table |
| Duplicate prompt name | `McpError(InvalidParams)` | `prompt already registered: <name>` |
| Invalid skill uri | `McpError(InvalidParams)` | `invalid skill uri: <uri>`; the uri must end with `/SKILL.md` and have a `scheme://host/…` shape |
| Duplicate skill uri | `McpError(InvalidParams)` | `skill already registered: <uri>` |
| Empty completion handler | `McpError(InvalidParams)` | `completion handler must not be empty` |

Every check above runs at the entry of its registration function in `src/server/McpServer.cpp`; the tool-name and skill-uri shape rules are the file-local `IsValidToolName` / `IsSkillEntryUri`.

## Reverse requests: `Elicit` / `ElicitUrl`

| Symbol | Signature/Type | Default/Returns | Notes |
|---|---|---|---|
| `Elicit` | `std::future<ElicitResult> Elicit(const ElicitRequestParams& params, std::string_view session_id = {})` | `future<ElicitResult>` | Form-mode reverse request; timeout comes from `options.input_required_config->round_timeout`, otherwise 600s |
| `ElicitUrl` | `std::future<ElicitResult> ElicitUrl(const std::string& url, const std::string& message, std::chrono::seconds timeout = std::chrono::seconds(600), std::string_view session_id = {})` | `future<ElicitResult>` | URL mode (SEP-1034); in the legacy era it mints an auto-incremented `elicitationId` (`elicit-<n>`) and waits for `notifications/elicitation/complete`, with the watchdog resolving as `RequestTimeout` |

The trailing session argument (both APIs) is resolved by `EffectiveSessionId()` in this order: explicit non-empty `session_id` > global capabilities already set (returns the empty string, i.e. use global) > exactly one non-empty entry in `client_sessions_`, which becomes the fallback > otherwise the empty string (session capabilities unreadable). Under stateful HTTP capabilities land only in `client_sessions_`, so a no-argument call falls into the "global empty" branch and wrongly throws `-32021`; therefore tool handlers must pass `RequestContext::SessionId()`. The resolution logic is `EffectiveSessionId()` in `src/server/McpServer.cpp`.

Protocol-era differences (important):

- **Modern era** (negotiated version `>= 2026-07-28`): neither call **sends** `elicitation/create`; both throw the implementation-internal `ElicitationInputRequired` carrying `inputRequests["elicitation"]`. The three `HandleCallTool` paths catch it and turn it into `CallToolResult::input_required`, fulfilled by the client's `SetElicitationHandler` / `SetUrlElicitationHandler`. The `timeout` argument of `ElicitUrl` is ignored on this branch.
- **Legacy era**: client capabilities are checked first; a missing declaration throws `McpError(MissingRequiredClientCapability)` with `data.requiredCapabilities.elicitation` containing an empty `form`/`url` object. For form mode "declared" means `elicitation.form` present **or** `url` not declared; URL mode requires `elicitation.url`.
- If the response carries a known JSON-RPC error code, the `Elicit` future resolves by throwing `McpError(code, message)`.

::: warning
`ElicitationInputRequired` exists only inside the anonymous namespace of `src/server/McpServer.cpp` and appears in no exported header: user code must not catch it by name and may only observe the `input_required` field of the tool result.
:::

## Notification and push API

| Symbol | Signature/Type | Default/Returns | Notes |
|---|---|---|---|
| `SendToolListChanged` | `void SendToolListChanged()` | — | Delegates to `SendListChangedNotification(tools/list_changed)` |
| `SendResourceListChanged` | `void SendResourceListChanged()` | — | Same, `resources/list_changed` |
| `SendPromptListChanged` | `void SendPromptListChanged()` | — | Same, `prompts/list_changed` |
| `SendResourceUpdated` | `void SendResourceUpdated(const std::string& uri)` | — | Delivered only to subscribers of that uri via `NotifySubscribers`; modern-era only |
| (routing rule) | internal `SendListChangedNotification` | — | Modern era → `NotifySubscribers`; legacy era → broadcast `SendNotification` |
| `SendLoggingMessage` | `void SendLoggingMessage(LoggingLevel level, std::string_view data)` | — | **Returns immediately without sending in the modern era**; in the legacy era it is filtered by the level recorded through `logging/setLevel` |
| `SendLoggingMessage` (overload) | `void SendLoggingMessage(LoggingLevel level, std::string_view data, std::optional<LoggingLevel> min_level)` | `min_level = nullopt` | With `min_level` set, that bound alone filters and sends (overriding the `setLevel` record); with `nullopt` it falls back to the two-argument behaviour |
| `SendTaskStatus` | `void SendTaskStatus(std::string_view task_id, TaskStatus status)` | — | Looks the owner session up in `task_store`, then routes through subscriptions or broadcast according to that session's negotiated version; needs `enable_tasks_extension` + `task_store` for any usable task |
| `SendProgress` | `void SendProgress(const ProgressToken& token, double progress, std::optional<double> total = std::nullopt, std::optional<std::string> message = std::nullopt)` | — | Modern era first tries the request-stream buffer (`BufferRequestStreamNotification`); otherwise sends the global `notifications/progress` |
| `SetCacheable` | `void SetCacheable(std::function<std::optional<CacheHint>(std::string_view method)> resolver)` | — | Replaces the cache hook at runtime, taking precedence over `ServerOptions::cache_hints`; hints are still validated in the modern era |

Declared in `include/mcp/server/McpServer.hpp`; delivery and era routing are in `src/server/McpServer.cpp`.

`TaskStatus` values: `Pending`, `Working`, `Completed`, `Failed`, `Cancelled`, `InputRequired` (defined in `include/mcp/server/McpTaskStore.hpp`).

## Client state queries

| Symbol | Signature/Type | Default/Returns | Notes |
|---|---|---|---|
| `GetClientCapabilities` | `std::shared_ptr<const ClientCapabilities> GetClientCapabilities() const` | `nullptr` before handshake | Global slot; written both by `initialize` (empty session id) and by modern `server/discover` (`_meta.clientCapabilities`) |
| `GetClientCapabilities` (per session) | `GetClientCapabilities(std::string_view session_id) const` | `nullptr` for unknown sessions | Looks `client_sessions_` up exactly and applies **no** `EffectiveSessionId` fallback |
| `GetClientInfo` / `GetClientInfo(session_id)` | `std::shared_ptr<const Implementation>` | `nullptr` before handshake | The global slot is written only by `initialize`; with a modern direct connection (discover) that carries no `clientInfo` it stays `nullptr`, so read `_meta` from the `RequestContext` instead |
| `GetCapabilities` | `ServerCapabilities GetCapabilities() const` | **returned by value** | Copy taken under the lock; callers must not hold a reference; capabilities have no manual assignment entry point |
| `GetNegotiatedProtocolVersion` | `std::string GetNegotiatedProtocolVersion() const` / `(std::string_view session_id) const` | **empty string** when nothing was negotiated and `protocol_version` is unset | The per-session overload falls back to the global version for unknown sessions; `IsModernProtocolVersion("")` is false, so an empty string is treated as legacy. A legacy `initialize` without a version declaration yields `kDefaultNegotiatedProtocolVersion` (`2025-03-26`); a declared version that is absent from the supported table or belongs to the modern era yields `kLegacyProtocolVersion` (`2025-11-25`) |
| `IsMrtrSupported` | `bool IsMrtrSupported() const` | `false` | Non-stateless transport and the session resolved by `EffectiveSessionId({})` declares `elicitation` |
| `GetServerCard` | `ext::ServerCard GetServerCard() const` | value | Throws `McpError(HandlerError, "server card is disabled")` when `enable_server_card=false`; without `server_card` a card is synthesized from `server_info`/`kSdkVersion`/the full version table with a placeholder endpoint URL |
| `SessionId` | `const std::string& SessionId() const` | random 8-byte hex generated at construction | **Instance-level** identifier used only to anchor task ownership: task ids look like `task-<session>-<16-byte hex>` |
| `RequestContext::SessionId` | `std::string_view SessionId() const` | empty string for stdio / stateless HTTP | **Request-level** MCP session id (stateful HTTP: the `mcp-session-id` header value) |
| `GetSessionHandler` | `McpSessionHandler& GetSessionHandler()` | reference | Escape hatch exposing the session handler; ordinary code does not need it |

The queries are declared in `include/mcp/server/McpServer.hpp` and `RequestContext::SessionId` in `include/mcp/server/RequestContext.hpp`; the version fallback rules span `src/server/McpServer.cpp` and `src/protocol/McpSessionHandler.cpp`.

::: danger
`McpServer::SessionId()` and `RequestContext::SessionId()` are unrelated and must never be interchanged: the first is a per-process random instance identifier (task owner), the second is the protocol session of the current request. Passing `McpServer::SessionId()` to `Elicit`/`ElicitUrl` resolves the wrong session capabilities.
:::

## `RequestContext<TParams>` members

| Symbol | Signature/Type | Default/Returns | Notes |
|---|---|---|---|
| `Params` | `const TParams& Params() const` | reference | Deserialized request parameters (`CallToolRequestParams` on the tool path) |
| `Server` | `McpServer& Server() const` | reference | Entry point to the reverse-call facade (`Elicit`, `SendProgress`, …) |
| `GetRequest` | `const mcp::JsonRpcRequest& GetRequest() const` | reference | Raw JSON-RPC request including `id`/`method`/`params`/`meta` |
| `LogLevel` | `std::optional<LoggingLevel> LogLevel() const` | `nullopt` without `_meta.logLevel` | Read from `_meta["io.modelcontextprotocol/logLevel"]`; integer and string forms both parse, parse failures are silently ignored |
| `SessionId` | `std::string_view SessionId() const` | empty string | Request-level session id; the object holds a `std::string` copy, so asynchronous handlers may read it safely |
| `Log` | `void Log(LoggingLevel level, std::string_view data) const` | — | Two gates: if a per-request `logLevel` is required but absent, the message is dropped; anything below `LogLevel()` is dropped |
| `IsCancellationRequested` | `bool IsCancellationRequested() const` | `false` without a flag | Cooperative cancellation; `notifications/cancelled` and `tasks/cancel` set the same flag, so plain tool handlers can poll it too |

`using LogFn = std::function<void(LoggingLevel, std::string_view)>`. All members and types are declared in `include/mcp/server/RequestContext.hpp`; the constructor is public but exists for SDK use — `HandleCallTool` and friends inject `log_fn`, the cancellation flag, `require_request_log_level` (true in the modern era) and the session id. User code must never build a `RequestContext`; obtain one from the handler signature.

## `ServerOptions` field by field

| Field | Type | Default | Notes / prerequisites |
|---|---|---|---|
| `server_info` | `std::optional<Implementation>` | `nullopt` → `{"mcp-server", kSdkVersion}` | Server identity in the `initialize`/`discover` response body |
| `protocol_version` | `std::optional<std::string>` | `nullopt` | Pins the wire codec version and overrides the negotiation outcome (written per session); does not change the capability set |
| `server_instructions` | `std::optional<std::string>` | `nullopt` | Sent as `instructions` with `initialize`/`discover` |
| `initialization_timeout` | `std::chrono::seconds` | `60` | **The server implementation currently has no read site at all** (only the client's field of the same name is effective); do not rely on it to bound the handshake |
| `cache_hints` | `std::optional<std::map<std::string, CacheHint, std::less<>>>` | `nullopt` | Static per-method map of `ttlMs`/`cacheScope`; in the modern era a missing or invalid value throws `InvalidParams` |
| `cache_hint_resolver` | `std::function<std::optional<CacheHint>(std::string_view method)>` | empty | Takes precedence over `cache_hints`; replaceable at runtime via `SetCacheable` |
| `request_state_verifier` | `std::function<bool(std::string_view)>` | empty | MRTR `requestState` verifier; when set, the built-in `request_state_key` verifier is not installed |
| `request_state_key` | `std::optional<std::string>` | `nullopt` | HMAC key: setting it installs the built-in verifier and signs `iat` into the state when returning `input_required` |
| `request_state_ttl` | `std::chrono::seconds` | `0` (no expiry check) | Applies only to the built-in verifier, comparing the payload `iat` |
| `input_required_config` | `std::optional<InputRequiredConfig>` | `nullopt` | MRTR/elicitation parameter bundle |
| `input_required_config->max_rounds` | `int` | `10` | **No read site on the server** (the round cap only applies to the client MRTR loop) |
| `input_required_config->round_timeout` | `std::chrono::seconds` | `600` | Used as the `Elicit` wait timeout; overrides the 600s default when configured |
| `declare_logging` | `bool` | `false` | Advertises the `logging` capability even with no handler yet |
| `declare_completions` | `bool` | `false` | Advertises the `completions` capability regardless of whether a handler is registered |
| `on_method_called` | `std::function<void(std::string_view method)>` | empty | Chained together with `on_request`; both fire |
| `on_client_connected` | `std::function<void(const Implementation& client_info)>` | empty | Fires once `initialize` exposes `clientInfo` |
| `on_initialized` | `std::function<void()>` | empty | Fires on the first `notifications/initialized` for that session (duplicates do not re-fire); exceptions inside the callback are swallowed and logged |
| `on_protocol_error` | `std::function<void(std::string_view error)>` | empty | Chained with `on_error`, receives only the error message |
| `on_request` | `std::function<void(std::string_view method, const JsonRpcRequest&)>` | empty | Every inbound request |
| `on_response` | `std::function<void(const JsonRpcResponse&)>` | empty | Every outbound success response |
| `on_error` | `std::function<void(const JsonRpcErrorResponse&)>` | empty | Every outbound error response |
| `on_notification` | `std::function<void(const JsonRpcNotification&)>` | empty | Every inbound notification |
| `span_handler` | `SpanHandler` (`std::function<void(const SpanEvent&)>`) | empty | Observability hook, moved into the session handler; `SpanHandler` is defined in `include/mcp/SpanHooks.hpp` |
| `incoming_filters` / `outgoing_filters` | `std::shared_ptr<FilterPipeline>` | `nullptr` | Message pipelines injected once, at session-handler construction |
| `on_transport_close` | `std::function<void()>` | empty | Attached only when the transport is a `TransportBase` subclass (`dynamic_cast` failure is silently ignored) |
| `on_transport_error` | `std::function<void(std::string_view)>` | empty | Same as above |
| `max_tool_input_elements` | `std::optional<std::size_t>` | `nullopt` (unlimited) | Objects counted by keys, arrays by length, scalars as 1; exceeding it yields an `is_error` tool result rather than a protocol error |
| `supported_protocol_versions` | `std::optional<std::vector<std::string>>` | `nullopt`/empty → full `kProtocolVersions` | Narrows the `discover` answer, the `initialize` candidate set and the error `data.supported` simultaneously |
| `subscriptions_enabled` | `bool` | `true` | When `false`, `subscriptions/listen` returns `MethodNotFound` (`"subscriptions are disabled"`); non-modern versions already return `MethodNotFound` |
| `task_store` | `std::shared_ptr<IMcpTaskStore>` | `nullptr` | Persistence backend for task execution; without it no task handle is ever produced |
| `enable_tasks_extension` | `bool` | `false` | Declares the extension and admits `tasks/*`; with a `task_store` but this flag off, `tasks/*` is rejected |
| `enable_skills_extension` | `bool` | `false` | Admits `skills/list`, `skills/get`, `skills/directory` (otherwise `MethodNotFound`) |
| `enable_apps_extension` | `bool` | `false` | Declares the apps extension and admits apps-related requests |
| `enable_server_card` | `bool` | `false` | Declares the server-card extension; required for `GetServerCard()` to work |
| `server_card` | `std::optional<ext::ServerCard>` | `nullopt` | Used verbatim (after validation) when provided; otherwise a default card is synthesized |
| `enable_etag_cache` | `bool` | `false` | **Process-wide side effect**: the constructor calls `SetEtagCacheEnabled`, changing etag behaviour of every client/server in the same process |
| `enable_progressive_discovery` | `bool` | `false` | When off, `tools/list` serves a whole-table cache and ignores `query`/`pageSize`; when on, filtering and paging apply |
| `enable_file_input` | `bool` | `false` | Expands `FileRef` entries (`file://`, `resource://`) inside `arguments`; 8MB per slice, 256KB inline (`kFileInputMaxBytes`/`kFileInputInlineMaxBytes`) |
| `enable_triggers_events` | `bool` | `false` | `EmitTriggerEvent` returns silently while this is off (register/unregister are not gated) |
| `TasksImplicitlyEnabled()` | `[[deprecated]] bool` (method) | computed | Deprecated implicit derivation (`enable_tasks_extension \|\| task_store`); set `enable_tasks_extension` explicitly instead |
| `SkillsImplicitlyEnabled()` | `[[deprecated]] bool` (method) | `enable_skills_extension` | Deprecated; set `enable_skills_extension` explicitly instead |

Every field is declared in `include/mcp/server/ServerOptions.hpp`; the construction-time effects and the individual read sites are in `src/server/McpServer.cpp`.

## Capability derivation

Capabilities have no assignment entry point and are derived purely from registrations and flags (re-run after every registration; the derivation function and its call sites are in `src/server/McpServer.cpp`):

| Capability | Trigger |
|---|---|
| `tools.list_changed` | `tools_` non-empty |
| `resources.subscribe` + `list_changed` | `resources_` non-empty (static and templates share the table) |
| `prompts.list_changed` | `prompts_` non-empty |
| `logging` | `declare_logging` only |
| `completions` | `declare_completions` only |
| `extensions["io.modelcontextprotocol/tasks"]` | `enable_tasks_extension` |
| `extensions[apps]` / `extensions[server-card]` | `enable_apps_extension` / `enable_server_card` (ids: `ext::kAppsExtensionId`, `ext::kServerCardExtensionId`) |
| `extensions[skills]` | `enable_skills_extension` **and** at least one registered skill **and** an existing `resources` capability; `directory_read` comes from `SetSkillsDirectoryRead` |

## Error semantics of tool calls

Based on the synchronous path in `src/server/McpServer.cpp` (no `task_store`, or the tool is not task-declared):

| Situation | Observable behaviour |
|---|---|
| Handler throws `McpError` | Result has `is_error=true` with a single `text` content equal to `ErrorMessage()`; **the error code and `data` are discarded** |
| Handler throws another `std::exception` or an unknown type | Outgoing text is generalized to `Error executing tool <name>`; the original `what()` goes only to internal logging (logging is off by default, so it is invisible externally) |
| Handler throws `ElicitationInputRequired` (raised by `Elicit`/`ElicitUrl` in the modern era) | Converted into a `resultType=input_required` result whose `inputRequests["elicitation"]` carries the original arguments |
| `arguments` violate `input_schema` | The SDK validates against the JSON Schema itself and returns `is_error` + `"invalid arguments: <reason>"`; the handler never runs |
| `structured_content` violates `output_schema` | A **protocol error**, not `is_error`: `InvalidParams` + `"tool output does not match outputSchema: <reason>"`; no tool result is produced for that call |
| Argument element count exceeds `max_tool_input_elements` | `is_error` + `"tool arguments exceed maximum input element count"` |
| Unknown tool name | Protocol error `InvalidParams`, `tool not found: <name>` (not an `is_error` result) |
| Legacy era, handshake incomplete | Protocol error `InvalidRequest`, `Server not initialized`; the modern era skips this check |
| Handler exception on the task path | No `is_error`: `McpError` goes through `FailTask` keeping code/message/data, other exceptions become `InternalError` + `Error executing tool <name>` |

## Internal symbols (not user API — never cite them as interface)

- `ElicitationInputRequired`, `ToolExecutionFailureMessage`, `IsValidToolName`, `IsSkillEntryUri`, `kElicitTimeout` (600s) and `kFileInputMaxBytes` (8MB) live in the anonymous namespace of `src/server/McpServer.cpp` and are exported by no header.
- `detail::MintRequestState` / `detail::VerifyRequestState` / `detail::DecodeRequestStatePayload` (`include/mcp/server/RequestState.hpp`) are internal `mcp::detail` HMAC/base64url helpers; users only configure `request_state_key`/`request_state_verifier`.
- The `Wire*`, `Handle*`, `EffectiveSessionId` and `ClientCapabilitiesFor` members of `McpServer` are private (see `include/mcp/server/McpServer.hpp`) and are not part of any public contract.
- `ResolveToolParamAnnotations` is a public method but returns `std::vector<detail::McpParamAnnotation>`; it is an advanced facility that ordinary registrations never need.

Task-shaped guides: [/en/howto/register-a-tool](/en/howto/register-a-tool), [/en/howto/collect-user-input](/en/howto/collect-user-input), [/en/howto/run-long-tasks](/en/howto/run-long-tasks).

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
