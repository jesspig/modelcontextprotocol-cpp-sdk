# 类型层参考

本页是 core 类型层公共符号的**唯一权威字段表**，逐条对照头文件；门面签名见 [服务端参考](/zh/reference/server) 与 [客户端参考](/zh/reference/client)，错误码全表见 [错误码参考](/zh/reference/errors)，协议语义叙述见 [协议版本概念](/zh/learn/protocol-versions)。本页不含操作步骤。

## `JsonValue`（本仓库自研类型）

::: danger
`mcp::JsonValue` **不是 nlohmann::json**，也没有别名或兼容层。`parse()`/`dump()`/`contains()`/`value()`/`is_string()`/`push_back()`/`json::object()` 在本仓库一律不存在，照抄会编译失败；构造空对象/数组用 `JsonValue(JsonValue::object_tag)` / `JsonValue(JsonValue::array_tag)`，取值用大写的 `Parse`/`Dump`/`Contains`/`Find`/`At`/`PushBack`/`IsString`。
:::

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `JsonValue::Object` | `using Object = std::map<std::string, JsonValue, std::less<>>` | — | 透明比较，可用 `string_view` 查键 | `include/mcp/JsonValue.hpp:21` |
| `JsonValue::Array` | `using Array = std::vector<JsonValue>` | — | 数组容器 | `JsonValue.hpp:22` |
| 内部存储 | `std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object>` | `nullptr` | 七种备选，无 uint/float 分支 | `JsonValue.hpp:25` |
| 标量构造 | `JsonValue(bool/int/int64_t/double/std::string&&/const std::string&/const char*/const char* + size_t)` | — | `int` 一律升为 `int64_t` | `JsonValue.hpp:28-39` |
| 容器构造 | `JsonValue(Array v)` / `JsonValue(Object v)` / `JsonValue(ObjectTag)` / `JsonValue(ArrayTag)` | — | 两个 Tag 枚举即 `object_tag` / `array_tag` | `JsonValue.hpp:38-44` |
| `Parse` | `static JsonValue Parse(std::string_view json)` | — | 解析失败抛异常 | `JsonValue.hpp:52` |
| `SanitizeUtf8` | `static std::string SanitizeUtf8(std::string_view text)` | — | 清洗非法 UTF-8 序列 | `JsonValue.hpp:54` |
| `Dump` | `std::string Dump(int indent = -1) const` | `-1`（紧凑） | 序列化回字符串 | `JsonValue.hpp:56` |
| 类型判定 | `IsNull`/`IsBool`/`IsInt`/`IsDouble`/`IsNumber`/`IsString`/`IsArray`/`IsObject` | — | 全部返回 `bool`；`IsNumber` = `IsInt \|\| IsDouble` | `JsonValue.hpp:58-65` |
| 取值 | `GetBool`/`GetInt`/`GetDouble`/`GetString`/`GetArray`/`GetObject` | — | 类型不符抛异常；`GetString` 返 `const std::string&`，`GetArray`/`GetObject` 有可变重载 | `JsonValue.hpp:67-74` |
| 容器信息 | `size_t Size() const` / `bool Empty() const` / `bool Contains(std::string_view) const` | — | 对数组与对象均适用 | `JsonValue.hpp:76-78` |
| 下标 | `JsonValue& operator[](size_t)` / `JsonValue& operator[](std::string_view)` | — | `const` 与非 `const` 双重载 | `JsonValue.hpp:80-84` |
| `Find` | `const JsonValue* Find(std::string_view key) const` / 非 const 重载 | 未命中返回 `nullptr` | 可选字段首选，不抛异常 | `JsonValue.hpp:86-87` |
| `At` | `const JsonValue& At(std::string_view key) const` / 非 const 重载 | — | 缺失即抛 | `JsonValue.hpp:89-90` |
| `PushBack` | `void PushBack(JsonValue val)` | — | 按值入队 | `JsonValue.hpp:92` |
| 相等比较 | `bool operator==(const JsonValue&) const` / `operator!=` | — | 结构化深比较 | `JsonValue.hpp:94-95` |
| 范围 for | `begin()` / `end()` | — | **仅对象可迭代**（转发到 `GetObject()`），数组迭代须显式取 `GetArray()` | `JsonValue.hpp:97-100` |
| `GetObject` 宏保护 | `#pragma push_macro("GetObject")` / `pop_macro` | — | 规避 Windows 头同名宏，勿删 | `JsonValue.hpp:11-15`、`105-108` |

