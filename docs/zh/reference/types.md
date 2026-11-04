# 类型层参考

本页是 core 类型层公共符号的**唯一权威字段表**，逐条对照头文件；门面签名见 [服务端参考](/zh/reference/server) 与 [客户端参考](/zh/reference/client)，错误码全表见 [错误码参考](/zh/reference/errors)，协议语义叙述见 [协议版本概念](/zh/learn/protocol-versions)。本页不含操作步骤。

## `JsonValue`（本仓库自研类型）

::: danger
`mcp::JsonValue` **不是 nlohmann::json**，也没有别名或兼容层。`parse()`/`dump()`/`contains()`/`value()`/`is_string()`/`push_back()`/`json::object()` 在本仓库一律不存在，照抄会编译失败；构造空对象/数组用 `JsonValue(JsonValue::object_tag)` / `JsonValue(JsonValue::array_tag)`，取值用大写的 `Parse`/`Dump`/`Contains`/`Find`/`At`/`PushBack`/`IsString`。
:::

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `JsonValue::Object` | `using Object = std::map<std::string, JsonValue, std::less<>>` | — | 透明比较，可用 `string_view` 查键 |
| `JsonValue::Array` | `using Array = std::vector<JsonValue>` | — | 数组容器 |
| 内部存储 | `std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object>` | `nullptr` | 七种备选，无 uint/float 分支 |
| 标量构造 | `JsonValue(bool/int/int64_t/double/std::string&&/const std::string&/const char*/const char* + size_t)` | — | `int` 一律升为 `int64_t` |
| 容器构造 | `JsonValue(Array v)` / `JsonValue(Object v)` / `JsonValue(ObjectTag)` / `JsonValue(ArrayTag)` | — | 两个 Tag 枚举即 `object_tag` / `array_tag` |
| `Parse` | `static JsonValue Parse(std::string_view json)` | — | 解析失败抛异常 |
| `SanitizeUtf8` | `static std::string SanitizeUtf8(std::string_view text)` | — | 清洗非法 UTF-8 序列 |
| `Dump` | `std::string Dump(int indent = -1) const` | `-1`（紧凑） | 序列化回字符串 |
| 类型判定 | `IsNull`/`IsBool`/`IsInt`/`IsDouble`/`IsNumber`/`IsString`/`IsArray`/`IsObject` | — | 全部返回 `bool`；`IsNumber` = `IsInt \|\| IsDouble` |
| 取值 | `GetBool`/`GetInt`/`GetDouble`/`GetString`/`GetArray`/`GetObject` | — | 类型不符抛异常；`GetString` 返 `const std::string&`，`GetArray`/`GetObject` 有可变重载 |
| 容器信息 | `size_t Size() const` / `bool Empty() const` / `bool Contains(std::string_view) const` | — | 对数组与对象均适用 |
| 下标 | `JsonValue& operator[](size_t)` / `JsonValue& operator[](std::string_view)` | — | `const` 与非 `const` 双重载 |
| `Find` | `const JsonValue* Find(std::string_view key) const` / 非 const 重载 | 未命中返回 `nullptr` | 可选字段首选，不抛异常 |
| `At` | `const JsonValue& At(std::string_view key) const` / 非 const 重载 | — | 缺失即抛 |
| `PushBack` | `void PushBack(JsonValue val)` | — | 按值入队 |
| 相等比较 | `bool operator==(const JsonValue&) const` / `operator!=` | — | 结构化深比较 |
| 范围 for | `begin()` / `end()` | — | **仅对象可迭代**（转发到 `GetObject()`），数组迭代须显式取 `GetArray()` |
| `GetObject` 宏保护 | `#pragma push_macro("GetObject")` / `pop_macro` | — | 规避 Windows 头同名宏，勿删 |

全部符号声明在 `include/mcp/JsonValue.hpp`。

