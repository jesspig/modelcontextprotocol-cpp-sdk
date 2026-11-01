# Types Reference

This page is the **authoritative field table** for the core type layer; every entry is checked against the headers. Facade signatures live in the [Server reference](/en/reference/server) and [Client reference](/en/reference/client), the complete error-code table in the [Error codes reference](/en/reference/errors), and the conceptual narrative in [Protocol versions](/en/learn/protocol-versions). No procedural steps on this page.

## `JsonValue` (a type owned by this repository)

::: danger
`mcp::JsonValue` is **not nlohmann::json** and there is no alias or compatibility shim. `parse()`/`dump()`/`contains()`/`value()`/`is_string()`/`push_back()`/`json::object()` do not exist here and copying nlohmann style fails to compile; build an empty object/array with `JsonValue(JsonValue::object_tag)` / `JsonValue(JsonValue::array_tag)` and use the capitalised accessors `Parse`/`Dump`/`Contains`/`Find`/`At`/`PushBack`/`IsString`.
:::

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `JsonValue::Object` | `using Object = std::map<std::string, JsonValue, std::less<>>` | — | Transparent comparison, so `string_view` keys work | `include/mcp/JsonValue.hpp:21` |
| `JsonValue::Array` | `using Array = std::vector<JsonValue>` | — | Array container | `JsonValue.hpp:22` |
| Internal storage | `std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object>` | `nullptr` | Seven alternatives; no uint/float branch | `JsonValue.hpp:25` |
| Scalar constructors | `JsonValue(bool/int/int64_t/double/std::string&&/const std::string&/const char*/const char* + size_t)` | — | `int` is always widened to `int64_t` | `JsonValue.hpp:28-39` |
| Container constructors | `JsonValue(Array v)` / `JsonValue(Object v)` / `JsonValue(ObjectTag)` / `JsonValue(ArrayTag)` | — | The two tag enums are `object_tag` / `array_tag` | `JsonValue.hpp:38-44` |
| `Parse` | `static JsonValue Parse(std::string_view json)` | — | Throws on malformed input | `JsonValue.hpp:52` |
| `SanitizeUtf8` | `static std::string SanitizeUtf8(std::string_view text)` | — | Strips invalid UTF-8 sequences | `JsonValue.hpp:54` |
| `Dump` | `std::string Dump(int indent = -1) const` | `-1` (compact) | Serialises back to a string | `JsonValue.hpp:56` |
| Type checks | `IsNull`/`IsBool`/`IsInt`/`IsDouble`/`IsNumber`/`IsString`/`IsArray`/`IsObject` | — | All return `bool`; `IsNumber` = `IsInt \|\| IsDouble` | `JsonValue.hpp:58-65` |
| Getters | `GetBool`/`GetInt`/`GetDouble`/`GetString`/`GetArray`/`GetObject` | — | Throw on a type mismatch; `GetString` returns `const std::string&`, `GetArray`/`GetObject` also have mutable overloads | `JsonValue.hpp:67-74` |
| Container info | `size_t Size() const` / `bool Empty() const` / `bool Contains(std::string_view) const` | — | Work on both arrays and objects | `JsonValue.hpp:76-78` |
| Indexing | `JsonValue& operator[](size_t)` / `JsonValue& operator[](std::string_view)` | — | `const` and non-`const` overloads | `JsonValue.hpp:80-84` |
| `Find` | `const JsonValue* Find(std::string_view key) const` / non-const overload | `nullptr` when absent | Preferred for optional fields; never throws | `JsonValue.hpp:86-87` |
| `At` | `const JsonValue& At(std::string_view key) const` / non-const overload | — | Throws when the key is missing | `JsonValue.hpp:89-90` |
| `PushBack` | `void PushBack(JsonValue val)` | — | Appends by value | `JsonValue.hpp:92` |
| Equality | `bool operator==(const JsonValue&) const` / `operator!=` | — | Structural deep comparison | `JsonValue.hpp:94-95` |
| Range-for | `begin()` / `end()` | — | **Only objects are iterable** (they forward to `GetObject()`); arrays need an explicit `GetArray()` | `JsonValue.hpp:97-100` |
| `GetObject` macro guard | `#pragma push_macro("GetObject")` / `pop_macro` | — | Defends against the same-named Windows header macro; do not remove | `JsonValue.hpp:11-15`、`105-108` |