## JSON-RPC 信封

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `RequestId` | `using RequestId = std::variant<int64_t, std::string>` | — | 数字或字符串 id | `include/mcp/JsonRpc.hpp:15` |
| `ErrorData` | `struct { McpErrorCode code; std::string message; std::optional<JsonValue> data; }` | `code = InternalError` | 错误载荷载体 | `JsonRpc.hpp:17-21` |
| `JsonRpcRequest` | `struct`，含 `jsonrpc`/`id`/`method`/`params`/`meta`/`session_id` | `jsonrpc = "2.0"` | `params`、`meta` 为 `optional<JsonValue>`；`session_id` 为传输侧填充 | `JsonRpc.hpp:23-30` |
| `JsonRpcNotification` | `struct`，同上去 `id` | `jsonrpc = "2.0"` | 无 id | `JsonRpc.hpp:32-38` |
| `JsonRpcResponse` | `struct { std::string jsonrpc; RequestId id; JsonValue result; std::string session_id; }` | `"2.0"` | `result` 按值持有 | `JsonRpc.hpp:40-45` |
| `JsonRpcErrorResponse` | `struct { ...; std::optional<RequestId> id; ErrorData error; ... }` | `"2.0"` | id 可空（解析失败场景） | `JsonRpc.hpp:47-52` |
| `JsonRpcMessage` | `std::variant<JsonRpcRequest, JsonRpcNotification, JsonRpcResponse, JsonRpcErrorResponse>` | — | 传输层唯一消息类型 | `JsonRpc.hpp:54-58` |
| `SerializeMessage` | `std::string SerializeMessage(const JsonRpcMessage&)` / `(JsonRpcMessage&&)` | — | 移动重载避免深拷贝 | `JsonRpc.hpp:60-61` |
| `DeserializeMessage` | `JsonRpcMessage DeserializeMessage(std::string_view json)` | — | 失败抛 `McpError` | `JsonRpc.hpp:62` |
| `Is*` / `As*` | `IsRequest`/`IsNotification`/`IsResponse`/`IsError`、`AsRequest`/`AsNotification`/`AsResponse`/`AsError` | — | `As*` 返回 `std::get_if` 指针，不抛 | `JsonRpc.hpp:64-87` |
| `RequestIdToJson` / `RequestIdFromJson` | `JsonValue ...` / `RequestId ...` | — | id 与线格式互转 | `JsonRpc.hpp:89-90` |

## 内容变体

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `Icon` | `struct { std::string src; optional<string> mime_type; optional<vector<string>> sizes; optional<string> theme; }` | `src` 必填 | 图标条目 | `include/mcp/Content.hpp:13-18` |
| `Annotations` | `struct { optional<vector<string>> audience; optional<double> priority; optional<string> last_modified; }` | 全 `nullopt` | 内容标注 | `Content.hpp:20-24` |
| `TextResourceContents` | `struct { uri; text; optional<mime_type>; optional<JsonValue> meta; }` | — | 文本型资源体 | `Content.hpp:26-31` |
| `BlobResourceContents` | `struct { uri; blob; optional<mime_type>; optional<JsonValue> meta; }` | — | Base64 资源体 | `Content.hpp:33-38` |
| `ResourceContents` | `std::variant<TextResourceContents, BlobResourceContents>` | — | 资源内容二选一 | `Content.hpp:40` |
| `TextContent` | `struct { std::string type = "text"; std::string text; optional<string> mime_type; optional<Annotations> annotations; optional<JsonValue> meta; }` | `type = "text"` | 注意 `type` 有默认值，聚合初始化仍需显式给出 | `Content.hpp:42-48` |
| `ImageContent` / `AudioContent` | `struct { type("image"/"audio"); data; mime_type; optional<annotations>; optional<meta>; }` | `type` 内置 | `mime_type` **非** optional | `Content.hpp:50-64` |
| `EmbeddedResource` | `struct { type("resource"); ResourceContents resource; optional<annotations>; optional<meta>; }` | — | 内嵌资源 | `Content.hpp:66-71` |
| `ResourceLink` | `struct { type("resource_link"); uri; name; optional<title/description/mime_type/size>; vector<Icon> icons; optional<annotations>; optional<meta>; }` | — | 资源链接 | `Content.hpp:73-84` |
| `ContentVariant` | `std::variant<TextContent, ImageContent, AudioContent, EmbeddedResource, ResourceLink>` | — | `CallToolResult::content` 的元素类型 | `Content.hpp:86` |

## `_meta`、进度令牌与缓存提示

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `ProgressToken` | `using ProgressToken = std::variant<std::string, int64_t>` | — | `_meta.progressToken` | `include/mcp/Meta.hpp:15` |
| `LoggingLevel` | `enum class LoggingLevel { Debug, Info, Notice, Warning, Error, Critical, Alert, Emergency }` | — | **协议层**日志级别，与 SDK 内部 `LogLevel` 无关 | `Meta.hpp:17-19` |
| `RequestMeta.progress_token` | `std::optional<ProgressToken>` | `nullopt` | 进度关联令牌 | `Meta.hpp:22` |
| `RequestMeta.protocol_version` | `std::string` | `kLatestProtocolVersion`（`"2026-07-28"`） | 出向 `_meta` 版本戳 | `Meta.hpp:23` |
| `RequestMeta.client_info` / `client_capabilities` | `optional<Implementation>` / `optional<ClientCapabilities>` | `nullopt` | 仅 `initialize` 期填充 | `Meta.hpp:24-25` |
| `RequestMeta.log_level` | `std::optional<LoggingLevel>` | `nullopt` | 客户端要求的日志级别 | `Meta.hpp:26` |
| `RequestMeta.extensions` | `std::optional<JsonValue>` | `nullopt` | `_meta` 扩展槽 | `Meta.hpp:27` |
| `RequestMeta.traceparent` / `tracestate` / `baggage` | `std::optional<std::string>` ×3 | `nullopt` | W3C 追踪上下文，喂给 span 钩子 | `Meta.hpp:28-30` |
| `CacheHint.ttl_ms` / `cache_scope` | `std::optional<int64_t>` / `std::optional<std::string>` | `nullopt` | 列表类结果的缓存提示 | `Meta.hpp:33-36` |