## JSON-RPC 信封

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `RequestId` | `using RequestId = std::variant<int64_t, std::string>` | — | 数字或字符串 id |
| `ErrorData` | `struct { McpErrorCode code; std::string message; std::optional<JsonValue> data; }` | `code = InternalError` | 错误载荷载体 |
| `JsonRpcRequest` | `struct`，含 `jsonrpc`/`id`/`method`/`params`/`meta`/`session_id` | `jsonrpc = "2.0"` | `params`、`meta` 为 `optional<JsonValue>`；`session_id` 为传输侧填充 |
| `JsonRpcNotification` | `struct`，同上去 `id` | `jsonrpc = "2.0"` | 无 id |
| `JsonRpcResponse` | `struct { std::string jsonrpc; RequestId id; JsonValue result; std::string session_id; }` | `"2.0"` | `result` 按值持有 |
| `JsonRpcErrorResponse` | `struct { ...; std::optional<RequestId> id; ErrorData error; ... }` | `"2.0"` | id 可空（解析失败场景） |
| `JsonRpcMessage` | `std::variant<JsonRpcRequest, JsonRpcNotification, JsonRpcResponse, JsonRpcErrorResponse>` | — | 传输层唯一消息类型 |
| `SerializeMessage` | `std::string SerializeMessage(const JsonRpcMessage&)` / `(JsonRpcMessage&&)` | — | 移动重载避免深拷贝 |
| `DeserializeMessage` | `JsonRpcMessage DeserializeMessage(std::string_view json)` | — | 失败抛 `McpError` |
| `Is*` / `As*` | `IsRequest`/`IsNotification`/`IsResponse`/`IsError`、`AsRequest`/`AsNotification`/`AsResponse`/`AsError` | — | `As*` 返回 `std::get_if` 指针，不抛 |
| `RequestIdToJson` / `RequestIdFromJson` | `JsonValue ...` / `RequestId ...` | — | id 与线格式互转 |

全部符号声明在 `include/mcp/JsonRpc.hpp`。

## 内容变体

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `Icon` | `struct { std::string src; optional<string> mime_type; optional<vector<string>> sizes; optional<string> theme; }` | `src` 必填 | 图标条目 |
| `Annotations` | `struct { optional<vector<string>> audience; optional<double> priority; optional<string> last_modified; }` | 全 `nullopt` | 内容标注 |
| `TextResourceContents` | `struct { uri; text; optional<mime_type>; optional<JsonValue> meta; }` | — | 文本型资源体 |
| `BlobResourceContents` | `struct { uri; blob; optional<mime_type>; optional<JsonValue> meta; }` | — | Base64 资源体 |
| `ResourceContents` | `std::variant<TextResourceContents, BlobResourceContents>` | — | 资源内容二选一 |
| `TextContent` | `struct { std::string type = "text"; std::string text; optional<string> mime_type; optional<Annotations> annotations; optional<JsonValue> meta; }` | `type = "text"` | 注意 `type` 有默认值，聚合初始化仍需显式给出 |
| `ImageContent` / `AudioContent` | `struct { type("image"/"audio"); data; mime_type; optional<annotations>; optional<meta>; }` | `type` 内置 | `mime_type` **非** optional |
| `EmbeddedResource` | `struct { type("resource"); ResourceContents resource; optional<annotations>; optional<meta>; }` | — | 内嵌资源 |
| `ResourceLink` | `struct { type("resource_link"); uri; name; optional<title/description/mime_type/size>; vector<Icon> icons; optional<annotations>; optional<meta>; }` | — | 资源链接 |
| `ContentVariant` | `std::variant<TextContent, ImageContent, AudioContent, EmbeddedResource, ResourceLink>` | — | `CallToolResult::content` 的元素类型 |

全部符号声明在 `include/mcp/Content.hpp`。

## `_meta`、进度令牌与缓存提示

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `ProgressToken` | `using ProgressToken = std::variant<std::string, int64_t>` | — | `_meta.progressToken` |
| `LoggingLevel` | `enum class LoggingLevel { Debug, Info, Notice, Warning, Error, Critical, Alert, Emergency }` | — | **协议层**日志级别，与 SDK 内部 `LogLevel` 无关 |
| `RequestMeta.progress_token` | `std::optional<ProgressToken>` | `nullopt` | 进度关联令牌 |
| `RequestMeta.protocol_version` | `std::string` | `kLatestProtocolVersion`（`"2026-07-28"`） | 出向 `_meta` 版本戳 |
| `RequestMeta.client_info` / `client_capabilities` | `optional<Implementation>` / `optional<ClientCapabilities>` | `nullopt` | 仅 `initialize` 期填充 |
| `RequestMeta.log_level` | `std::optional<LoggingLevel>` | `nullopt` | 客户端要求的日志级别 |
| `RequestMeta.extensions` | `std::optional<JsonValue>` | `nullopt` | `_meta` 扩展槽 |
| `RequestMeta.traceparent` / `tracestate` / `baggage` | `std::optional<std::string>` ×3 | `nullopt` | W3C 追踪上下文，喂给 span 钩子 |
| `CacheHint.ttl_ms` / `cache_scope` | `std::optional<int64_t>` / `std::optional<std::string>` | `nullopt` | 列表类结果的缓存提示 |

全部符号声明在 `include/mcp/Meta.hpp`。