## JSON-RPC envelope

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `RequestId` | `using RequestId = std::variant<int64_t, std::string>` | — | Numeric or string id | `include/mcp/JsonRpc.hpp:15` |
| `ErrorData` | `struct { McpErrorCode code; std::string message; std::optional<JsonValue> data; }` | `code = InternalError` | Error payload carrier | `JsonRpc.hpp:17-21` |
| `JsonRpcRequest` | `struct` with `jsonrpc`/`id`/`method`/`params`/`meta`/`session_id` | `jsonrpc = "2.0"` | `params` and `meta` are `optional<JsonValue>`; `session_id` is filled by the transport | `JsonRpc.hpp:23-30` |
| `JsonRpcNotification` | `struct`, same minus `id` | `jsonrpc = "2.0"` | No id | `JsonRpc.hpp:32-38` |
| `JsonRpcResponse` | `struct { std::string jsonrpc; RequestId id; JsonValue result; std::string session_id; }` | `"2.0"` | `result` held by value | `JsonRpc.hpp:40-45` |
| `JsonRpcErrorResponse` | `struct { ...; std::optional<RequestId> id; ErrorData error; ... }` | `"2.0"` | id may be absent (parse failures) | `JsonRpc.hpp:47-52` |
| `JsonRpcMessage` | `std::variant<JsonRpcRequest, JsonRpcNotification, JsonRpcResponse, JsonRpcErrorResponse>` | — | The only message type on the wire in the transport layer | `JsonRpc.hpp:54-58` |
| `SerializeMessage` | `std::string SerializeMessage(const JsonRpcMessage&)` / `(JsonRpcMessage&&)` | — | The move overload avoids a deep copy | `JsonRpc.hpp:60-61` |
| `DeserializeMessage` | `JsonRpcMessage DeserializeMessage(std::string_view json)` | — | Throws `McpError` on failure | `JsonRpc.hpp:62` |
| `Is*` / `As*` | `IsRequest`/`IsNotification`/`IsResponse`/`IsError`, `AsRequest`/`AsNotification`/`AsResponse`/`AsError` | — | `As*` return `std::get_if` pointers and never throw | `JsonRpc.hpp:64-87` |
| `RequestIdToJson` / `RequestIdFromJson` | `JsonValue ...` / `RequestId ...` | — | Convert ids to and from wire form | `JsonRpc.hpp:89-90` |

## Content variants

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `Icon` | `struct { std::string src; optional<string> mime_type; optional<vector<string>> sizes; optional<string> theme; }` | `src` required | Icon entry | `include/mcp/Content.hpp:13-18` |
| `Annotations` | `struct { optional<vector<string>> audience; optional<double> priority; optional<string> last_modified; }` | all `nullopt` | Content annotations | `Content.hpp:20-24` |
| `TextResourceContents` | `struct { uri; text; optional<mime_type>; optional<JsonValue> meta; }` | — | Textual resource body | `Content.hpp:26-31` |
| `BlobResourceContents` | `struct { uri; blob; optional<mime_type>; optional<JsonValue> meta; }` | — | Base64 resource body | `Content.hpp:33-38` |
| `ResourceContents` | `std::variant<TextResourceContents, BlobResourceContents>` | — | One of the two resource bodies | `Content.hpp:40` |
| `TextContent` | `struct { std::string type = "text"; std::string text; optional<string> mime_type; optional<Annotations> annotations; optional<JsonValue> meta; }` | `type = "text"` | Note `type` has an in-struct default, yet aggregate initialisation still passes it explicitly | `Content.hpp:42-48` |
| `ImageContent` / `AudioContent` | `struct { type("image"/"audio"); data; mime_type; optional<annotations>; optional<meta>; }` | `type` built in | `mime_type` is **not** optional | `Content.hpp:50-64` |
| `EmbeddedResource` | `struct { type("resource"); ResourceContents resource; optional<annotations>; optional<meta>; }` | — | Embedded resource | `Content.hpp:66-71` |
| `ResourceLink` | `struct { type("resource_link"); uri; name; optional<title/description/mime_type/size>; vector<Icon> icons; optional<annotations>; optional<meta>; }` | — | Resource link | `Content.hpp:73-84` |
| `ContentVariant` | `std::variant<TextContent, ImageContent, AudioContent, EmbeddedResource, ResourceLink>` | — | Element type of `CallToolResult::content` | `Content.hpp:86` |

## `_meta`, progress tokens and cache hints

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `ProgressToken` | `using ProgressToken = std::variant<std::string, int64_t>` | — | `_meta.progressToken` | `include/mcp/Meta.hpp:15` |
| `LoggingLevel` | `enum class LoggingLevel { Debug, Info, Notice, Warning, Error, Critical, Alert, Emergency }` | — | **Protocol-level** log severity, unrelated to the SDK's internal `LogLevel` | `Meta.hpp:17-19` |
| `RequestMeta.progress_token` | `std::optional<ProgressToken>` | `nullopt` | Progress correlation token | `Meta.hpp:22` |
| `RequestMeta.protocol_version` | `std::string` | `kLatestProtocolVersion` (`"2026-07-28"`) | Version stamp on outgoing `_meta` | `Meta.hpp:23` |
| `RequestMeta.client_info` / `client_capabilities` | `optional<Implementation>` / `optional<ClientCapabilities>` | `nullopt` | Populated only during `initialize` | `Meta.hpp:24-25` |
| `RequestMeta.log_level` | `std::optional<LoggingLevel>` | `nullopt` | Log level requested by the client | `Meta.hpp:26` |
| `RequestMeta.extensions` | `std::optional<JsonValue>` | `nullopt` | Extension slot inside `_meta` | `Meta.hpp:27` |
| `RequestMeta.traceparent` / `tracestate` / `baggage` | `std::optional<std::string>` ×3 | `nullopt` | W3C trace context, fed to the span hooks | `Meta.hpp:28-30` |
| `CacheHint.ttl_ms` / `cache_scope` | `std::optional<int64_t>` / `std::optional<std::string>` | `nullopt` | Cache hints on list results | `Meta.hpp:33-36` |