## 能力声明

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `ToolsCapability` / `PromptsCapability` / `RootsCapability` | `struct { std::optional<bool> list_changed; }` | `nullopt` | 三者结构相同 | `include/mcp/Capabilities.hpp:12-14`、`21-23`、`27-29` |
| `ResourcesCapability` | `struct { optional<bool> subscribe; optional<bool> list_changed; }` | `nullopt` | 唯一带 `subscribe` | `Capabilities.hpp:16-19` |
| `SamplingCapability` / `EmptyCapability` | `struct {}` | — | 空标记；`LoggingCapability`、`TasksCapability`、`SubscriptionsCapability`、`CompletionsCapability` 均为 `EmptyCapability` 别名 | `Capabilities.hpp:25`、`36`、`38-41` |
| `ElicitationCapability` | `struct { std::optional<JsonValue> form; std::optional<JsonValue> url; }` | `nullopt` | 声明支持的 elicitation 形态 | `Capabilities.hpp:31-34` |
| `kTasksExtensionId` / `kSkillsExtensionId` | `inline constexpr std::string_view` | `"io.modelcontextprotocol/tasks"` / `".../skills"` | 扩展声明键 | `Capabilities.hpp:43-44` |
| `TasksExtensionSettings` / `SkillsExtensionSettings` | `struct {}` / `struct { bool directory_read{false}; }` | `false` | 扩展设置体 | `Capabilities.hpp:46-50` |
| `ServerCapabilities` | `struct`：`tools`/`resources`/`prompts`/`logging`/`sampling`/`roots`/`elicitation`/`completions`/`subscriptions` 均 `optional<>`，加 `optional<map<string, JsonValue>> extensions`、`optional<JsonValue> experimental` | 全 `nullopt` | 共 11 个字段 | `Capabilities.hpp:52-64` |
| `ClientCapabilities` | `struct`：`roots`/`sampling`/`elicitation`/`extensions`/`experimental` | 全 `nullopt` | 共 5 个字段 | `Capabilities.hpp:66-72` |

## 实现信息

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `Implementation` | `struct { std::string name; std::string version; optional<string> title; vector<Icon> icons; optional<string> description; optional<string> website_url; }` | `name`/`version` 必填，`icons = {}` | 同时用于 `client_info` 与 `server_info` | `include/mcp/Implementation.hpp:11-18` |

## 声明类型：`Tool` 与 `ToolOptions`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `Tool::name` | `std::string` | 空（必填） | 注册名 | `include/mcp/McpTypes.hpp:51` |
| `Tool::title` / `description` | `std::optional<std::string>` | `nullopt` | 人类可读信息 | `McpTypes.hpp:52-53` |
| `Tool::input_schema` | `JsonValue`（**非** optional） | 默认构造 = `null` | 线格式必填，注册期由 `ToolOptions` 补默认 schema | `McpTypes.hpp:54` |
| `Tool::output_schema` | `std::optional<JsonValue>` | `nullopt` | 结构化输出模式 | `McpTypes.hpp:55` |
| `Tool::annotations` | `std::optional<ToolAnnotations>` | `nullopt` | 行为注解 | `McpTypes.hpp:56` |
| `Tool::icons` | `std::vector<Icon>` | `{}` | 图标集 | `McpTypes.hpp:57` |
| `Tool::meta` | `std::optional<JsonValue>` | `nullopt` | `_meta` 透传 | `McpTypes.hpp:58` |
| `Tool::execution` | `std::optional<ToolExecution>` | `nullopt` | 任务/人审执行模式 | `McpTypes.hpp:59` |
| `ToolOptions` | `struct`，13 个字段 + 3 个 fluent setter | 见下 | 注册入参，与 `Tool` 字段并不一一对应 | `McpTypes.hpp:523-541` |
| `ToolOptions::name` | `std::optional<std::string>` | `nullopt` | 未被消费：实际名取 `RegisterTool`/`Create` 的 `name` 参数 | `McpTypes.hpp:524` |
| `ToolOptions::{description,title,destructive,idempotent,read_only_hint,open_world_hint}` | `std::optional<std::string>` ×2 + `std::optional<bool>` ×4 | `nullopt` | 后四个 bool 组装进 `ToolAnnotations` | `McpTypes.hpp:525-530` |
| `ToolOptions::use_structured_content` | `bool` | `false` | 置真时把 `output_schema` 设为 `{"type":"object"}` | `McpTypes.hpp:531` |
| `ToolOptions::{input_schema,output_schema,icons,meta,execution}` | `optional<JsonValue>` ×3（含 `meta`）、`vector<Icon>`、`optional<ToolExecution>` | `nullopt` / `{}` | **只能直接赋值** | `McpTypes.hpp:532-536` |
| `ToolOptions::Description` / `Title` / `InputSchema` | `ToolOptions& Description(std::string_view)`、`ToolOptions& Title(std::string_view)`、`ToolOptions& InputSchema(JsonValue)` | — | **仅此三个** fluent setter，可链式；其余字段一律成员赋值 | `McpTypes.hpp:538-540` |