## 能力声明

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `ToolsCapability` / `PromptsCapability` / `RootsCapability` | `struct { std::optional<bool> list_changed; }` | `nullopt` | 三者结构相同 |
| `ResourcesCapability` | `struct { optional<bool> subscribe; optional<bool> list_changed; }` | `nullopt` | 唯一带 `subscribe` |
| `SamplingCapability` / `EmptyCapability` | `struct {}` | — | 空标记；`LoggingCapability`、`TasksCapability`、`SubscriptionsCapability`、`CompletionsCapability` 均为 `EmptyCapability` 别名 |
| `ElicitationCapability` | `struct { std::optional<JsonValue> form; std::optional<JsonValue> url; }` | `nullopt` | 声明支持的 elicitation 形态 |
| `kTasksExtensionId` / `kSkillsExtensionId` | `inline constexpr std::string_view` | `"io.modelcontextprotocol/tasks"` / `".../skills"` | 扩展声明键 |
| `TasksExtensionSettings` / `SkillsExtensionSettings` | `struct {}` / `struct { bool directory_read{false}; }` | `false` | 扩展设置体 |
| `ServerCapabilities` | `struct`：`tools`/`resources`/`prompts`/`logging`/`sampling`/`roots`/`elicitation`/`completions`/`subscriptions` 均 `optional<>`，加 `optional<map<string, JsonValue>> extensions`、`optional<JsonValue> experimental` | 全 `nullopt` | 共 11 个字段 |
| `ClientCapabilities` | `struct`：`roots`/`sampling`/`elicitation`/`extensions`/`experimental` | 全 `nullopt` | 共 5 个字段 |

全部符号声明在 `include/mcp/Capabilities.hpp`。

## 实现信息

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `Implementation` | `struct { std::string name; std::string version; optional<string> title; vector<Icon> icons; optional<string> description; optional<string> website_url; }` | `name`/`version` 必填，`icons = {}` | 同时用于 `client_info` 与 `server_info` |

声明在 `include/mcp/Implementation.hpp`。

## 声明类型：`Tool` 与 `ToolOptions`

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `Tool::name` | `std::string` | 空（必填） | 注册名 |
| `Tool::title` / `description` | `std::optional<std::string>` | `nullopt` | 人类可读信息 |
| `Tool::input_schema` | `JsonValue`（**非** optional） | 默认构造 = `null` | 线格式必填，注册期由 `ToolOptions` 补默认 schema |
| `Tool::output_schema` | `std::optional<JsonValue>` | `nullopt` | 结构化输出模式 |
| `Tool::annotations` | `std::optional<ToolAnnotations>` | `nullopt` | 行为注解 |
| `Tool::icons` | `std::vector<Icon>` | `{}` | 图标集 |
| `Tool::meta` | `std::optional<JsonValue>` | `nullopt` | `_meta` 透传 |
| `Tool::execution` | `std::optional<ToolExecution>` | `nullopt` | 任务/人审执行模式 |
| `ToolOptions` | `struct`，13 个字段 + 3 个 fluent setter | 见下 | 注册入参，与 `Tool` 字段并不一一对应 |
| `ToolOptions::name` | `std::optional<std::string>` | `nullopt` | 未被消费：实际名取 `RegisterTool`/`Create` 的 `name` 参数 |
| `ToolOptions::{description,title,destructive,idempotent,read_only_hint,open_world_hint}` | `std::optional<std::string>` ×2 + `std::optional<bool>` ×4 | `nullopt` | 后四个 bool 组装进 `ToolAnnotations` |
| `ToolOptions::use_structured_content` | `bool` | `false` | 置真时把 `output_schema` 设为 `{"type":"object"}` |
| `ToolOptions::{input_schema,output_schema,icons,meta,execution}` | `optional<JsonValue>` ×3（含 `meta`）、`vector<Icon>`、`optional<ToolExecution>` | `nullopt` / `{}` | **只能直接赋值** |
| `ToolOptions::Description` / `Title` / `InputSchema` | `ToolOptions& Description(std::string_view)`、`ToolOptions& Title(std::string_view)`、`ToolOptions& InputSchema(JsonValue)` | — | **仅此三个** fluent setter，可链式；其余字段一律成员赋值 |

::: warning
`ToolOptions` 没有 `Name()`、`OutputSchema()`、`Icons()`、`Meta()`、`Execution()`、`Destructive()` 等 setter，写 `ToolOptions{}.OutputSchema(x)` 会编译失败。
:::

全部声明在 `include/mcp/McpTypes.hpp`。