## Capabilities

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `ToolsCapability` / `PromptsCapability` / `RootsCapability` | `struct { std::optional<bool> list_changed; }` | `nullopt` | Identical shape | `include/mcp/Capabilities.hpp:12-14`、`21-23`、`27-29` |
| `ResourcesCapability` | `struct { optional<bool> subscribe; optional<bool> list_changed; }` | `nullopt` | The only one carrying `subscribe` | `Capabilities.hpp:16-19` |
| `SamplingCapability` / `EmptyCapability` | `struct {}` | — | Empty markers; `LoggingCapability`, `TasksCapability`, `SubscriptionsCapability`, `CompletionsCapability` are aliases of `EmptyCapability` | `Capabilities.hpp:25`、`36`、`38-41` |
| `ElicitationCapability` | `struct { std::optional<JsonValue> form; std::optional<JsonValue> url; }` | `nullopt` | Declares supported elicitation shapes | `Capabilities.hpp:31-34` |
| `kTasksExtensionId` / `kSkillsExtensionId` | `inline constexpr std::string_view` | `"io.modelcontextprotocol/tasks"` / `".../skills"` | Extension declaration keys | `Capabilities.hpp:43-44` |
| `TasksExtensionSettings` / `SkillsExtensionSettings` | `struct {}` / `struct { bool directory_read{false}; }` | `false` | Extension settings bodies | `Capabilities.hpp:46-50` |
| `ServerCapabilities` | `struct`: `tools`/`resources`/`prompts`/`logging`/`sampling`/`roots`/`elicitation`/`completions`/`subscriptions` all `optional<>`, plus `optional<map<string, JsonValue>> extensions` and `optional<JsonValue> experimental` | all `nullopt` | 11 fields in total | `Capabilities.hpp:52-64` |
| `ClientCapabilities` | `struct`: `roots`/`sampling`/`elicitation`/`extensions`/`experimental` | all `nullopt` | 5 fields in total | `Capabilities.hpp:66-72` |

## Implementation information

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `Implementation` | `struct { std::string name; std::string version; optional<string> title; vector<Icon> icons; optional<string> description; optional<string> website_url; }` | `name`/`version` required, `icons = {}` | Used for both `client_info` and `server_info` | `include/mcp/Implementation.hpp:11-18` |

## Declaration types: `Tool` and `ToolOptions`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `Tool::name` | `std::string` | empty (required) | Registration name | `include/mcp/McpTypes.hpp:51` |
| `Tool::title` / `description` | `std::optional<std::string>` | `nullopt` | Human-readable metadata | `McpTypes.hpp:52-53` |
| `Tool::input_schema` | `JsonValue` (**not** optional) | default-constructed = `null` | Mandatory on the wire; the default schema is supplied during registration from `ToolOptions` | `McpTypes.hpp:54` |
| `Tool::output_schema` | `std::optional<JsonValue>` | `nullopt` | Structured-output schema | `McpTypes.hpp:55` |
| `Tool::annotations` | `std::optional<ToolAnnotations>` | `nullopt` | Behavioural annotations | `McpTypes.hpp:56` |
| `Tool::icons` | `std::vector<Icon>` | `{}` | Icon set | `McpTypes.hpp:57` |
| `Tool::meta` | `std::optional<JsonValue>` | `nullopt` | `_meta` pass-through | `McpTypes.hpp:58` |
| `Tool::execution` | `std::optional<ToolExecution>` | `nullopt` | Task / human-in-the-loop execution declaration | `McpTypes.hpp:59` |
| `ToolOptions` | `struct`, 13 fields + 3 fluent setters | see below | Registration input; it does not map one-to-one onto `Tool` | `McpTypes.hpp:523-541` |
| `ToolOptions::name` | `std::optional<std::string>` | `nullopt` | Never consumed: the effective name comes from the `name` argument of `RegisterTool`/`Create` | `McpTypes.hpp:524` |
| `ToolOptions::{description,title,destructive,idempotent,read_only_hint,open_world_hint}` | `std::optional<std::string>` ×2 + `std::optional<bool>` ×4 | `nullopt` | The four bools are assembled into `ToolAnnotations` | `McpTypes.hpp:525-530` |
| `ToolOptions::use_structured_content` | `bool` | `false` | When true, `output_schema` is set to `{"type":"object"}` | `McpTypes.hpp:531` |
| `ToolOptions::{input_schema,output_schema,icons,meta,execution}` | `optional<JsonValue>` ×3 (incl. `meta`), `vector<Icon>`, `optional<ToolExecution>` | `nullopt` / `{}` | **Assign these members directly** | `McpTypes.hpp:532-536` |
| `ToolOptions::Description` / `Title` / `InputSchema` | `ToolOptions& Description(std::string_view)`, `ToolOptions& Title(std::string_view)`, `ToolOptions& InputSchema(JsonValue)` | — | These are the **only** fluent setters; every other field must be assigned | `McpTypes.hpp:538-540` |