::: warning
`ToolOptions` 没有 `Name()`、`OutputSchema()`、`Icons()`、`Meta()`、`Execution()`、`Destructive()` 等 setter，写 `ToolOptions{}.OutputSchema(x)` 会编译失败。
:::

## 其余声明类型

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `Resource` | `struct { uri; name; optional<title/description/mime_type/size>; optional<vector<Icon>> icons; optional<ResourceAnnotations> annotations; optional<JsonValue> meta; }` | 必填 `uri`/`name` | 静态资源描述 | `McpTypes.hpp:62-72` |
| `ResourceTemplate` | `struct`，首字段为 `uri_template`，其余同 `Resource`（无 `size`） | 必填 `uri_template`/`name` | 模板资源描述 | `McpTypes.hpp:74-83` |
| `ResourceAnnotations` | `struct { optional<vector<string>> audience; optional<double> priority; optional<string> last_modified; }` | 全 `nullopt` | 与 `Annotations` 同形但独立类型 | `McpTypes.hpp:44-48` |
| `ToolAnnotations` | `struct { optional<string> title; optional<bool> read_only_hint/idempotent_hint/open_world_hint/destructive_hint; }` | 全 `nullopt` | 由 `ToolOptions` 的四个 bool 装配 | `McpTypes.hpp:28-34` |
| `ToolExecutionMode` / `TaskSupport` | `enum class { Auto, Manual, Task }` / `enum class { Optional, Required }` | `Auto` / 无默认 | 执行模式与任务支持强度 | `McpTypes.hpp:36-37` |
| `ToolExecution` | `struct { ToolExecutionMode mode{Auto}; optional<string> human_use; optional<TaskSupport> task_support; }` | `mode = Auto` | 工具执行声明 | `McpTypes.hpp:38-42` |
| `Prompt` | `struct { name; optional<title/description>; optional<vector<PromptArgument>> arguments; optional<vector<Icon>> icons; optional<JsonValue> meta; }` | 必填 `name` | 提示词描述 | `McpTypes.hpp:91-98` |
| `PromptArgument` | `struct { name; optional<description>; optional<bool> required; }` | 必填 `name` | 参数声明 | `McpTypes.hpp:85-89` |
| `PromptMessage` | `struct { std::string role; ContentVariant content; }` | — | `prompts/get` 返回条目 | `McpTypes.hpp:100-103` |
| `ResourceOptions` | `struct { optional<name/description/title/mime_type>; vector<Icon> icons; }` + `Description`/`Title`/`MimeType` | `nullopt` / `{}` | 3 个 setter，其余成员赋值；`name` 未被消费，实际名取 `RegisterResource` 的 `name` 实参（`McpServer.cpp:795`） | `McpTypes.hpp:543-553` |
| `PromptOptions` | `struct { optional<name/description/title>; vector<Icon> icons; optional<vector<PromptArgument>> arguments; }` + `Description`/`Title`/`Arguments` | `nullopt` / `{}` | 3 个 setter，其余成员赋值；`name` 未被消费，实际名取 `RegisterPrompt` 的 `name` 实参（`McpServer.cpp:885`） | `McpTypes.hpp:555-565` |
| `SkillResource` | `struct { std::string uri; std::string digest; int64_t size{0}; }` | `size = 0` | 技能内文件摘要 | `McpTypes.hpp:481-485` |
| `SkillResources` | `std::variant<std::vector<SkillResource>, std::string>` | — | 列表或目录引用二选一 | `McpTypes.hpp:487` |
| `Skill` | `struct { std::string uri; JsonValue frontmatter; SkillResources resources; optional<JsonValue> meta; }` | 必填 `uri`/`frontmatter`/`resources` | 技能声明（`skills/*`） | `McpTypes.hpp:489-494` |