## 其余声明类型

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `Resource` | `struct { uri; name; optional<title/description/mime_type/size>; optional<vector<Icon>> icons; optional<ResourceAnnotations> annotations; optional<JsonValue> meta; }` | 必填 `uri`/`name` | 静态资源描述 |
| `ResourceTemplate` | `struct`，首字段为 `uri_template`，其余同 `Resource`（无 `size`） | 必填 `uri_template`/`name` | 模板资源描述 |
| `ResourceAnnotations` | `struct { optional<vector<string>> audience; optional<double> priority; optional<string> last_modified; }` | 全 `nullopt` | 与 `Annotations` 同形但独立类型 |
| `ToolAnnotations` | `struct { optional<string> title; optional<bool> read_only_hint/idempotent_hint/open_world_hint/destructive_hint; }` | 全 `nullopt` | 由 `ToolOptions` 的四个 bool 装配 |
| `ToolExecutionMode` / `TaskSupport` | `enum class { Auto, Manual, Task }` / `enum class { Optional, Required }` | `Auto` / 无默认 | 执行模式与任务支持强度 |
| `ToolExecution` | `struct { ToolExecutionMode mode{Auto}; optional<string> human_use; optional<TaskSupport> task_support; }` | `mode = Auto` | 工具执行声明 |
| `Prompt` | `struct { name; optional<title/description>; optional<vector<PromptArgument>> arguments; optional<vector<Icon>> icons; optional<JsonValue> meta; }` | 必填 `name` | 提示词描述 |
| `PromptArgument` | `struct { name; optional<description>; optional<bool> required; }` | 必填 `name` | 参数声明 |
| `PromptMessage` | `struct { std::string role; ContentVariant content; }` | — | `prompts/get` 返回条目 |
| `ResourceOptions` | `struct { optional<name/description/title/mime_type>; vector<Icon> icons; }` + `Description`/`Title`/`MimeType` | `nullopt` / `{}` | 3 个 setter，其余成员赋值；`name` 未被消费，实际名取 `RegisterResource` 的 `name` 实参 |
| `PromptOptions` | `struct { optional<name/description/title>; vector<Icon> icons; optional<vector<PromptArgument>> arguments; }` + `Description`/`Title`/`Arguments` | `nullopt` / `{}` | 3 个 setter，其余成员赋值；`name` 未被消费，实际名取 `RegisterPrompt` 的 `name` 实参 |
| `SkillResource` | `struct { std::string uri; std::string digest; int64_t size{0}; }` | `size = 0` | 技能内文件摘要 |
| `SkillResources` | `std::variant<std::vector<SkillResource>, std::string>` | — | 列表或目录引用二选一 |
| `Skill` | `struct { std::string uri; JsonValue frontmatter; SkillResources resources; optional<JsonValue> meta; }` | 必填 `uri`/`frontmatter`/`resources` | 技能声明（`skills/*`） |

全部声明在 `include/mcp/McpTypes.hpp`；两处「`name` 未被消费」的取实在 `src/server/McpServer.cpp` 的注册函数。

