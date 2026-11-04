# Types Reference

This page is the **authoritative field table** for the core type layer; every entry is checked against the headers. Facade signatures live in the [Server reference](/en/reference/server) and [Client reference](/en/reference/client), the complete error-code table in the [Error codes reference](/en/reference/errors), and the conceptual narrative in [Protocol versions](/en/learn/protocol-versions). No procedural steps on this page.

## `JsonValue` (a type owned by this repository)

::: danger
`mcp::JsonValue` is **not nlohmann::json** and there is no alias or compatibility shim. `parse()`/`dump()`/`contains()`/`value()`/`is_string()`/`push_back()`/`json::object()` do not exist here and copying nlohmann style fails to compile; build an empty object/array with `JsonValue(JsonValue::object_tag)` / `JsonValue(JsonValue::array_tag)` and use the capitalised accessors `Parse`/`Dump`/`Contains`/`Find`/`At`/`PushBack`/`IsString`.
:::

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `JsonValue::Object` | `using Object = std::map<std::string, JsonValue, std::less<>>` | — | Transparent comparison, so `string_view` keys work |
| `JsonValue::Array` | `using Array = std::vector<JsonValue>` | — | Array container |
| Internal storage | `std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object>` | `nullptr` | Seven alternatives; no uint/float branch |
| Scalar constructors | `JsonValue(bool/int/int64_t/double/std::string&&/const std::string&/const char*/const char* + size_t)` | — | `int` is always widened to `int64_t` |
| Container constructors | `JsonValue(Array v)` / `JsonValue(Object v)` / `JsonValue(ObjectTag)` / `JsonValue(ArrayTag)` | — | The two tag enums are `object_tag` / `array_tag` |
| `Parse` | `static JsonValue Parse(std::string_view json)` | — | Throws on malformed input |
| `SanitizeUtf8` | `static std::string SanitizeUtf8(std::string_view text)` | — | Strips invalid UTF-8 sequences |
| `Dump` | `std::string Dump(int indent = -1) const` | `-1` (compact) | Serialises back to a string |
| Type checks | `IsNull`/`IsBool`/`IsInt`/`IsDouble`/`IsNumber`/`IsString`/`IsArray`/`IsObject` | — | All return `bool`; `IsNumber` = `IsInt \|\| IsDouble` |
| Getters | `GetBool`/`GetInt`/`GetDouble`/`GetString`/`GetArray`/`GetObject` | — | Throw on a type mismatch; `GetString` returns `const std::string&`, `GetArray`/`GetObject` also have mutable overloads |
| Container info | `size_t Size() const` / `bool Empty() const` / `bool Contains(std::string_view) const` | — | Work on both arrays and objects |
| Indexing | `JsonValue& operator[](size_t)` / `JsonValue& operator[](std::string_view)` | — | `const` and non-`const` overloads |
| `Find` | `const JsonValue* Find(std::string_view key) const` / non-const overload | `nullptr` when absent | Preferred for optional fields; never throws |
| `At` | `const JsonValue& At(std::string_view key) const` / non-const overload | — | Throws when the key is missing |
| `PushBack` | `void PushBack(JsonValue val)` | — | Appends by value |
| Equality | `bool operator==(const JsonValue&) const` / `operator!=` | — | Structural deep comparison |
| Range-for | `begin()` / `end()` | — | **Only objects are iterable** (they forward to `GetObject()`); arrays need an explicit `GetArray()` |
| `GetObject` macro guard | `#pragma push_macro("GetObject")` / `pop_macro` | — | Defends against the same-named Windows header macro; do not remove |

All of the above is declared in `include/mcp/JsonValue.hpp`.