## 请求参数类型

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `PaginatedRequestParams` | `struct { optional<string> cursor; optional<RequestMeta> meta; }` | `nullopt` | 别名：`ListToolsRequestParams`、`ListResourcesRequestParams`、`ListResourceTemplatesRequestParams`、`ListPromptsRequestParams`、`ListSkillsRequestParams` | `McpTypes.hpp:119-126`、`468` |
| `ResourceRequestParams` | `struct { std::string uri; optional<RequestMeta> meta; optional<JsonValue> input_responses; optional<string> request_state; }` | 必填 `uri` | 别名：`ReadResourceRequestParams`、`SubscribeRequestParams`、`UnsubscribeRequestParams` | `McpTypes.hpp:128-136` |
| `CallToolRequestParams` | `struct { name; optional<JsonValue> arguments; optional<RequestMeta> meta; optional<JsonValue> input_responses; optional<string> request_state; }` | 必填 `name` | MRTR 续跑靠后两字段 | `McpTypes.hpp:138-144` |
| `GetPromptRequestParams` | `struct { name; optional<JsonValue> arguments; optional<RequestMeta> meta; optional<JsonValue> input_responses; optional<string> request_state; }` | 必填 `name` | 与 `CallTool` 同构 | `McpTypes.hpp:191-197` |
| `CompleteRequestParams` | `struct { JsonValue ref; std::string argument_name; std::string argument_value; map<string,string,less<>> context_arguments; optional<RequestMeta> meta; }` | `context_arguments = {}` | `ref` 为原始对象（`{"type":"ref/prompt","name":...}`），非强类型 | `McpTypes.hpp:199-205` |
| `InitializeRequestParams` | `struct { std::string protocol_version; ClientCapabilities capabilities; Implementation client_info; }` | 无 optional | 三字段全必填 | `McpTypes.hpp:209-213` |
| `DiscoverRequestParams` / `ListRootsRequestParams` | `struct {}` | — | 空参数请求 | `McpTypes.hpp:207`、`243` |
| `Pagination` | `struct { std::optional<std::string> next_cursor; }` | `nullopt` | 游标信封 | `McpTypes.hpp:105-107` |
| `FileRef` | `struct { std::string uri; int64_t offset{0}; int64_t length{0}; }` | `0` | `x-mcp-fileref` 参数；序列化时 `0` 值字段被省略 | `McpTypes.hpp:146-158` |
| `SubscriptionFilter` | `struct { optional<bool> tools_list_changed/prompts_list_changed/resources_list_changed; vector<string> resource_subscriptions; }` | `nullopt` / `{}` | `subscriptions/listen` 过滤器 | `McpTypes.hpp:215-220` |
| `SubscriptionsListenRequestParams` | `struct { SubscriptionFilter notifications; optional<RequestMeta> meta; }` | — | 监听订阅入参 | `McpTypes.hpp:222-225` |
| `CreateMessageRequestParams` / `SamplingMessage` | `struct { vector<SamplingMessage> messages; int64_t max_tokens; optional<string> stop_reason/model_preference; }` / `struct { role; ContentVariant content; }` | `max_tokens` 必填 | `sampling/*` 已废弃（SEP-2577），类型仍保留 | `McpTypes.hpp:231-241` |
| `SetLevelRequestParams` | `struct { LoggingLevel level; }` | — | `logging/setLevel` 入参 | `McpTypes.hpp:413-415` |
| `LoggingMessageNotificationParams` | `struct { LoggingLevel level; optional<string> logger; JsonValue data; }` | `logger = nullopt` | `notifications/message` | `McpTypes.hpp:344-348` |
| `ProgressNotificationParams` | `struct { ProgressToken progress_token; double progress; optional<double> total; optional<string> message; }` | — | `on_progress` 回调入参 | `McpTypes.hpp:332-337` |
| `CancelledNotificationParams` | `struct { RequestId request_id; optional<string> reason; }` | — | 取消通知 | `McpTypes.hpp:339-342` |

## 结果类型

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `Result` | `struct { std::optional<JsonValue> meta; ResultType result_type{Complete}; }` | `Complete` | 所有结果基类；`ResultType = { Complete, InputRequired, Task }` | `McpTypes.hpp:109`、`114-117` |
| `EmptyResult` / `PingResult` | `struct EmptyResult : Result {}`；`using PingResult = EmptyResult` | — | 空结果 | `McpTypes.hpp:257`、`330` |
| `CacheableResult` | `struct CacheableResult : Result { std::optional<std::string> etag; }` | `nullopt` | ETag 由全局开关控制 | `McpTypes.hpp:259-266` |
| `CallToolResult` | `struct : Result { vector<ContentVariant> content; optional<JsonValue> structured_content; bool is_error{false}; optional<InputRequiredResult> input_required; }` | `is_error = false` | 共 4 个自有字段 + 基类 2 个 | `McpTypes.hpp:268-273` |
| `ListToolsResult` / `ListResourcesResult` / `ListResourceTemplatesResult` / `ListPromptsResult` | `struct : Result { vector<…>; optional<string> next_cursor; optional<CacheHint> cache_hint; }` | `nullopt` | 四个分页列表结果同构 | `McpTypes.hpp:275-303` |
| `ReadResourceResult` | `struct : Result { vector<ResourceContents> contents; optional<CacheHint> cache_hint; optional<InputRequiredResult> input_required; }` | — | 读取资源 | `McpTypes.hpp:293-297` |
| `GetPromptResult` | `struct : Result { vector<PromptMessage> messages; optional<string> description; optional<InputRequiredResult> input_required; }` | — | 获取提示词 | `McpTypes.hpp:305-309` |
| `CompleteResult` | `struct : Result { JsonValue completion; }` | 默认构造 `null` | 补全结果；填充示例 `completion/complete` 的 `{"values":[…]}` | `McpTypes.hpp:311-313` |
| `InitializeResult` | `struct : Result { std::string protocol_version; ServerCapabilities capabilities; Implementation server_info; optional<string> instructions; }` | 前三必填 | 握手结果 | `McpTypes.hpp:315-320` |
| `DiscoverResult` | `struct : Result { vector<string> supported_versions; ServerCapabilities capabilities; Implementation server_info; optional<string> instructions; optional<CacheHint> cache_hint; }` | — | `server/discover` 结果 | `McpTypes.hpp:322-328` |
| `SkillsListResult` / `SkillsGetResult` / `ReadDirectoryResult` / `DirectoryChild` | `struct : Result { …; optional<int64_t> ttl_ms; optional<string> cache_scope; }` 等 | `nullopt` | 技能与目录浏览结果，缓存字段用 `ttl_ms`/`cache_scope` 而非 `CacheHint` | `McpTypes.hpp:496-521` |
| `CreateMessageResult` / `ListRootsResult` / `Root` | `struct : Result { role; ContentVariant content; string model; optional<stop_reason>; }` / `{ vector<Root> roots; }` / `{ uri; optional<name>; }` | — | 已废弃能力的类型 | `McpTypes.hpp:378-392` |