::: warning
`ToolOptions` has no `Name()`, `OutputSchema()`, `Icons()`, `Meta()`, `Execution()` or `Destructive()` setter — `ToolOptions{}.OutputSchema(x)` does not compile.
:::

## Remaining declaration types

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `Resource` | `struct { uri; name; optional<title/description/mime_type/size>; optional<vector<Icon>> icons; optional<ResourceAnnotations> annotations; optional<JsonValue> meta; }` | `uri`/`name` required | Static resource descriptor | `McpTypes.hpp:62-72` |
| `ResourceTemplate` | `struct`, first field `uri_template`, otherwise like `Resource` (minus `size`) | `uri_template`/`name` required | Templated resource descriptor | `McpTypes.hpp:74-83` |
| `ResourceAnnotations` | `struct { optional<vector<string>> audience; optional<double> priority; optional<string> last_modified; }` | all `nullopt` | Same shape as `Annotations` but a distinct type | `McpTypes.hpp:44-48` |
| `ToolAnnotations` | `struct { optional<string> title; optional<bool> read_only_hint/idempotent_hint/open_world_hint/destructive_hint; }` | all `nullopt` | Assembled from the four `ToolOptions` bools | `McpTypes.hpp:28-34` |
| `ToolExecutionMode` / `TaskSupport` | `enum class { Auto, Manual, Task }` / `enum class { Optional, Required }` | `Auto` / no default | Execution mode and task-support strength | `McpTypes.hpp:36-37` |
| `ToolExecution` | `struct { ToolExecutionMode mode{Auto}; optional<string> human_use; optional<TaskSupport> task_support; }` | `mode = Auto` | Tool execution declaration | `McpTypes.hpp:38-42` |
| `Prompt` | `struct { name; optional<title/description>; optional<vector<PromptArgument>> arguments; optional<vector<Icon>> icons; optional<JsonValue> meta; }` | `name` required | Prompt descriptor | `McpTypes.hpp:91-98` |
| `PromptArgument` | `struct { name; optional<description>; optional<bool> required; }` | `name` required | Argument declaration | `McpTypes.hpp:85-89` |
| `PromptMessage` | `struct { std::string role; ContentVariant content; }` | — | Entry returned by `prompts/get` | `McpTypes.hpp:100-103` |
| `ResourceOptions` | `struct { optional<name/description/title/mime_type>; vector<Icon> icons; }` + `Description`/`Title`/`MimeType` | `nullopt` / `{}` | Three setters, everything else assigned directly; `name` is never consumed — the effective name is the `name` argument of `RegisterResource` (`McpServer.cpp:795`) | `McpTypes.hpp:543-553` |
| `PromptOptions` | `struct { optional<name/description/title>; vector<Icon> icons; optional<vector<PromptArgument>> arguments; }` + `Description`/`Title`/`Arguments` | `nullopt` / `{}` | Three setters, everything else assigned directly; `name` is never consumed — the effective name is the `name` argument of `RegisterPrompt` (`McpServer.cpp:885`) | `McpTypes.hpp:555-565` |
| `SkillResource` | `struct { std::string uri; std::string digest; int64_t size{0}; }` | `size = 0` | Digest of a file inside a skill | `McpTypes.hpp:481-485` |
| `SkillResources` | `std::variant<std::vector<SkillResource>, std::string>` | — | Either an explicit list or a directory reference | `McpTypes.hpp:487` |
| `Skill` | `struct { std::string uri; JsonValue frontmatter; SkillResources resources; optional<JsonValue> meta; }` | `uri`/`frontmatter`/`resources` required | Skill declaration (`skills/*`) | `McpTypes.hpp:489-494` |