## 请求参数类型

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `PaginatedRequestParams` | `struct { optional<string> cursor; optional<RequestMeta> meta; }` | `nullopt` | 别名：`ListToolsRequestParams`、`ListResourcesRequestParams`、`ListResourceTemplatesRequestParams`、`ListPromptsRequestParams`、`ListSkillsRequestParams` |
| `ResourceRequestParams` | `struct { std::string uri; optional<RequestMeta> meta; optional<JsonValue> input_responses; optional<string> request_state; }` | 必填 `uri` | 别名：`ReadResourceRequestParams`、`SubscribeRequestParams`、`UnsubscribeRequestParams` |
| `CallToolRequestParams` | `struct { name; optional<JsonValue> arguments; optional<RequestMeta> meta; optional<JsonValue> input_responses; optional<string> request_state; }` | 必填 `name` | MRTR 续跑靠后两字段 |
| `GetPromptRequestParams` | `struct { name; optional<JsonValue> arguments; optional<RequestMeta> meta; optional<JsonValue> input_responses; optional<string> request_state; }` | 必填 `name` | 与 `CallTool` 同构 |
| `CompleteRequestParams` | `struct { JsonValue ref; std::string argument_name; std::string argument_value; map<string,string,less<>> context_arguments; optional<RequestMeta> meta; }` | `context_arguments = {}` | `ref` 为原始对象（`{"type":"ref/prompt","name":...}`），非强类型 |
| `InitializeRequestParams` | `struct { std::string protocol_version; ClientCapabilities capabilities; Implementation client_info; }` | 无 optional | 三字段全必填 |
| `DiscoverRequestParams` / `ListRootsRequestParams` | `struct {}` | — | 空参数请求 |
| `Pagination` | `struct { std::optional<std::string> next_cursor; }` | `nullopt` | 游标信封 |
| `FileRef` | `struct { std::string uri; int64_t offset{0}; int64_t length{0}; }` | `0` | `x-mcp-fileref` 参数；序列化时 `0` 值字段被省略 |
| `SubscriptionFilter` | `struct { optional<bool> tools_list_changed/prompts_list_changed/resources_list_changed; vector<string> resource_subscriptions; }` | `nullopt` / `{}` | `subscriptions/listen` 过滤器 |
| `SubscriptionsListenRequestParams` | `struct { SubscriptionFilter notifications; optional<RequestMeta> meta; }` | — | 监听订阅入参 |
| `CreateMessageRequestParams` / `SamplingMessage` | `struct { vector<SamplingMessage> messages; int64_t max_tokens; optional<string> stop_reason/model_preference; }` / `struct { role; ContentVariant content; }` | `max_tokens` 必填 | `sampling/*` 已废弃（SEP-2577），类型仍保留 |
| `SetLevelRequestParams` | `struct { LoggingLevel level; }` | — | `logging/setLevel` 入参 |
| `LoggingMessageNotificationParams` | `struct { LoggingLevel level; optional<string> logger; JsonValue data; }` | `logger = nullopt` | `notifications/message` |
| `ProgressNotificationParams` | `struct { ProgressToken progress_token; double progress; optional<double> total; optional<string> message; }` | — | `on_progress` 回调入参 |
| `CancelledNotificationParams` | `struct { RequestId request_id; optional<string> reason; }` | — | 取消通知 |

全部声明在 `include/mcp/McpTypes.hpp`。

## 结果类型

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `Result` | `struct { std::optional<JsonValue> meta; ResultType result_type{Complete}; }` | `Complete` | 所有结果基类；`ResultType = { Complete, InputRequired, Task }` |
| `EmptyResult` / `PingResult` | `struct EmptyResult : Result {}`；`using PingResult = EmptyResult` | — | 空结果 |
| `CacheableResult` | `struct CacheableResult : Result { std::optional<std::string> etag; }` | `nullopt` | ETag 由全局开关控制 |
| `CallToolResult` | `struct : Result { vector<ContentVariant> content; optional<JsonValue> structured_content; bool is_error{false}; optional<InputRequiredResult> input_required; }` | `is_error = false` | 共 4 个自有字段 + 基类 2 个 |
| `ListToolsResult` / `ListResourcesResult` / `ListResourceTemplatesResult` / `ListPromptsResult` | `struct : Result { vector<…>; optional<string> next_cursor; optional<CacheHint> cache_hint; }` | `nullopt` | 四个分页列表结果同构 |
| `ReadResourceResult` | `struct : Result { vector<ResourceContents> contents; optional<CacheHint> cache_hint; optional<InputRequiredResult> input_required; }` | — | 读取资源 |
| `GetPromptResult` | `struct : Result { vector<PromptMessage> messages; optional<string> description; optional<InputRequiredResult> input_required; }` | — | 获取提示词 |
| `CompleteResult` | `struct : Result { JsonValue completion; }` | 默认构造 `null` | 补全结果；填充示例 `completion/complete` 的 `{"values":[…]}` |
| `InitializeResult` | `struct : Result { std::string protocol_version; ServerCapabilities capabilities; Implementation server_info; optional<string> instructions; }` | 前三必填 | 握手结果 |
| `DiscoverResult` | `struct : Result { vector<string> supported_versions; ServerCapabilities capabilities; Implementation server_info; optional<string> instructions; optional<CacheHint> cache_hint; }` | — | `server/discover` 结果 |
| `SkillsListResult` / `SkillsGetResult` / `ReadDirectoryResult` / `DirectoryChild` | `struct : Result { …; optional<int64_t> ttl_ms; optional<string> cache_scope; }` 等 | `nullopt` | 技能与目录浏览结果，缓存字段用 `ttl_ms`/`cache_scope` 而非 `CacheHint` |
| `CreateMessageResult` / `ListRootsResult` / `Root` | `struct : Result { role; ContentVariant content; string model; optional<stop_reason>; }` / `{ vector<Root> roots; }` / `{ uri; optional<name>; }` | — | 已废弃能力的类型 |

全部声明在 `include/mcp/McpTypes.hpp`。