## MRTR 输入请求

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `InputRequest` | `struct { std::string method; JsonValue params; }` | 必填 | 单条待补输入 | `McpTypes.hpp:245-248` |
| `InputRequests` | `using InputRequests = std::map<std::string, InputRequest, std::less<>>` | — | 按 id 索引，透明查键 | `McpTypes.hpp:250` |
| `InputRequiredResult` | `struct { InputRequests input_requests; std::optional<std::string> request_state; }` | `nullopt` | 结果侧的续跑载荷 | `McpTypes.hpp:252-255` |
| `MakeInputRequestForElicitation` / `...ForSampling` / `...ForRoots` | `InputRequest Make...(const *RequestParams&)` | — | 三个构造器 | `McpTypes.hpp:371-373` |
| `MakeInputResponseFromElicitResult` / `...FromCreateMessageResult` / `...FromListRootsResult` | `JsonValue Make...(const ...Result&)` | — | 把用户应答包成 `input_responses` 条目 | `McpTypes.hpp:374`、`394-395` |
| `IsInputRequiredResult` / `ExtractInputRequests` | `bool (const JsonValue&)` / `std::optional<InputRequests> (const JsonValue&)` | — | 从原始结果 JSON 识别 input_required | `McpTypes.hpp:375-376` |
| `ElicitRequestParams` | `struct { std::string message; optional<JsonValue> requested_schema; std::string mode{"form"}; optional<string> url; optional<string> elicitation_id; }` | `mode = "form"` | 服务端发起 elicitation 的入参 | `McpTypes.hpp:350-356` |
| `ElicitResult` | `struct : Result { std::string action; optional<JsonValue> content; }` | `action` 无默认 | 线格式结果（`accept`/`decline`/`cancel`） | `McpTypes.hpp:358-361` |
| `ElicitResultTyped<T>` | `template <typename T> struct { std::string action = "cancel"; optional<T> content; bool is_accepted() const; }` | `action = "cancel"` | 强类型应答视图；`is_accepted()` 是**该模板唯一的 snake_case 成员**，勿改名 | `McpTypes.hpp:363-369` |

## 任务相关类型

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `TaskStatus` | `enum class { Pending, Working, Completed, Failed, Cancelled, InputRequired }` | `Pending` | 任务状态机 | `include/mcp/server/McpTaskStore.hpp:16-23` |
| `TaskState` | `struct`（17 个字段：`task_id`/`owner`/`status`/`result`/`error_message`/`input_required`/`progress`/`progress_total`/`created_at`/`status_message`/`last_updated_at`/`ttl_ms`/`poll_interval_ms`/`input_requests`/`input_responses`/`tool_name`/`tool_arguments`） | `status = Pending`、`progress = 0` | 定义在**服务端**头，非 `McpTypes.hpp`；存储侧细节见 [进程与存储参考](/zh/reference/process-and-storage) | `McpTaskStore.hpp:25-43` |
| `CreateTaskResult` | `struct : Result { task_id; status; created_at; optional<status_message/last_updated_at/ttl_ms/poll_interval_ms/input_requests>; }` | 前三必填 | `tasks/*` 创建结果 | `McpTypes.hpp:434-443` |
| `GetTaskResult` | `struct`（**不继承** `Result`，14 个字段，含 `task_result_type{"task"}`、`error`、`error_message`、`input_required`、`meta`） | `task_result_type = "task"` | 与 `CreateTaskResult` 不同基类，勿混淆 | `McpTypes.hpp:417-432` |
| `UpdateTaskResult` / `CancelTaskResult` | `using ... = EmptyResult` | — | 无自有字段 | `McpTypes.hpp:445-446` |
| `GetTaskRequestParams` / `UpdateTaskRequestParams` / `CancelTaskRequestParams` / `TaskStatusNotificationParams` | `{ task_id }` / `{ task_id, optional<result>, optional<input_responses> }` / `{ task_id, optional<reason> }` / `{ task_id, status }` | 按值 | 任务方法与通知入参 | `McpTypes.hpp:448-466` |
| `SkillsGetRequestParams` / `ReadDirectoryRequestParams` | `{ uri, optional<RequestMeta> meta }` / `{ uri, optional<string> cursor, optional<RequestMeta> meta }` | 必填 `uri` | 技能读取与目录浏览入参 | `McpTypes.hpp:470-479` |