## Request parameter types

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `PaginatedRequestParams` | `struct { optional<string> cursor; optional<RequestMeta> meta; }` | `nullopt` | Aliased as `ListToolsRequestParams`, `ListResourcesRequestParams`, `ListResourceTemplatesRequestParams`, `ListPromptsRequestParams`, `ListSkillsRequestParams` | `McpTypes.hpp:119-126`、`468` |
| `ResourceRequestParams` | `struct { std::string uri; optional<RequestMeta> meta; optional<JsonValue> input_responses; optional<string> request_state; }` | `uri` required | Aliased as `ReadResourceRequestParams`, `SubscribeRequestParams`, `UnsubscribeRequestParams` | `McpTypes.hpp:128-136` |
| `CallToolRequestParams` | `struct { name; optional<JsonValue> arguments; optional<RequestMeta> meta; optional<JsonValue> input_responses; optional<string> request_state; }` | `name` required | MRTR continuation relies on the last two fields | `McpTypes.hpp:138-144` |
| `GetPromptRequestParams` | `struct { name; optional<JsonValue> arguments; optional<RequestMeta> meta; optional<JsonValue> input_responses; optional<string> request_state; }` | `name` required | Same shape as `CallTool` | `McpTypes.hpp:191-197` |
| `CompleteRequestParams` | `struct { JsonValue ref; std::string argument_name; std::string argument_value; map<string,string,less<>> context_arguments; optional<RequestMeta> meta; }` | `context_arguments = {}` | `ref` is a raw object (`{"type":"ref/prompt","name":...}`), not a strongly typed variant | `McpTypes.hpp:199-205` |
| `InitializeRequestParams` | `struct { std::string protocol_version; ClientCapabilities capabilities; Implementation client_info; }` | no optionals | All three fields mandatory | `McpTypes.hpp:209-213` |
| `DiscoverRequestParams` / `ListRootsRequestParams` | `struct {}` | — | Empty-parameter requests | `McpTypes.hpp:207`、`243` |
| `Pagination` | `struct { std::optional<std::string> next_cursor; }` | `nullopt` | Cursor envelope | `McpTypes.hpp:105-107` |
| `FileRef` | `struct { std::string uri; int64_t offset{0}; int64_t length{0}; }` | `0` | `x-mcp-fileref` argument; zero-valued fields are omitted during serialisation | `McpTypes.hpp:146-158` |
| `SubscriptionFilter` | `struct { optional<bool> tools_list_changed/prompts_list_changed/resources_list_changed; vector<string> resource_subscriptions; }` | `nullopt` / `{}` | `subscriptions/listen` filter | `McpTypes.hpp:215-220` |
| `SubscriptionsListenRequestParams` | `struct { SubscriptionFilter notifications; optional<RequestMeta> meta; }` | — | Listen-subscription input | `McpTypes.hpp:222-225` |
| `CreateMessageRequestParams` / `SamplingMessage` | `struct { vector<SamplingMessage> messages; int64_t max_tokens; optional<string> stop_reason/model_preference; }` / `struct { role; ContentVariant content; }` | `max_tokens` required | `sampling/*` is deprecated (SEP-2577); the types remain | `McpTypes.hpp:231-241` |
| `SetLevelRequestParams` | `struct { LoggingLevel level; }` | — | Input of `logging/setLevel` | `McpTypes.hpp:413-415` |
| `LoggingMessageNotificationParams` | `struct { LoggingLevel level; optional<string> logger; JsonValue data; }` | `logger = nullopt` | `notifications/message` | `McpTypes.hpp:344-348` |
| `ProgressNotificationParams` | `struct { ProgressToken progress_token; double progress; optional<double> total; optional<string> message; }` | — | Argument of the `on_progress` callback | `McpTypes.hpp:332-337` |
| `CancelledNotificationParams` | `struct { RequestId request_id; optional<string> reason; }` | — | Cancellation notification | `McpTypes.hpp:339-342` |

## Result types

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `Result` | `struct { std::optional<JsonValue> meta; ResultType result_type{Complete}; }` | `Complete` | Base of every result; `ResultType = { Complete, InputRequired, Task }` | `McpTypes.hpp:109`、`114-117` |
| `EmptyResult` / `PingResult` | `struct EmptyResult : Result {}`; `using PingResult = EmptyResult` | — | Empty result | `McpTypes.hpp:257`、`330` |
| `CacheableResult` | `struct CacheableResult : Result { std::optional<std::string> etag; }` | `nullopt` | ETag governed by the global switch | `McpTypes.hpp:259-266` |
| `CallToolResult` | `struct : Result { vector<ContentVariant> content; optional<JsonValue> structured_content; bool is_error{false}; optional<InputRequiredResult> input_required; }` | `is_error = false` | Four own fields plus the two inherited ones | `McpTypes.hpp:268-273` |
| `ListToolsResult` / `ListResourcesResult` / `ListResourceTemplatesResult` / `ListPromptsResult` | `struct : Result { vector<…>; optional<string> next_cursor; optional<CacheHint> cache_hint; }` | `nullopt` | Four isomorphic paginated list results | `McpTypes.hpp:275-303` |
| `ReadResourceResult` | `struct : Result { vector<ResourceContents> contents; optional<CacheHint> cache_hint; optional<InputRequiredResult> input_required; }` | — | Resource read | `McpTypes.hpp:293-297` |
| `GetPromptResult` | `struct : Result { vector<PromptMessage> messages; optional<string> description; optional<InputRequiredResult> input_required; }` | — | Prompt retrieval | `McpTypes.hpp:305-309` |
| `CompleteResult` | `struct : Result { JsonValue completion; }` | default-constructed `null` | Completion result; the payload follows the `completion/complete` shape `{"values":[…]}` | `McpTypes.hpp:311-313` |
| `InitializeResult` | `struct : Result { std::string protocol_version; ServerCapabilities capabilities; Implementation server_info; optional<string> instructions; }` | first three mandatory | Handshake result | `McpTypes.hpp:315-320` |
| `DiscoverResult` | `struct : Result { vector<string> supported_versions; ServerCapabilities capabilities; Implementation server_info; optional<string> instructions; optional<CacheHint> cache_hint; }` | — | `server/discover` result | `McpTypes.hpp:322-328` |
| `SkillsListResult` / `SkillsGetResult` / `ReadDirectoryResult` / `DirectoryChild` | `struct : Result { …; optional<int64_t> ttl_ms; optional<string> cache_scope; }` and friends | `nullopt` | Skill and directory browsing results; caching uses `ttl_ms`/`cache_scope`, not `CacheHint` | `McpTypes.hpp:496-521` |
| `CreateMessageResult` / `ListRootsResult` / `Root` | `struct : Result { role; ContentVariant content; string model; optional<stop_reason>; }` / `{ vector<Root> roots; }` / `{ uri; optional<name>; }` | — | Types of the deprecated capabilities | `McpTypes.hpp:378-392` |