## JSON-RPC envelope

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `RequestId` | `using RequestId = std::variant<int64_t, std::string>` | — | Numeric or string id |
| `ErrorData` | `struct { McpErrorCode code; std::string message; std::optional<JsonValue> data; }` | `code = InternalError` | Error payload carrier |
| `JsonRpcRequest` | `struct` with `jsonrpc`/`id`/`method`/`params`/`meta`/`session_id` | `jsonrpc = "2.0"` | `params` and `meta` are `optional<JsonValue>`; `session_id` is filled by the transport |
| `JsonRpcNotification` | `struct`, same minus `id` | `jsonrpc = "2.0"` | No id |
| `JsonRpcResponse` | `struct { std::string jsonrpc; RequestId id; JsonValue result; std::string session_id; }` | `"2.0"` | `result` held by value |
| `JsonRpcErrorResponse` | `struct { ...; std::optional<RequestId> id; ErrorData error; ... }` | `"2.0"` | id may be absent (parse failures) |
| `JsonRpcMessage` | `std::variant<JsonRpcRequest, JsonRpcNotification, JsonRpcResponse, JsonRpcErrorResponse>` | — | The only message type on the wire in the transport layer |
| `SerializeMessage` | `std::string SerializeMessage(const JsonRpcMessage&)` / `(JsonRpcMessage&&)` | — | The move overload avoids a deep copy |
| `DeserializeMessage` | `JsonRpcMessage DeserializeMessage(std::string_view json)` | — | Throws `McpError` on failure |
| `Is*` / `As*` | `IsRequest`/`IsNotification`/`IsResponse`/`IsError`, `AsRequest`/`AsNotification`/`AsResponse`/`AsError` | — | `As*` return `std::get_if` pointers and never throw |
| `RequestIdToJson` / `RequestIdFromJson` | `JsonValue ...` / `RequestId ...` | — | Convert ids to and from wire form |

All of the above is declared in `include/mcp/JsonRpc.hpp`.

## Content variants

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `Icon` | `struct { std::string src; optional<string> mime_type; optional<vector<string>> sizes; optional<string> theme; }` | `src` required | Icon entry |
| `Annotations` | `struct { optional<vector<string>> audience; optional<double> priority; optional<string> last_modified; }` | all `nullopt` | Content annotations |
| `TextResourceContents` | `struct { uri; text; optional<mime_type>; optional<JsonValue> meta; }` | — | Textual resource body |
| `BlobResourceContents` | `struct { uri; blob; optional<mime_type>; optional<JsonValue> meta; }` | — | Base64 resource body |
| `ResourceContents` | `std::variant<TextResourceContents, BlobResourceContents>` | — | One of the two resource bodies |
| `TextContent` | `struct { std::string type = "text"; std::string text; optional<string> mime_type; optional<Annotations> annotations; optional<JsonValue> meta; }` | `type = "text"` | Note `type` has an in-struct default, yet aggregate initialisation still passes it explicitly |
| `ImageContent` / `AudioContent` | `struct { type("image"/"audio"); data; mime_type; optional<annotations>; optional<meta>; }` | `type` built in | `mime_type` is **not** optional |
| `EmbeddedResource` | `struct { type("resource"); ResourceContents resource; optional<annotations>; optional<meta>; }` | — | Embedded resource |
| `ResourceLink` | `struct { type("resource_link"); uri; name; optional<title/description/mime_type/size>; vector<Icon> icons; optional<annotations>; optional<meta>; }` | — | Resource link |
| `ContentVariant` | `std::variant<TextContent, ImageContent, AudioContent, EmbeddedResource, ResourceLink>` | — | Element type of `CallToolResult::content` |

All of the above is declared in `include/mcp/Content.hpp`.

## `_meta`, progress tokens and cache hints

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `ProgressToken` | `using ProgressToken = std::variant<std::string, int64_t>` | — | `_meta.progressToken` |
| `LoggingLevel` | `enum class LoggingLevel { Debug, Info, Notice, Warning, Error, Critical, Alert, Emergency }` | — | **Protocol-level** log severity, unrelated to the SDK's internal `LogLevel` |
| `RequestMeta.progress_token` | `std::optional<ProgressToken>` | `nullopt` | Progress correlation token |
| `RequestMeta.protocol_version` | `std::string` | `kLatestProtocolVersion` (`"2026-07-28"`) | Version stamp on outgoing `_meta` |
| `RequestMeta.client_info` / `client_capabilities` | `optional<Implementation>` / `optional<ClientCapabilities>` | `nullopt` | Populated only during `initialize` |
| `RequestMeta.log_level` | `std::optional<LoggingLevel>` | `nullopt` | Log level requested by the client |
| `RequestMeta.extensions` | `std::optional<JsonValue>` | `nullopt` | Extension slot inside `_meta` |
| `RequestMeta.traceparent` / `tracestate` / `baggage` | `std::optional<std::string>` ×3 | `nullopt` | W3C trace context, fed to the span hooks |
| `CacheHint.ttl_ms` / `cache_scope` | `std::optional<int64_t>` / `std::optional<std::string>` | `nullopt` | Cache hints on list results |

All of the above is declared in `include/mcp/Meta.hpp`.