## MRTR 输入请求

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `InputRequest` | `struct { std::string method; JsonValue params; }` | 必填 | 单条待补输入 |
| `InputRequests` | `using InputRequests = std::map<std::string, InputRequest, std::less<>>` | — | 按 id 索引，透明查键 |
| `InputRequiredResult` | `struct { InputRequests input_requests; std::optional<std::string> request_state; }` | `nullopt` | 结果侧的续跑载荷 |
| `MakeInputRequestForElicitation` / `...ForSampling` / `...ForRoots` | `InputRequest Make...(const *RequestParams&)` | — | 三个构造器 |
| `MakeInputResponseFromElicitResult` / `...FromCreateMessageResult` / `...FromListRootsResult` | `JsonValue Make...(const ...Result&)` | — | 把用户应答包成 `input_responses` 条目 |
| `IsInputRequiredResult` / `ExtractInputRequests` | `bool (const JsonValue&)` / `std::optional<InputRequests> (const JsonValue&)` | — | 从原始结果 JSON 识别 input_required |
| `ElicitRequestParams` | `struct { std::string message; optional<JsonValue> requested_schema; std::string mode{"form"}; optional<string> url; optional<string> elicitation_id; }` | `mode = "form"` | 服务端发起 elicitation 的入参 |
| `ElicitResult` | `struct : Result { std::string action; optional<JsonValue> content; }` | `action` 无默认 | 线格式结果（`accept`/`decline`/`cancel`） |
| `ElicitResultTyped<T>` | `template <typename T> struct { std::string action = "cancel"; optional<T> content; bool is_accepted() const; }` | `action = "cancel"` | 强类型应答视图；`is_accepted()` 是**该模板唯一的 snake_case 成员**，勿改名 |

全部声明在 `include/mcp/McpTypes.hpp`。

## 任务相关类型

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `TaskStatus` | `enum class { Pending, Working, Completed, Failed, Cancelled, InputRequired }` | `Pending` | 任务状态机 |
| `TaskState` | `struct`（17 个字段：`task_id`/`owner`/`status`/`result`/`error_message`/`input_required`/`progress`/`progress_total`/`created_at`/`status_message`/`last_updated_at`/`ttl_ms`/`poll_interval_ms`/`input_requests`/`input_responses`/`tool_name`/`tool_arguments`） | `status = Pending`、`progress = 0` | 定义在**服务端**头，非 `McpTypes.hpp`；存储侧细节见 [进程与存储参考](/zh/reference/process-and-storage) |
| `CreateTaskResult` | `struct : Result { task_id; status; created_at; optional<status_message/last_updated_at/ttl_ms/poll_interval_ms/input_requests>; }` | 前三必填 | `tasks/*` 创建结果 |
| `GetTaskResult` | `struct`（**不继承** `Result`，14 个字段，含 `task_result_type{"task"}`、`error`、`error_message`、`input_required`、`meta`） | `task_result_type = "task"` | 与 `CreateTaskResult` 不同基类，勿混淆 |
| `UpdateTaskResult` / `CancelTaskResult` | `using ... = EmptyResult` | — | 无自有字段 |
| `GetTaskRequestParams` / `UpdateTaskRequestParams` / `CancelTaskRequestParams` / `TaskStatusNotificationParams` | `{ task_id }` / `{ task_id, optional<result>, optional<input_responses> }` / `{ task_id, optional<reason> }` / `{ task_id, status }` | 按值 | 任务方法与通知入参 |
| `SkillsGetRequestParams` / `ReadDirectoryRequestParams` | `{ uri, optional<RequestMeta> meta }` / `{ uri, optional<string> cursor, optional<RequestMeta> meta }` | 必填 `uri` | 技能读取与目录浏览入参 |

结果与入参声明在 `include/mcp/McpTypes.hpp`；`TaskStatus` 与 `TaskState` 声明在 `include/mcp/server/McpTaskStore.hpp`。

## 请求选项

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `RequestOptions::meta` | `std::optional<JsonValue>` | `nullopt` | 出向 `_meta` |
| `RequestOptions::read_timeout_ms` | `std::optional<int64_t>` | `nullopt` | 读超时（区别于总超时） |
| `RequestOptions::input_responses` | `std::optional<JsonValue>` | `nullopt` | MRTR 续跑载荷 |
| `RequestOptions::request_state` | `std::optional<std::string>` | `nullopt` | 与 `input_responses` 配对 |
| `RequestOptions::on_progress` | `std::function<void(const ProgressNotificationParams&)>` | 未设置 | 等价于 `mcp::ProgressCallback`（别名在 `include/mcp/client/McpClient.hpp`） |
| `RequestOptions::timeout` | `std::chrono::milliseconds` | `kDefaultRequestTimeout` = `60000` | 总超时 |
| `RequestOptions::cancel_on_timeout` | `bool` | `true` | 超时即发取消 |
| `RequestOptions::cancel_token` | `CancelToken` | 默认构造（未取消） | 成员是值类型，非指针 |
| `CacheableRequestOptions` | `struct : RequestOptions { optional<string> cache_mode; optional<int64_t> max_age_ms; }` | `nullopt` | 读类请求的缓存重载入口 |