## MRTR input requests

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `InputRequest` | `struct { std::string method; JsonValue params; }` | both required | A single outstanding input request | `McpTypes.hpp:245-248` |
| `InputRequests` | `using InputRequests = std::map<std::string, InputRequest, std::less<>>` | — | Indexed by id, transparent key lookup | `McpTypes.hpp:250` |
| `InputRequiredResult` | `struct { InputRequests input_requests; std::optional<std::string> request_state; }` | `nullopt` | Continuation payload on the result side | `McpTypes.hpp:252-255` |
| `MakeInputRequestForElicitation` / `...ForSampling` / `...ForRoots` | `InputRequest Make...(const *RequestParams&)` | — | Three constructors | `McpTypes.hpp:371-373` |
| `MakeInputResponseFromElicitResult` / `...FromCreateMessageResult` / `...FromListRootsResult` | `JsonValue Make...(const ...Result&)` | — | Wrap the user's answer as an `input_responses` entry | `McpTypes.hpp:374`、`394-395` |
| `IsInputRequiredResult` / `ExtractInputRequests` | `bool (const JsonValue&)` / `std::optional<InputRequests> (const JsonValue&)` | — | Detect input_required in a raw result | `McpTypes.hpp:375-376` |
| `ElicitRequestParams` | `struct { std::string message; optional<JsonValue> requested_schema; std::string mode{"form"}; optional<string> url; optional<string> elicitation_id; }` | `mode = "form"` | Server-initiated elicitation input | `McpTypes.hpp:350-356` |
| `ElicitResult` | `struct : Result { std::string action; optional<JsonValue> content; }` | `action` has no default | Wire result (`accept`/`decline`/`cancel`) | `McpTypes.hpp:358-361` |
| `ElicitResultTyped<T>` | `template <typename T> struct { std::string action = "cancel"; optional<T> content; bool is_accepted() const; }` | `action = "cancel"` | Typed answer view; `is_accepted()` is the **template's only snake_case member** — do not rename it | `McpTypes.hpp:363-369` |

## Task-related types

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `TaskStatus` | `enum class { Pending, Working, Completed, Failed, Cancelled, InputRequired }` | `Pending` | Task state machine | `include/mcp/server/McpTaskStore.hpp:16-23` |
| `TaskState` | `struct` (17 fields: `task_id`/`owner`/`status`/`result`/`error_message`/`input_required`/`progress`/`progress_total`/`created_at`/`status_message`/`last_updated_at`/`ttl_ms`/`poll_interval_ms`/`input_requests`/`input_responses`/`tool_name`/`tool_arguments`) | `status = Pending`, `progress = 0` | Defined in the **server** header, not in `McpTypes.hpp`; storage details in the [Process and storage reference](/en/reference/process-and-storage) | `McpTaskStore.hpp:25-43` |
| `CreateTaskResult` | `struct : Result { task_id; status; created_at; optional<status_message/last_updated_at/ttl_ms/poll_interval_ms/input_requests>; }` | first three mandatory | Result of creating a task | `McpTypes.hpp:434-443` |
| `GetTaskResult` | `struct` (**does not inherit** `Result`, 14 fields, incl. `task_result_type{"task"}`, `error`, `error_message`, `input_required`, `meta`) | `task_result_type = "task"` | Different base class from `CreateTaskResult`; do not conflate | `McpTypes.hpp:417-432` |
| `UpdateTaskResult` / `CancelTaskResult` | `using ... = EmptyResult` | — | No own fields | `McpTypes.hpp:445-446` |
| `GetTaskRequestParams` / `UpdateTaskRequestParams` / `CancelTaskRequestParams` / `TaskStatusNotificationParams` | `{ task_id }` / `{ task_id, optional<result>, optional<input_responses> }` / `{ task_id, optional<reason> }` / `{ task_id, status }` | by value | Inputs of the task methods and the status notification | `McpTypes.hpp:448-466` |
| `SkillsGetRequestParams` / `ReadDirectoryRequestParams` | `{ uri, optional<RequestMeta> meta }` / `{ uri, optional<string> cursor, optional<RequestMeta> meta }` | `uri` required | Skill retrieval and directory browsing inputs | `McpTypes.hpp:470-479` |