## Capabilities

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `ToolsCapability` / `PromptsCapability` / `RootsCapability` | `struct { std::optional<bool> list_changed; }` | `nullopt` | Identical shape |
| `ResourcesCapability` | `struct { optional<bool> subscribe; optional<bool> list_changed; }` | `nullopt` | The only one carrying `subscribe` |
| `SamplingCapability` / `EmptyCapability` | `struct {}` | — | Empty markers; `LoggingCapability`, `TasksCapability`, `SubscriptionsCapability`, `CompletionsCapability` are aliases of `EmptyCapability` |
| `ElicitationCapability` | `struct { std::optional<JsonValue> form; std::optional<JsonValue> url; }` | `nullopt` | Declares supported elicitation shapes |
| `kTasksExtensionId` / `kSkillsExtensionId` | `inline constexpr std::string_view` | `"io.modelcontextprotocol/tasks"` / `".../skills"` | Extension declaration keys |
| `TasksExtensionSettings` / `SkillsExtensionSettings` | `struct {}` / `struct { bool directory_read{false}; }` | `false` | Extension settings bodies |
| `ServerCapabilities` | `struct`: `tools`/`resources`/`prompts`/`logging`/`sampling`/`roots`/`elicitation`/`completions`/`subscriptions` all `optional<>`, plus `optional<map<string, JsonValue>> extensions` and `optional<JsonValue> experimental` | all `nullopt` | 11 fields in total |
| `ClientCapabilities` | `struct`: `roots`/`sampling`/`elicitation`/`extensions`/`experimental` | all `nullopt` | 5 fields in total |

All of the above is declared in `include/mcp/Capabilities.hpp`.

## Implementation information

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `Implementation` | `struct { std::string name; std::string version; optional<string> title; vector<Icon> icons; optional<string> description; optional<string> website_url; }` | `name`/`version` required, `icons = {}` | Used for both `client_info` and `server_info` |

Declared in `include/mcp/Implementation.hpp`.

## Declaration types: `Tool` and `ToolOptions`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `Tool::name` | `std::string` | empty (required) | Registration name |
| `Tool::title` / `description` | `std::optional<std::string>` | `nullopt` | Human-readable metadata |
| `Tool::input_schema` | `JsonValue` (**not** optional) | default-constructed = `null` | Mandatory on the wire; the default schema is supplied during registration from `ToolOptions` |
| `Tool::output_schema` | `std::optional<JsonValue>` | `nullopt` | Structured-output schema |
| `Tool::annotations` | `std::optional<ToolAnnotations>` | `nullopt` | Behavioural annotations |
| `Tool::icons` | `std::vector<Icon>` | `{}` | Icon set |
| `Tool::meta` | `std::optional<JsonValue>` | `nullopt` | `_meta` pass-through |
| `Tool::execution` | `std::optional<ToolExecution>` | `nullopt` | Task / human-in-the-loop execution declaration |
| `ToolOptions` | `struct`, 13 fields + 3 fluent setters | see below | Registration input; it does not map one-to-one onto `Tool` |
| `ToolOptions::name` | `std::optional<std::string>` | `nullopt` | Never consumed: the effective name comes from the `name` argument of `RegisterTool`/`Create` |
| `ToolOptions::{description,title,destructive,idempotent,read_only_hint,open_world_hint}` | `std::optional<std::string>` ×2 + `std::optional<bool>` ×4 | `nullopt` | The four bools are assembled into `ToolAnnotations` |
| `ToolOptions::use_structured_content` | `bool` | `false` | When true, `output_schema` is set to `{"type":"object"}` |
| `ToolOptions::{input_schema,output_schema,icons,meta,execution}` | `optional<JsonValue>` ×3 (incl. `meta`), `vector<Icon>`, `optional<ToolExecution>` | `nullopt` / `{}` | **Assign these members directly** |
| `ToolOptions::Description` / `Title` / `InputSchema` | `ToolOptions& Description(std::string_view)`, `ToolOptions& Title(std::string_view)`, `ToolOptions& InputSchema(JsonValue)` | — | These are the **only** fluent setters; every other field must be assigned |

::: warning
`ToolOptions` has no `Name()`, `OutputSchema()`, `Icons()`, `Meta()`, `Execution()` or `Destructive()` setter — `ToolOptions{}.OutputSchema(x)` does not compile.
:::

All of the above is declared in `include/mcp/McpTypes.hpp`.