::: info
`RequestOptions` 共 **8** 个字段，不是迁移文档里那 4 个字段的形状。
:::

全部声明在 `include/mcp/McpTypes.hpp`。

## 取消与错误

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `CancelToken` | `class MCP_API CancelToken`（值语义，内部 `shared_ptr<State>`） | 未取消 | 可拷贝、跨线程共享同一状态 |
| `Cancel` | `void Cancel(std::string_view reason = {})` | 空原因 | 置位并广播处理器 |
| `IsCancelled` / `Reason` | `bool () const` / `std::string () const` | `false` / `""` | 状态查询 |
| `AddCancelHandler` / `RemoveCancelHandler` | `std::size_t AddCancelHandler(CancelHandler)` / `void RemoveCancelHandler(std::size_t)` | — | 返回句柄 id 供移除；`CancelHandler = std::function<void(std::string_view)>` |
| `McpErrorCode` | `enum class McpErrorCode : int32_t` | — | JSON-RPC 标准码 + SDK 扩展码；全表见 [错误码参考](/zh/reference/errors) |
| `make_error_code` / `McpErrorCategory` / `GetMcpErrorCategory` | `std::error_code` / `class std::error_category` / `const McpErrorCategory&` | — | `is_error_code_enum` 已特化，可直接隐式转 `std::error_code` |
| `McpError` | `class McpError : public std::runtime_error` | — | 唯一业务异常类型 |
| `McpError` 构造 | `McpError(McpErrorCode, std::string message)` / `McpError(McpErrorCode, std::string, std::optional<JsonValue> data)` | — | 第二重载携带 `data` |
| `Code` / `ErrorMessage` / `Data` | `McpErrorCode Code() const noexcept` / `std::string_view ErrorMessage() const noexcept` / `const std::optional<JsonValue>& Data() const noexcept` | — | 三个访问器均 `noexcept` |

`CancelToken` 声明在 `include/mcp/CancelToken.hpp`，错误码与分类器在 `include/mcp/ErrorCodes.hpp`，异常类型在 `include/mcp/McpError.hpp`。

## 日志与观测

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `LogLevel` | `enum class LogLevel : int { Off=0, Error=1, Warning=2, Info=3, Debug=4, Trace=5 }` | `Off` | SDK 内部日志级别；与协议层 `LoggingLevel` 是两套 |
| `MCP_LOG_LEVEL` | 环境变量 `0`–`5` | 未设置 = 全关 | 首次访问日志状态时读取；`≥5` 归 `Trace` |
| `LogContext` | `struct { std::string request_id; std::string method; std::string session_id; std::string peer; }` | 全空 | `MCP_LOG_CTX` 携带 |
| `LogRecord` | `struct { LogLevel level; string_view tag; string_view message; const char* file; int line; const LogContext* context; }` | — | handler 入参；`tag`/`message` 仅在回调期内有效 |
| `LogHandler` | `using LogHandler = std::function<void(const LogRecord&)>` | — | handler 存在时不再写 `stderr` |
| `SetLogLevel` / `GetLogLevel` | `void SetLogLevel(LogLevel)` / `LogLevel GetLogLevel()` | `Off` | 运行时覆盖环境变量 |
| `SetLogHandler` / `GetLogHandler` | `void SetLogHandler(LogHandler)` / `LogHandler GetLogHandler()` | 未设置 | 传 `nullptr` 恢复默认 `stderr` 输出 |
| `MCP_LOG` / `MCP_LOG_CTX` | 宏 | — | SDK 内部使用；级别名须与 `LogLevel` 成员一致 |
| `SpanKind` | `enum class SpanKind : std::uint8_t { ServerRequest, TransportReceive, TransportSend }` | `ServerRequest` | span 类别 |
| `SpanPhase` | `enum class SpanPhase : std::uint8_t { Begin, End }` | `Begin` | 成对事件的两个阶段 |
| `SpanEvent` | `struct { SpanPhase phase; SpanKind kind; string name/method/request_id/traceparent/tracestate/baggage; bool failed; }` | `failed = false` | 追踪上下文来自 `RequestMeta` |
| `SpanHandler` | `using SpanHandler = std::function<void(const SpanEvent&)>` | — | 注入口在服务端/客户端选项，见对应参考页 |