## Request options

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `RequestOptions::meta` | `std::optional<JsonValue>` | `nullopt` | Outgoing `_meta` | `McpTypes.hpp:398` |
| `RequestOptions::read_timeout_ms` | `std::optional<int64_t>` | `nullopt` | Read timeout (distinct from the total timeout) | `McpTypes.hpp:399` |
| `RequestOptions::input_responses` | `std::optional<JsonValue>` | `nullopt` | MRTR continuation payload | `McpTypes.hpp:400` |
| `RequestOptions::request_state` | `std::optional<std::string>` | `nullopt` | Paired with `input_responses` | `McpTypes.hpp:401` |
| `RequestOptions::on_progress` | `std::function<void(const ProgressNotificationParams&)>` | unset | Equivalent to `mcp::ProgressCallback` (`include/mcp/client/McpClient.hpp:27`) | `McpTypes.hpp:402` |
| `RequestOptions::timeout` | `std::chrono::milliseconds` | `kDefaultRequestTimeout` = `60000` | Total timeout | `McpTypes.hpp:403`、`26` |
| `RequestOptions::cancel_on_timeout` | `bool` | `true` | Cancel as soon as the deadline hits | `McpTypes.hpp:404` |
| `RequestOptions::cancel_token` | `CancelToken` | default (not cancelled) | The member is a value type, not a pointer | `McpTypes.hpp:405` |
| `CacheableRequestOptions` | `struct : RequestOptions { optional<string> cache_mode; optional<int64_t> max_age_ms; }` | `nullopt` | Cache entry point for read-shaped requests | `McpTypes.hpp:408-411` |

::: info
`RequestOptions` has **8** fields (`McpTypes.hpp:397-406`), not the four-field shape shown in the migration notes.
:::

## Cancellation and errors

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `CancelToken` | `class MCP_API CancelToken` (value semantics, internal `shared_ptr<State>`) | not cancelled | Copyable; copies share one state across threads | `include/mcp/CancelToken.hpp:13`、`26-28` |
| `Cancel` | `void Cancel(std::string_view reason = {})` | empty reason | Sets the flag and broadcasts handlers | `CancelToken.hpp:19` |
| `IsCancelled` / `Reason` | `bool () const` / `std::string () const` | `false` / `""` | State queries | `CancelToken.hpp:20-21` |
| `AddCancelHandler` / `RemoveCancelHandler` | `std::size_t AddCancelHandler(CancelHandler)` / `void RemoveCancelHandler(std::size_t)` | — | Registration returns a handle id for removal; `CancelHandler = std::function<void(std::string_view)>` | `CancelToken.hpp:15`、`23-24` |
| `McpErrorCode` | `enum class McpErrorCode : int32_t` | — | JSON-RPC standard codes plus SDK extensions; the full table is in the [Error codes reference](/en/reference/errors) | `include/mcp/ErrorCodes.hpp:8-30` |
| `make_error_code` / `McpErrorCategory` / `GetMcpErrorCategory` | `std::error_code` / `class std::error_category` / `const McpErrorCategory&` | — | `is_error_code_enum` is specialised, so implicit conversion to `std::error_code` works | `ErrorCodes.hpp:32`、`36-39`、`43-92`、`94-101` |
| `McpError` | `class McpError : public std::runtime_error` | — | The only business exception type | `include/mcp/McpError.hpp:13` |
| `McpError` constructors | `McpError(McpErrorCode, std::string message)` / `McpError(McpErrorCode, std::string, std::optional<JsonValue> data)` | — | The second overload carries `data` | `McpError.hpp:15-24` |
| `Code` / `ErrorMessage` / `Data` | `McpErrorCode Code() const noexcept` / `std::string_view ErrorMessage() const noexcept` / `const std::optional<JsonValue>& Data() const noexcept` | — | All three accessors are `noexcept` | `McpError.hpp:26-28` |

## Logging and observability

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `LogLevel` | `enum class LogLevel : int { Off=0, Error=1, Warning=2, Info=3, Debug=4, Trace=5 }` | `Off` | SDK-internal log level; a different scale from the protocol-level `LoggingLevel` | `include/mcp/Log.hpp:15-22`、`48` |
| `MCP_LOG_LEVEL` | environment variable `0`–`5` | unset = all off | Read on first touch of the log state; values `≥5` map to `Trace` | `Log.hpp:55-60` |
| `LogContext` | `struct { std::string request_id; std::string method; std::string session_id; std::string peer; }` | all empty | Carried by `MCP_LOG_CTX` | `Log.hpp:24-29` |
| `LogRecord` | `struct { LogLevel level; string_view tag; string_view message; const char* file; int line; const LogContext* context; }` | — | Handler argument; `tag`/`message` stay valid only during the callback | `Log.hpp:31-38` |
| `LogHandler` | `using LogHandler = std::function<void(const LogRecord&)>` | — | While a handler is installed, nothing is written to `stderr` | `Log.hpp:40`、`108-122` |
| `SetLogLevel` / `GetLogLevel` | `void SetLogLevel(LogLevel)` / `LogLevel GetLogLevel()` | `Off` | Override the environment variable at runtime | `Log.hpp:82`、`99` |
| `SetLogHandler` / `GetLogHandler` | `void SetLogHandler(LogHandler)` / `LogHandler GetLogHandler()` | unset | Passing `nullptr` restores the default `stderr` sink | `Log.hpp:86`、`93` |
| `MCP_LOG` / `MCP_LOG_CTX` | macros | — | Used inside the SDK; the level name must match a `LogLevel` enumerator | `Log.hpp:172`、`179` |
| `SpanKind` | `enum class SpanKind : std::uint8_t { ServerRequest, TransportReceive, TransportSend }` | `ServerRequest` | Span category | `include/mcp/SpanHooks.hpp:9-13`、`22` |
| `SpanPhase` | `enum class SpanPhase : std::uint8_t { Begin, End }` | `Begin` | The two phases of a paired event | `SpanHooks.hpp:15-18`、`21` |
| `SpanEvent` | `struct { SpanPhase phase; SpanKind kind; string name/method/request_id/traceparent/tracestate/baggage; bool failed; }` | `failed = false` | Trace context comes from `RequestMeta` | `SpanHooks.hpp:20-30` |
| `SpanHandler` | `using SpanHandler = std::function<void(const SpanEvent&)>` | — | Injection points are the server/client options, see those reference pages | `SpanHooks.hpp:32` |