## Remaining declaration types

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `Resource` | `struct { uri; name; optional<title/description/mime_type/size>; optional<vector<Icon>> icons; optional<ResourceAnnotations> annotations; optional<JsonValue> meta; }` | `uri`/`name` required | Static resource descriptor |
| `ResourceTemplate` | `struct`, first field `uri_template`, otherwise like `Resource` (minus `size`) | `uri_template`/`name` required | Templated resource descriptor |
| `ResourceAnnotations` | `struct { optional<vector<string>> audience; optional<double> priority; optional<string> last_modified; }` | all `nullopt` | Same shape as `Annotations` but a distinct type |
| `ToolAnnotations` | `struct { optional<string> title; optional<bool> read_only_hint/idempotent_hint/open_world_hint/destructive_hint; }` | all `nullopt` | Assembled from the four `ToolOptions` bools |
| `ToolExecutionMode` / `TaskSupport` | `enum class { Auto, Manual, Task }` / `enum class { Optional, Required }` | `Auto` / no default | Execution mode and task-support strength |
| `ToolExecution` | `struct { ToolExecutionMode mode{Auto}; optional<string> human_use; optional<TaskSupport> task_support; }` | `mode = Auto` | Tool execution declaration |
| `Prompt` | `struct { name; optional<title/description>; optional<vector<PromptArgument>> arguments; optional<vector<Icon>> icons; optional<JsonValue> meta; }` | `name` required | Prompt descriptor |
| `PromptArgument` | `struct { name; optional<description>; optional<bool> required; }` | `name` required | Argument declaration |
| `PromptMessage` | `struct { std::string role; ContentVariant content; }` | — | Entry returned by `prompts/get` |
| `ResourceOptions` | `struct { optional<name/description/title/mime_type>; vector<Icon> icons; }` + `Description`/`Title`/`MimeType` | `nullopt` / `{}` | Three setters, everything else assigned directly; `name` is never consumed — the effective name is the `name` argument of `RegisterResource` |
| `PromptOptions` | `struct { optional<name/description/title>; vector<Icon> icons; optional<vector<PromptArgument>> arguments; }` + `Description`/`Title`/`Arguments` | `nullopt` / `{}` | Three setters, everything else assigned directly; `name` is never consumed — the effective name is the `name` argument of `RegisterPrompt` |
| `SkillResource` | `struct { std::string uri; std::string digest; int64_t size{0}; }` | `size = 0` | Digest of a file inside a skill |
| `SkillResources` | `std::variant<std::vector<SkillResource>, std::string>` | — | Either an explicit list or a directory reference |
| `Skill` | `struct { std::string uri; JsonValue frontmatter; SkillResources resources; optional<JsonValue> meta; }` | `uri`/`frontmatter`/`resources` required | Skill declaration (`skills/*`) |

All of the above is declared in `include/mcp/McpTypes.hpp`; the two places where `name` is ignored are the registration functions in `src/server/McpServer.cpp`.