日志侧声明在 `include/mcp/Log.hpp`，span 侧在 `include/mcp/SpanHooks.hpp`。

## 方法名与版本常量

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `methods::k*` | `inline constexpr std::string_view` | — | 请求方法名（`initialize`、`tools/call`、`server/discover`、`subscriptions/listen`、`tasks/*`、`skills/*`、`logging/setLevel` 等）；禁止用字面量拼方法名 |
| `notifications::k*` | `inline constexpr std::string_view` | — | 通知方法名（含 `notifications/tasks/*`、`notifications/elicitation/complete`） |
| `kProtocolVersions` | `inline constexpr std::string_view[]` | 5 项 | `2024-11-05` … `2026-07-28` |
| `kLatestProtocolVersion` / `kModernProtocolVersion` / `kLegacyProtocolVersion` / `kDefaultNegotiatedProtocolVersion` | `inline constexpr std::string_view` | `"2026-07-28"` / `"2026-07-28"` / `"2025-11-25"` / `"2025-03-26"` | 双 era 判定基准；缺失版本声明时的默认协商值 |
| `IsModernProtocolVersion` | `constexpr bool IsModernProtocolVersion(std::string_view)` | — | 字典序比较 `>= kLatestProtocolVersion` |
| `kJsonRpcVersion` / `kSdkVersion` | `inline constexpr std::string_view` | `"2.0"` / `kSdkVersion` 随发布变化 | 信封版本与 SDK 版本（后者是服务端发现信息默认值来源，文档不写死数字） |

方法名在 `include/mcp/Methods.hpp`，版本常量在 `include/mcp/ProtocolVersion.hpp` 与 `include/mcp/McpVersion.hpp`。

## 序列化约定与表达惯例

| 惯例 | 形式 | 默认值 | 说明 |
|---|---|---|---|
| 类型转换入口 | 每个结构体配一对自由函数 `JsonValue SerializeX(const X&)` / `X DeserializeX(const JsonValue&)` | — | 没有成员 `to_json`/`from_json`，ADL 查找自由函数 |
| ETag 全局开关 | `void SetEtagCacheEnabled(bool)` / `bool IsEtagCacheEnabled()` | — | 进程级，影响 `CacheableResult` 是否带 `etag` |
| 可选字段 | `std::optional<T>` | `std::nullopt` | 缺失与「空字符串/0」是两种语义 |
| JSON 按值传递 | `JsonValue`（非引用）入参与返回 | — | `JsonValue` 内部 `variant` + 深拷贝语义，按值便于转移 |
| 工厂返回所有权 | `McpClient`/`McpServer::Create` 返回 `std::unique_ptr`；`IClientTransport::Connect()` 返回 `std::shared_ptr<ITransport>` | — | 见 [客户端参考](/zh/reference/client)、[传输层参考](/zh/reference/transports) |
| 设计动机 | 一句话：让「未设置」可被类型系统区分、让 JSON 拥有明确生命周期，从而免除哨兵值与悬垂引用 | — | 惯例本身不进代码注释，意图靠类型表达 |

涉及头文件：`include/mcp/McpTypes.hpp`、`include/mcp/Content.hpp`、`include/mcp/Capabilities.hpp`、`include/mcp/JsonValue.hpp`、`include/mcp/JsonRpc.hpp`、`include/mcp/CancelToken.hpp`、`include/mcp/Transport.hpp`。

相关任务写法：[/zh/howto/validate-tool-input](/zh/howto/validate-tool-input)、[/zh/howto/report-progress](/zh/howto/report-progress)。

## 本页对应源码

- `include/mcp/JsonValue.hpp`、`include/mcp/JsonRpc.hpp`、`include/mcp/Content.hpp`、`include/mcp/Meta.hpp`
- `include/mcp/Capabilities.hpp`、`include/mcp/Implementation.hpp`、`include/mcp/McpTypes.hpp`、`include/mcp/Methods.hpp`
- `include/mcp/Log.hpp`、`include/mcp/SpanHooks.hpp`、`include/mcp/CancelToken.hpp`
- `include/mcp/ErrorCodes.hpp`、`include/mcp/McpError.hpp`、`include/mcp/server/McpTaskStore.hpp`（`TaskState`/`TaskStatus`）
- `include/mcp/Methods.hpp`、`include/mcp/ProtocolVersion.hpp`、`include/mcp/McpVersion.hpp`