## 请求选项

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `RequestOptions::meta` | `std::optional<JsonValue>` | `nullopt` | 出向 `_meta` | `McpTypes.hpp:398` |
| `RequestOptions::read_timeout_ms` | `std::optional<int64_t>` | `nullopt` | 读超时（区别于总超时） | `McpTypes.hpp:399` |
| `RequestOptions::input_responses` | `std::optional<JsonValue>` | `nullopt` | MRTR 续跑载荷 | `McpTypes.hpp:400` |
| `RequestOptions::request_state` | `std::optional<std::string>` | `nullopt` | 与 `input_responses` 配对 | `McpTypes.hpp:401` |
| `RequestOptions::on_progress` | `std::function<void(const ProgressNotificationParams&)>` | 未设置 | 等价于 `mcp::ProgressCallback`（`include/mcp/client/McpClient.hpp:27`） | `McpTypes.hpp:402` |
| `RequestOptions::timeout` | `std::chrono::milliseconds` | `kDefaultRequestTimeout` = `60000` | 总超时 | `McpTypes.hpp:403`、`26` |
| `RequestOptions::cancel_on_timeout` | `bool` | `true` | 超时即发取消 | `McpTypes.hpp:404` |
| `RequestOptions::cancel_token` | `CancelToken` | 默认构造（未取消） | 成员是值类型，非指针 | `McpTypes.hpp:405` |
| `CacheableRequestOptions` | `struct : RequestOptions { optional<string> cache_mode; optional<int64_t> max_age_ms; }` | `nullopt` | 读类请求的缓存重载入口 | `McpTypes.hpp:408-411` |

::: info
`RequestOptions` 共 **8** 个字段（`McpTypes.hpp:397-406`），不是迁移文档里那 4 个字段的形状。
:::

## 取消与错误

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `CancelToken` | `class MCP_API CancelToken`（值语义，内部 `shared_ptr<State>`） | 未取消 | 可拷贝、跨线程共享同一状态 | `include/mcp/CancelToken.hpp:13`、`26-28` |
| `Cancel` | `void Cancel(std::string_view reason = {})` | 空原因 | 置位并广播处理器 | `CancelToken.hpp:19` |
| `IsCancelled` / `Reason` | `bool () const` / `std::string () const` | `false` / `""` | 状态查询 | `CancelToken.hpp:20-21` |
| `AddCancelHandler` / `RemoveCancelHandler` | `std::size_t AddCancelHandler(CancelHandler)` / `void RemoveCancelHandler(std::size_t)` | — | 返回句柄 id 供移除；`CancelHandler = std::function<void(std::string_view)>` | `CancelToken.hpp:15`、`23-24` |
| `McpErrorCode` | `enum class McpErrorCode : int32_t` | — | JSON-RPC 标准码 + SDK 扩展码；全表见 [错误码参考](/zh/reference/errors) | `include/mcp/ErrorCodes.hpp:8-30` |
| `make_error_code` / `McpErrorCategory` / `GetMcpErrorCategory` | `std::error_code` / `class std::error_category` / `const McpErrorCategory&` | — | `is_error_code_enum` 已特化，可直接隐式转 `std::error_code` | `ErrorCodes.hpp:32`、`36-39`、`43-92`、`94-101` |
| `McpError` | `class McpError : public std::runtime_error` | — | 唯一业务异常类型 | `include/mcp/McpError.hpp:13` |
| `McpError` 构造 | `McpError(McpErrorCode, std::string message)` / `McpError(McpErrorCode, std::string, std::optional<JsonValue> data)` | — | 第二重载携带 `data` | `McpError.hpp:15-24` |
| `Code` / `ErrorMessage` / `Data` | `McpErrorCode Code() const noexcept` / `std::string_view ErrorMessage() const noexcept` / `const std::optional<JsonValue>& Data() const noexcept` | — | 三个访问器均 `noexcept` | `McpError.hpp:26-28` |

## 日志与观测

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `LogLevel` | `enum class LogLevel : int { Off=0, Error=1, Warning=2, Info=3, Debug=4, Trace=5 }` | `Off` | SDK 内部日志级别；与协议层 `LoggingLevel` 是两套 | `include/mcp/Log.hpp:15-22`、`48` |
| `MCP_LOG_LEVEL` | 环境变量 `0`–`5` | 未设置 = 全关 | 首次访问日志状态时读取；`≥5` 归 `Trace` | `Log.hpp:55-60` |
| `LogContext` | `struct { std::string request_id; std::string method; std::string session_id; std::string peer; }` | 全空 | `MCP_LOG_CTX` 携带 | `Log.hpp:24-29` |
| `LogRecord` | `struct { LogLevel level; string_view tag; string_view message; const char* file; int line; const LogContext* context; }` | — | handler 入参；`tag`/`message` 仅在回调期内有效 | `Log.hpp:31-38` |
| `LogHandler` | `using LogHandler = std::function<void(const LogRecord&)>` | — | handler 存在时不再写 `stderr` | `Log.hpp:40`、`108-122` |
| `SetLogLevel` / `GetLogLevel` | `void SetLogLevel(LogLevel)` / `LogLevel GetLogLevel()` | `Off` | 运行时覆盖环境变量 | `Log.hpp:82`、`99` |
| `SetLogHandler` / `GetLogHandler` | `void SetLogHandler(LogHandler)` / `LogHandler GetLogHandler()` | 未设置 | 传 `nullptr` 恢复默认 `stderr` 输出 | `Log.hpp:86`、`93` |
| `MCP_LOG` / `MCP_LOG_CTX` | 宏 | — | SDK 内部使用；级别名须与 `LogLevel` 成员一致 | `Log.hpp:172`、`179` |
| `SpanKind` | `enum class SpanKind : std::uint8_t { ServerRequest, TransportReceive, TransportSend }` | `ServerRequest` | span 类别 | `include/mcp/SpanHooks.hpp:9-13`、`22` |
| `SpanPhase` | `enum class SpanPhase : std::uint8_t { Begin, End }` | `Begin` | 成对事件的两个阶段 | `SpanHooks.hpp:15-18`、`21` |
| `SpanEvent` | `struct { SpanPhase phase; SpanKind kind; string name/method/request_id/traceparent/tracestate/baggage; bool failed; }` | `failed = false` | 追踪上下文来自 `RequestMeta` | `SpanHooks.hpp:20-30` |
| `SpanHandler` | `using SpanHandler = std::function<void(const SpanEvent&)>` | — | 注入口在服务端/客户端选项，见对应参考页 | `SpanHooks.hpp:32` |