## Request parameter types

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `PaginatedRequestParams` | `struct { optional<string> cursor; optional<RequestMeta> meta; }` | `nullopt` | Aliased as `ListToolsRequestParams`, `ListResourcesRequestParams`, `ListResourceTemplatesRequestParams`, `ListPromptsRequestParams`, `ListSkillsRequestParams` |
| `ResourceRequestParams` | `struct { std::string uri; optional<RequestMeta> meta; optional<JsonValue> input_responses; optional<string> request_state; }` | `uri` required | Aliased as `ReadResourceRequestParams`, `SubscribeRequestParams`, `UnsubscribeRequestParams` |
| `CallToolRequestParams` | `struct { name; optional<JsonValue> arguments; optional<RequestMeta> meta; optional<JsonValue> input_responses; optional<string> request_state; }` | `name` required | MRTR continuation relies on the last two fields |
| `GetPromptRequestParams` | `struct { name; optional<JsonValue> arguments; optional<RequestMeta> meta; optional<JsonValue> input_responses; optional<string> request_state; }` | `name` required | Same shape as `CallTool` |
| `CompleteRequestParams` | `struct { JsonValue ref; std::string argument_name; std::string argument_value; map<string,string,less<>> context_arguments; optional<RequestMeta> meta; }` | `context_arguments = {}` | `ref` is a raw object (`{"type":"ref/prompt","name":...}`), not a strongly typed variant |
| `InitializeRequestParams` | `struct { std::string protocol_version; ClientCapabilities capabilities; Implementation client_info; }` | no optionals | All three fields mandatory |
| `DiscoverRequestParams` / `ListRootsRequestParams` | `struct {}` | — | Empty-parameter requests |
| `Pagination` | `struct { std::optional<std::string> next_cursor; }` | `nullopt` | Cursor envelope |
| `FileRef` | `struct { std::string uri; int64_t offset{0}; int64_t length{0}; }` | `0` | `x-mcp-fileref` argument; zero-valued fields are omitted during serialisation |
| `SubscriptionFilter` | `struct { optional<bool> tools_list_changed/prompts_list_changed/resources_list_changed; vector<string> resource_subscriptions; }` | `nullopt` / `{}` | `subscriptions/listen` filter |
| `SubscriptionsListenRequestParams` | `struct { SubscriptionFilter notifications; optional<RequestMeta> meta; }` | — | Listen-subscription input |
| `CreateMessageRequestParams` / `SamplingMessage` | `struct { vector<SamplingMessage> messages; int64_t max_tokens; optional<string> stop_reason/model_preference; }` / `struct { role; ContentVariant content; }` | `max_tokens` required | `sampling/*` is deprecated (SEP-2577); the types remain |
| `SetLevelRequestParams` | `struct { LoggingLevel level; }` | — | Input of `logging/setLevel` |
| `LoggingMessageNotificationParams` | `struct { LoggingLevel level; optional<string> logger; JsonValue data; }` | `logger = nullopt` | `notifications/message` |
| `ProgressNotificationParams` | `struct { ProgressToken progress_token; double progress; optional<double> total; optional<string> message; }` | — | Argument of the `on_progress` callback |
| `CancelledNotificationParams` | `struct { RequestId request_id; optional<string> reason; }` | — | Cancellation notification |

All of the above is declared in `include/mcp/McpTypes.hpp`.

## Result types

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `Result` | `struct { std::optional<JsonValue> meta; ResultType result_type{Complete}; }` | `Complete` | Base of every result; `ResultType = { Complete, InputRequired, Task }` |
| `EmptyResult` / `PingResult` | `struct EmptyResult : Result {}`; `using PingResult = EmptyResult` | — | Empty result |
| `CacheableResult` | `struct CacheableResult : Result { std::optional<std::string> etag; }` | `nullopt` | ETag governed by the global switch |
| `CallToolResult` | `struct : Result { vector<ContentVariant> content; optional<JsonValue> structured_content; bool is_error{false}; optional<InputRequiredResult> input_required; }` | `is_error = false` | Four own fields plus the two inherited ones |
| `ListToolsResult` / `ListResourcesResult` / `ListResourceTemplatesResult` / `ListPromptsResult` | `struct : Result { vector<…>; optional<string> next_cursor; optional<CacheHint> cache_hint; }` | `nullopt` | Four isomorphic paginated list results |
| `ReadResourceResult` | `struct : Result { vector<ResourceContents> contents; optional<CacheHint> cache_hint; optional<InputRequiredResult> input_required; }` | — | Resource read |
| `GetPromptResult` | `struct : Result { vector<PromptMessage> messages; optional<string> description; optional<InputRequiredResult> input_required; }` | — | Prompt retrieval |
| `CompleteResult` | `struct : Result { JsonValue completion; }` | default-constructed `null` | Completion result; the payload follows the `completion/complete` shape `{"values":[…]}` |
| `InitializeResult` | `struct : Result { std::string protocol_version; ServerCapabilities capabilities; Implementation server_info; optional<string> instructions; }` | first three mandatory | Handshake result |
| `DiscoverResult` | `struct : Result { vector<string> supported_versions; ServerCapabilities capabilities; Implementation server_info; optional<string> instructions; optional<CacheHint> cache_hint; }` | — | `server/discover` result |
| `SkillsListResult` / `SkillsGetResult` / `ReadDirectoryResult` / `DirectoryChild` | `struct : Result { …; optional<int64_t> ttl_ms; optional<string> cache_scope; }` and friends | `nullopt` | Skill and directory browsing results; caching uses `ttl_ms`/`cache_scope`, not `CacheHint` |
| `CreateMessageResult` / `ListRootsResult` / `Root` | `struct : Result { role; ContentVariant content; string model; optional<stop_reason>; }` / `{ vector<Root> roots; }` / `{ uri; optional<name>; }` | — | Types of the deprecated capabilities |

All of the above is declared in `include/mcp/McpTypes.hpp`.