## Method names and version constants

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `methods::k*` | `inline constexpr std::string_view` | — | Request method names (`initialize`, `tools/call`, `server/discover`, `subscriptions/listen`, `tasks/*`, `skills/*`, `logging/setLevel`, ...); never hand-write literals | `include/mcp/Methods.hpp:8-52` |
| `notifications::k*` | `inline constexpr std::string_view` | — | Notification method names (including `notifications/tasks/*` and `notifications/elicitation/complete`) | `Methods.hpp:56-77` |
| `kProtocolVersions` | `inline constexpr std::string_view[]` | 5 entries | `2024-11-05` … `2026-07-28` | `include/mcp/ProtocolVersion.hpp:13-19` |
| `kLatestProtocolVersion` / `kModernProtocolVersion` / `kLegacyProtocolVersion` / `kDefaultNegotiatedProtocolVersion` | `inline constexpr std::string_view` | `"2026-07-28"` / `"2026-07-28"` / `"2025-11-25"` / `"2025-03-26"` | Two-era decision basis; the fallback when a version declaration is missing | `ProtocolVersion.hpp:7-10` |
| `IsModernProtocolVersion` | `constexpr bool IsModernProtocolVersion(std::string_view)` | — | Lexicographic comparison `>= kLatestProtocolVersion` | `ProtocolVersion.hpp:21-23` |
| `kJsonRpcVersion` / `kSdkVersion` | `inline constexpr std::string_view` | `"2.0"` / `"0.3.9"` | Envelope version and SDK version (the latter feeds the server discovery defaults) | `ProtocolVersion.hpp:11`; `include/mcp/McpVersion.hpp:7` |

## Serialisation and modelling conventions

| Convention | Form | Default | Notes | Evidence |
|---|---|---|---|---|
| Type conversion entry points | one pair of free functions per struct: `JsonValue SerializeX(const X&)` / `X DeserializeX(const JsonValue&)` | — | There is no member `to_json`/`from_json`; lookup happens via ADL on the free functions | `McpTypes.hpp:567-691`、`Content.hpp:93-129`、`Capabilities.hpp:74-95` |
| Global ETag switch | `void SetEtagCacheEnabled(bool)` / `bool IsEtagCacheEnabled()` | — | Process-wide; decides whether `CacheableResult` carries an `etag` | `McpTypes.hpp:263-264` |
| Optional fields | `std::optional<T>` | `std::nullopt` | "Not set" and "empty string / 0" are two different meanings | `Content.hpp:15-18`、`McpTypes.hpp:52-59` |
| JSON passed by value | `JsonValue` (never a reference) in parameters and return values | — | `JsonValue` holds a `variant` with deep-copy semantics, so by-value works well with moves | `JsonRpc.hpp:60-62`、`McpTypes.hpp:540` |
| Factories return ownership | `McpClient`/`McpServer::Create` return `std::unique_ptr`; `IClientTransport::Connect()` returns `std::shared_ptr<ITransport>` | — | See the [Client reference](/en/reference/client) and the [Transports reference](/en/reference/transports) | `Transport.hpp:72` |
| Rationale | one sentence: the modelling makes "unset" distinguishable by the type system and gives JSON values a clear lifetime, removing sentinel values and dangling references | — | The conventions live in the docs, not in code comments; intent is expressed through types | `JsonValue.hpp:25`、`CancelToken.hpp:26-28` |

## Source files behind this page

- `include/mcp/JsonValue.hpp`、`include/mcp/JsonRpc.hpp`、`include/mcp/Content.hpp`、`include/mcp/Meta.hpp`
- `include/mcp/Capabilities.hpp`、`include/mcp/Implementation.hpp`、`include/mcp/McpTypes.hpp`、`include/mcp/Methods.hpp`
- `include/mcp/Log.hpp`、`include/mcp/SpanHooks.hpp`、`include/mcp/CancelToken.hpp`
- `include/mcp/ErrorCodes.hpp`、`include/mcp/McpError.hpp`、`include/mcp/server/McpTaskStore.hpp` (`TaskState`/`TaskStatus`)
- `include/mcp/ProtocolVersion.hpp`、`include/mcp/McpVersion.hpp`