## 方法名与版本常量

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `methods::k*` | `inline constexpr std::string_view` | — | 请求方法名（`initialize`、`tools/call`、`server/discover`、`subscriptions/listen`、`tasks/*`、`skills/*`、`logging/setLevel` 等）；禁止用字面量拼方法名 | `include/mcp/Methods.hpp:8-52` |
| `notifications::k*` | `inline constexpr std::string_view` | — | 通知方法名（含 `notifications/tasks/*`、`notifications/elicitation/complete`） | `Methods.hpp:56-77` |
| `kProtocolVersions` | `inline constexpr std::string_view[]` | 5 项 | `2024-11-05` … `2026-07-28` | `include/mcp/ProtocolVersion.hpp:13-19` |
| `kLatestProtocolVersion` / `kModernProtocolVersion` / `kLegacyProtocolVersion` / `kDefaultNegotiatedProtocolVersion` | `inline constexpr std::string_view` | `"2026-07-28"` / `"2026-07-28"` / `"2025-11-25"` / `"2025-03-26"` | 双 era 判定基准；缺失版本声明时的默认协商值 | `ProtocolVersion.hpp:7-10` |
| `IsModernProtocolVersion` | `constexpr bool IsModernProtocolVersion(std::string_view)` | — | 字典序比较 `>= kLatestProtocolVersion` | `ProtocolVersion.hpp:21-23` |
| `kJsonRpcVersion` / `kSdkVersion` | `inline constexpr std::string_view` | `"2.0"` / `"0.3.9"` | 信封版本与 SDK 版本（后者是服务端发现信息默认值来源） | `ProtocolVersion.hpp:11`；`include/mcp/McpVersion.hpp:7` |

## 序列化约定与表达惯例

| 惯例 | 形式 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| 类型转换入口 | 每个结构体配一对自由函数 `JsonValue SerializeX(const X&)` / `X DeserializeX(const JsonValue&)` | — | 没有成员 `to_json`/`from_json`，ADL 查找自由函数 | `McpTypes.hpp:567-691`、`Content.hpp:93-129`、`Capabilities.hpp:74-95` |
| ETag 全局开关 | `void SetEtagCacheEnabled(bool)` / `bool IsEtagCacheEnabled()` | — | 进程级，影响 `CacheableResult` 是否带 `etag` | `McpTypes.hpp:263-264` |
| 可选字段 | `std::optional<T>` | `std::nullopt` | 缺失与「空字符串/0」是两种语义 | `Content.hpp:15-18`、`McpTypes.hpp:52-59` |
| JSON 按值传递 | `JsonValue`（非引用）入参与返回 | — | `JsonValue` 内部 `variant` + 深拷贝语义，按值便于转移 | `JsonRpc.hpp:60-62`、`McpTypes.hpp:540` |
| 工厂返回所有权 | `McpClient`/`McpServer::Create` 返回 `std::unique_ptr`；`IClientTransport::Connect()` 返回 `std::shared_ptr<ITransport>` | — | 见 [客户端参考](/zh/reference/client)、[传输层参考](/zh/reference/transports) | `Transport.hpp:72` |
| 设计动机 | 一句话：让「未设置」可被类型系统区分、让 JSON 拥有明确生命周期，从而免除哨兵值与悬垂引用 | — | 惯例本身不进代码注释，意图靠类型表达 | `JsonValue.hpp:25`、`CancelToken.hpp:26-28` |

相关任务写法：[/zh/howto/validate-tool-input](/zh/howto/validate-tool-input)、[/zh/howto/report-progress](/zh/howto/report-progress)。

## 本页对应源码

- `include/mcp/JsonValue.hpp`、`include/mcp/JsonRpc.hpp`、`include/mcp/Content.hpp`、`include/mcp/Meta.hpp`
- `include/mcp/Capabilities.hpp`、`include/mcp/Implementation.hpp`、`include/mcp/McpTypes.hpp`、`include/mcp/Methods.hpp`
- `include/mcp/Log.hpp`、`include/mcp/SpanHooks.hpp`、`include/mcp/CancelToken.hpp`
- `include/mcp/ErrorCodes.hpp`、`include/mcp/McpError.hpp`、`include/mcp/server/McpTaskStore.hpp`（`TaskState`/`TaskStatus`）
- `include/mcp/Methods.hpp`、`include/mcp/ProtocolVersion.hpp`、`include/mcp/McpVersion.hpp`