## MRTR input requests

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `InputRequest` | `struct { std::string method; JsonValue params; }` | both required | A single outstanding input request |
| `InputRequests` | `using InputRequests = std::map<std::string, InputRequest, std::less<>>` | — | Indexed by id, transparent key lookup |
| `InputRequiredResult` | `struct { InputRequests input_requests; std::optional<std::string> request_state; }` | `nullopt` | Continuation payload on the result side |
| `MakeInputRequestForElicitation` / `...ForSampling` / `...ForRoots` | `InputRequest Make...(const *RequestParams&)` | — | Three constructors |
| `MakeInputResponseFromElicitResult` / `...FromCreateMessageResult` / `...FromListRootsResult` | `JsonValue Make...(const ...Result&)` | — | Wrap the user's answer as an `input_responses` entry |
| `IsInputRequiredResult` / `ExtractInputRequests` | `bool (const JsonValue&)` / `std::optional<InputRequests> (const JsonValue&)` | — | Detect input_required in a raw result |
| `ElicitRequestParams` | `struct { std::string message; optional<JsonValue> requested_schema; std::string mode{"form"}; optional<string> url; optional<string> elicitation_id; }` | `mode = "form"` | Server-initiated elicitation input |
| `ElicitResult` | `struct : Result { std::string action; optional<JsonValue> content; }` | `action` has no default | Wire result (`accept`/`decline`/`cancel`) |
| `ElicitResultTyped<T>` | `template <typename T> struct { std::string action = "cancel"; optional<T> content; bool is_accepted() const; }` | `action = "cancel"` | Typed answer view; `is_accepted()` is the **template's only snake_case member** — do not rename it |

All of the above is declared in `include/mcp/McpTypes.hpp`.

## Task-related types

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `TaskStatus` | `enum class { Pending, Working, Completed, Failed, Cancelled, InputRequired }` | `Pending` | Task state machine |
| `TaskState` | `struct` (17 fields: `task_id`/`owner`/`status`/`result`/`error_message`/`input_required`/`progress`/`progress_total`/`created_at`/`status_message`/`last_updated_at`/`ttl_ms`/`poll_interval_ms`/`input_requests`/`input_responses`/`tool_name`/`tool_arguments`) | `status = Pending`, `progress = 0` | Defined in the **server** header, not in `McpTypes.hpp`; storage details in the [Process and storage reference](/en/reference/process-and-storage) |
| `CreateTaskResult` | `struct : Result { task_id; status; created_at; optional<status_message/last_updated_at/ttl_ms/poll_interval_ms/input_requests>; }` | first three mandatory | Result of creating a task |
| `GetTaskResult` | `struct` (**does not inherit** `Result`, 14 fields, incl. `task_result_type{"task"}`, `error`, `error_message`, `input_required`, `meta`) | `task_result_type = "task"` | Different base class from `CreateTaskResult`; do not conflate |
| `UpdateTaskResult` / `CancelTaskResult` | `using ... = EmptyResult` | — | No own fields |
| `GetTaskRequestParams` / `UpdateTaskRequestParams` / `CancelTaskRequestParams` / `TaskStatusNotificationParams` | `{ task_id }` / `{ task_id, optional<result>, optional<input_responses> }` / `{ task_id, optional<reason> }` / `{ task_id, status }` | by value | Inputs of the task methods and the status notification |
| `SkillsGetRequestParams` / `ReadDirectoryRequestParams` | `{ uri, optional<RequestMeta> meta }` / `{ uri, optional<string> cursor, optional<RequestMeta> meta }` | `uri` required | Skill retrieval and directory browsing inputs |

Results and inputs are declared in `include/mcp/McpTypes.hpp`; `TaskStatus` and `TaskState` are declared in `include/mcp/server/McpTaskStore.hpp`.

## Request options

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `RequestOptions::meta` | `std::optional<JsonValue>` | `nullopt` | Outgoing `_meta` |
| `RequestOptions::read_timeout_ms` | `std::optional<int64_t>` | `nullopt` | Read timeout (distinct from the total timeout) |
| `RequestOptions::input_responses` | `std::optional<JsonValue>` | `nullopt` | MRTR continuation payload |
| `RequestOptions::request_state` | `std::optional<std::string>` | `nullopt` | Paired with `input_responses` |
| `RequestOptions::on_progress` | `std::function<void(const ProgressNotificationParams&)>` | unset | Equivalent to `mcp::ProgressCallback` (the alias is in `include/mcp/client/McpClient.hpp`) |
| `RequestOptions::timeout` | `std::chrono::milliseconds` | `kDefaultRequestTimeout` = `60000` | Total timeout |
| `RequestOptions::cancel_on_timeout` | `bool` | `true` | Cancel as soon as the deadline hits |
| `RequestOptions::cancel_token` | `CancelToken` | default (not cancelled) | The member is a value type, not a pointer |
| `CacheableRequestOptions` | `struct : RequestOptions { optional<string> cache_mode; optional<int64_t> max_age_ms; }` | `nullopt` | Cache entry point for read-shaped requests |

::: info
`RequestOptions` has **8** fields, not the four-field shape shown in the migration notes.
:::

All of the above is declared in `include/mcp/McpTypes.hpp`.

## Cancellation and errors

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `CancelToken` | `class MCP_API CancelToken` (value semantics, internal `shared_ptr<State>`) | not cancelled | Copyable; copies share one state across threads |
| `Cancel` | `void Cancel(std::string_view reason = {})` | empty reason | Sets the flag and broadcasts handlers |
| `IsCancelled` / `Reason` | `bool () const` / `std::string () const` | `false` / `""` | State queries |
| `AddCancelHandler` / `RemoveCancelHandler` | `std::size_t AddCancelHandler(CancelHandler)` / `void RemoveCancelHandler(std::size_t)` | — | Registration returns a handle id for removal; `CancelHandler = std::function<void(std::string_view)>` |
| `McpErrorCode` | `enum class McpErrorCode : int32_t` | — | JSON-RPC standard codes plus SDK extensions; the full table is in the [Error codes reference](/en/reference/errors) |
| `make_error_code` / `McpErrorCategory` / `GetMcpErrorCategory` | `std::error_code` / `class std::error_category` / `const McpErrorCategory&` | — | `is_error_code_enum` is specialised, so implicit conversion to `std::error_code` works |
| `McpError` | `class McpError : public std::runtime_error` | — | The only business exception type |
| `McpError` constructors | `McpError(McpErrorCode, std::string message)` / `McpError(McpErrorCode, std::string, std::optional<JsonValue> data)` | — | The second overload carries `data` |
| `Code` / `ErrorMessage` / `Data` | `McpErrorCode Code() const noexcept` / `std::string_view ErrorMessage() const noexcept` / `const std::optional<JsonValue>& Data() const noexcept` | — | All three accessors are `noexcept` |

`CancelToken` is declared in `include/mcp/CancelToken.hpp`, the codes and category in `include/mcp/ErrorCodes.hpp`, the exception type in `include/mcp/McpError.hpp`.

## Logging and observability

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `LogLevel` | `enum class LogLevel : int { Off=0, Error=1, Warning=2, Info=3, Debug=4, Trace=5 }` | `Off` | SDK-internal log level; a different scale from the protocol-level `LoggingLevel` |
| `MCP_LOG_LEVEL` | environment variable `0`–`5` | unset = all off | Read on first touch of the log state; values `≥5` map to `Trace` |
| `LogContext` | `struct { std::string request_id; std::string method; std::string session_id; std::string peer; }` | all empty | Carried by `MCP_LOG_CTX` |
| `LogRecord` | `struct { LogLevel level; string_view tag; string_view message; const char* file; int line; const LogContext* context; }` | — | Handler argument; `tag`/`message` stay valid only during the callback |
| `LogHandler` | `using LogHandler = std::function<void(const LogRecord&)>` | — | While a handler is installed, nothing is written to `stderr` |
| `SetLogLevel` / `GetLogLevel` | `void SetLogLevel(LogLevel)` / `LogLevel GetLogLevel()` | `Off` | Override the environment variable at runtime |
| `SetLogHandler` / `GetLogHandler` | `void SetLogHandler(LogHandler)` / `LogHandler GetLogHandler()` | unset | Passing `nullptr` restores the default `stderr` sink |
| `MCP_LOG` / `MCP_LOG_CTX` | macros | — | Used inside the SDK; the level name must match a `LogLevel` enumerator |
| `SpanKind` | `enum class SpanKind : std::uint8_t { ServerRequest, TransportReceive, TransportSend }` | `ServerRequest` | Span category |
| `SpanPhase` | `enum class SpanPhase : std::uint8_t { Begin, End }` | `Begin` | The two phases of a paired event |
| `SpanEvent` | `struct { SpanPhase phase; SpanKind kind; string name/method/request_id/traceparent/tracestate/baggage; bool failed; }` | `failed = false` | Trace context comes from `RequestMeta` |
| `SpanHandler` | `using SpanHandler = std::function<void(const SpanEvent&)>` | — | Injection points are the server/client options, see those reference pages |

The logging side is declared in `include/mcp/Log.hpp`, the span side in `include/mcp/SpanHooks.hpp`.

## Method names and version constants

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `methods::k*` | `inline constexpr std::string_view` | — | Request method names (`initialize`, `tools/call`, `server/discover`, `subscriptions/listen`, `tasks/*`, `skills/*`, `logging/setLevel`, ...); never hand-write literals |
| `notifications::k*` | `inline constexpr std::string_view` | — | Notification method names (including `notifications/tasks/*` and `notifications/elicitation/complete`) |
| `kProtocolVersions` | `inline constexpr std::string_view[]` | 5 entries | `2024-11-05` … `2026-07-28` |
| `kLatestProtocolVersion` / `kModernProtocolVersion` / `kLegacyProtocolVersion` / `kDefaultNegotiatedProtocolVersion` | `inline constexpr std::string_view` | `"2026-07-28"` / `"2026-07-28"` / `"2025-11-25"` / `"2025-03-26"` | Two-era decision basis; the fallback when a version declaration is missing |
| `IsModernProtocolVersion` | `constexpr bool IsModernProtocolVersion(std::string_view)` | — | Lexicographic comparison `>= kLatestProtocolVersion` |
| `kJsonRpcVersion` / `kSdkVersion` | `inline constexpr std::string_view` | `"2.0"` / `kSdkVersion` changes with each release | Envelope version and SDK version (the latter feeds the server discovery defaults, so the docs never hard-code the number) |

Method names are in `include/mcp/Methods.hpp`, the version constants in `include/mcp/ProtocolVersion.hpp` and `include/mcp/McpVersion.hpp`.

## Serialisation and modelling conventions

| Convention | Form | Default | Notes |
|---|---|---|---|
| Type conversion entry points | one pair of free functions per struct: `JsonValue SerializeX(const X&)` / `X DeserializeX(const JsonValue&)` | — | There is no member `to_json`/`from_json`; lookup happens via ADL on the free functions |
| Global ETag switch | `void SetEtagCacheEnabled(bool)` / `bool IsEtagCacheEnabled()` | — | Process-wide; decides whether `CacheableResult` carries an `etag` |
| Optional fields | `std::optional<T>` | `std::nullopt` | "Not set" and "empty string / 0" are two different meanings |
| JSON passed by value | `JsonValue` (never a reference) in parameters and return values | — | `JsonValue` holds a `variant` with deep-copy semantics, so by-value works well with moves |
| Factories return ownership | `McpClient`/`McpServer::Create` return `std::unique_ptr`; `IClientTransport::Connect()` returns `std::shared_ptr<ITransport>` | — | See the [Client reference](/en/reference/client) and the [Transports reference](/en/reference/transports) |
| Rationale | one sentence: the modelling makes "unset" distinguishable by the type system and gives JSON values a clear lifetime, removing sentinel values and dangling references | — | The conventions live in the docs, not in code comments; intent is expressed through types |

Headers involved: `include/mcp/McpTypes.hpp`, `include/mcp/Content.hpp`, `include/mcp/Capabilities.hpp`, `include/mcp/JsonValue.hpp`, `include/mcp/JsonRpc.hpp`, `include/mcp/CancelToken.hpp`, `include/mcp/Transport.hpp`.

Task-shaped guides: [/en/howto/validate-tool-input](/en/howto/validate-tool-input), [/en/howto/report-progress](/en/howto/report-progress).

## Source files behind this page

- `include/mcp/JsonValue.hpp`, `include/mcp/JsonRpc.hpp`, `include/mcp/Content.hpp`, `include/mcp/Meta.hpp`
- `include/mcp/Capabilities.hpp`, `include/mcp/Implementation.hpp`, `include/mcp/McpTypes.hpp`, `include/mcp/Methods.hpp`
- `include/mcp/Log.hpp`, `include/mcp/SpanHooks.hpp`, `include/mcp/CancelToken.hpp`
- `include/mcp/ErrorCodes.hpp`, `include/mcp/McpError.hpp`, `include/mcp/server/McpTaskStore.hpp` (`TaskState`/`TaskStatus`)
- `include/mcp/ProtocolVersion.hpp`, `include/mcp/McpVersion.hpp`
