# 客户端门面（`McpClient`）参考

本页是 `McpClient`、`ClientOptions`、`RequestHandle`/`RequestOutcome` 与 `ConnectMode` 的**唯一权威签名与字段表**；操作步骤见后续 how-to 页面，how-to 页只链接本页、不重复表格。版本协商语义见 `/en/learn/protocol-versions`（中文 `/zh/learn/protocol-versions`），数据类型见 `/zh/reference/types`，错误码见 `/zh/reference/errors`，传输构造见 `/zh/reference/transports`。

## 工厂与生命周期

| 符号 | 签名/类型 | 默认值/返回 | 说明 | 证据 |
|---|---|---|---|---|
| `McpClient::Create` | `static std::unique_ptr<McpClient> Create(std::shared_ptr<ITransport> transport, const ClientOptions& options = {})` | `std::unique_ptr<McpClient>` | 构造后**同步执行版本协商**（`Auto`：先 `server/discover` 探测，失败回退 `initialize` 握手），返回时会话已可用；协商失败直接抛异常 | `include/mcp/client/McpClient.hpp:34-36`；`src/client/McpClient.cpp:679-691`、`611-636` |
| `~McpClient` | `~McpClient()` | — | 自动 `Close()` 并摘除传输回调，故 `unique_ptr` 析构即断开 | `McpClient.hpp:38`；`McpClient.cpp:638-641` |
| `Close` | `void Close()` | — | 关闭会话处理器，并清空私有作用域响应缓存与缓存鉴权上下文 | `McpClient.hpp:150`；`McpClient.cpp:693-698` |
| `GetServerCapabilities` | `const ServerCapabilities& GetServerCapabilities() const` | **返回引用** | 协商结果快照，指向内部状态，不得越出客户端生命周期 | `McpClient.hpp:40`；`McpClient.cpp:818-821` |
| `GetServerInfo` | `const Implementation& GetServerInfo() const` | 引用 | 同上 | `McpClient.hpp:41`；`McpClient.cpp:822-825` |
| `GetInstructions` | `std::optional<std::string> GetInstructions() const` | `nullopt` | 服务端 `instructions` | `McpClient.hpp:42`；`McpClient.cpp:826-829` |
| `GetNegotiatedProtocolVersion` | `std::string_view GetNegotiatedProtocolVersion() const` | 协商所得版本 | 视图指向内部字符串 | `McpClient.hpp:43`；`McpClient.cpp:830-833` |
| `IsModernProtocol` | `bool IsModernProtocol() const` | 协商结果标志 | 直接返回协商结果的 `is_modern`，**不等价于** `IsModernProtocolVersion(协商版本)`：`Pin` 档位恒置 `true`，即使钉的是旧版本也报 modern | `McpClient.hpp:44`；`McpClient.cpp:834-837`、`429-434` |

::: info
客户端无 `Run()`：会话处理器在 `Create()` 内已启动消息线程，通知与反向请求经 handler 字段异步投递。
:::

## Typed 请求 API：工具、资源、提示

| 符号 | 签名/类型 | 默认值/返回 | 说明 | 证据 |
|---|---|---|---|---|
| `ListTools` | `ListToolsResult ListTools(std::optional<std::string> cursor = std::nullopt)` | 单页 | 先查响应缓存（命中即不发请求），带 etag；`notModified` 时回缓存体 | `McpClient.hpp:46-47`；`McpClient.cpp:1215-1235` |
| `ListToolsAll` | `ListToolsResult ListToolsAll()` | `next_cursor` 为空 | 循环调用 `ListTools` 聚合全部分页 | `McpClient.hpp:48`；`McpClient.cpp:1237-1251` |
| `SearchTools` | `ListToolsResult SearchTools(std::optional<std::string> query = std::nullopt, std::optional<int64_t> page_size = std::nullopt)` | 聚合结果 | 逐页直发 `tools/list`，**仅 `enable_progressive_discovery` 为真时**才下发 `query`/`pageSize`；不走缓存 | `McpClient.hpp:49-51`；`McpClient.cpp:1252-1279` |
| `CallTool` | `CallToolResult CallTool(std::string_view name, std::optional<JsonValue> arguments = std::nullopt, const RequestOptions& options = {})` | `CallToolResult` | 走 MRTR 履行环；对端返回 task 句柄时自动轮询到终态再返回；`HeaderMismatch` 时清缓存、重取 `tools/list` 后原样重试一次 | `McpClient.hpp:52-55`；`McpClient.cpp:1312-1357`、`1280-1310` |
| `ListResources` / `ListResourcesAll` | `ListResourcesResult (std::optional<std::string> cursor = std::nullopt)` / `ListResourcesResult ()` | 同工具语义 | 缓存 + etag；`All` 聚合分页 | `McpClient.hpp:57-59`；`McpClient.cpp:1359-1395` |
| `ListResourceTemplates` / `ListResourceTemplatesAll` | 同上 | 同上 | 结果字段为 `resource_templates` | `McpClient.hpp:60-62`；`McpClient.cpp:1397-1433` |
| `ReadResource` | `ReadResourceResult ReadResource(std::string_view uri, const CacheableRequestOptions& options = {})` | `ReadResourceResult` | 唯一吃 `CacheableRequestOptions` 的资源方法：`cache_mode` 决定是否读/写缓存，`max_age_ms` 限制新鲜度；走 MRTR | `McpClient.hpp:63-65`；`McpClient.cpp:1435-1470` |
| `SubscribeResource` / `UnsubscribeResource` | `EmptyResult SubscribeResource(std::string_view uri)` / `EmptyResult UnsubscribeResource(std::string_view uri)` | `EmptyResult` | 资源级订阅（区别于 `SubscribeAsync` 的通知过滤器） | `McpClient.hpp:66-67`；`McpClient.cpp:1472-1488` |
| `ListPrompts` / `ListPromptsAll` | `ListPromptsResult (std::optional<std::string> cursor = std::nullopt)` / `ListPromptsResult ()` | 同上 | 缓存 + etag + 分页聚合 | `McpClient.hpp:69-71`；`McpClient.cpp:1490-1525` |
| `GetPrompt` | `GetPromptResult GetPrompt(std::string_view name, std::optional<JsonValue> arguments = std::nullopt, const RequestOptions& options = {})` | `GetPromptResult` | 走 MRTR；对端返回 task 句柄时轮询到终态 | `McpClient.hpp:72-75`；`McpClient.cpp:1527-1557` |
| `Complete` | `CompleteResult Complete(const CompleteRequestParams& params)` | `CompleteResult` | `completion/complete`，固定 60s 超时，不走 MRTR | `McpClient.hpp:87`；`McpClient.cpp:1815-1821` |

## Typed 请求 API：技能、任务、发现

| 符号 | 签名/类型 | 默认值/返回 | 说明 | 证据 |
|---|---|---|---|---|
| `ListSkills` | `SkillsListResult ListSkills(std::optional<std::string> cursor = std::nullopt)` | 分页结果 | `enable_skills_extension=false` 时抛 `McpError(MethodNotFound, "skills extension is disabled")` | `McpClient.hpp:77-78`；`McpClient.cpp:1560-1592` |
| `GetSkill` | `SkillsGetResult GetSkill(std::string_view uri, const CacheableRequestOptions& options = {})` | `SkillsGetResult` | 同上开关门控；走 MRTR 与缓存 | `McpClient.hpp:79-81`；`McpClient.cpp:1594-1632` |
| `ReadDirectory` | `ReadDirectoryResult ReadDirectory(std::string_view uri, std::optional<std::string> cursor = std::nullopt, const CacheableRequestOptions& options = {})` | 子项列表 | 同上开关门控；服务端还需 `SetSkillsDirectoryRead(true)` | `McpClient.hpp:82-85`；`McpClient.cpp:1634-1679` |
| `CallToolAsTask` | `GetTaskResult CallToolAsTask(std::string_view name, std::optional<JsonValue> arguments = std::nullopt, const RequestOptions& options = {})` | 任务句柄（`task_id`/`status`） | 对端未任务化时**降级**返回 `task_id="<none>"`、`status="completed"`、`result` 为原始工具结果 JSON | `McpClient.hpp:89-92`；`McpClient.cpp:1681-1735` |
| `GetTask` | `GetTaskResult GetTask(std::string_view task_id)` | `GetTaskResult` | 固定 600s 超时；**不检查** `enable_tasks_extension` | `McpClient.hpp:94`；`McpClient.cpp:1737-1745` |
| `UpdateTask` | `UpdateTaskResult UpdateTask(std::string_view task_id, std::optional<JsonValue> result = std::nullopt, std::optional<JsonValue> input_responses = std::nullopt)` | `EmptyResult` 别名 | 需 `enable_tasks_extension`；`result` 与 `input_responses` 由服务端互斥校验 | `McpClient.hpp:95-98`；`McpClient.cpp:1746-1763` |
| `CancelTask` | `CancelTaskResult CancelTask(std::string_view task_id, std::optional<std::string> reason = std::nullopt)` | `EmptyResult` 别名 | **不检查** `enable_tasks_extension` | `McpClient.hpp:99-101`；`McpClient.cpp:1764-1776` |
| `PollTaskToCompletion` | `GetTaskResult PollTaskToCompletion(const std::string& task_id, std::chrono::milliseconds poll_interval = std::chrono::milliseconds(500), std::chrono::seconds timeout = std::chrono::seconds(300))` | 终态 `GetTaskResult` | 需 `enable_tasks_extension`；服务端 `poll_interval_ms` 优先，上限 300s；终态为 `completed`/`failed`/`cancelled`；超预算抛 `InternalError` | `McpClient.hpp:103-106`；`McpClient.cpp:1777-1814`、`21-25` |
| `Discover` | `DiscoverResult Discover()` | 当次应答 | 重新发 `server/discover`，**不改变**已协商版本 | `McpClient.hpp:111`；`McpClient.cpp:1831-1836` |

## 请求选项类型

| 字段 | 类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `RequestOptions::meta` | `std::optional<JsonValue>` | `nullopt` | 作为 `_meta` 扩展下发；内含 `progressToken` 时用作进度键 | `include/mcp/McpTypes.hpp:398`；`McpClient.cpp:937-968` |
| `RequestOptions::read_timeout_ms` | `std::optional<int64_t>` | `nullopt` | 仅 `ReadResource`/`GetSkill`/`ReadDirectory`/`GetPrompt` 读取，作为本轮 MRTR 超时 | `McpTypes.hpp:399`；`McpClient.cpp:1457-1462`、`1546-1550` |
| `RequestOptions::input_responses` | `std::optional<JsonValue>` | `nullopt` | 手工履行 `input_required`（关闭自动补全时使用） | `McpTypes.hpp:400` |
| `RequestOptions::request_state` | `std::optional<std::string>` | `nullopt` | 回传服务端签发的 `requestState` | `McpTypes.hpp:401` |
| `RequestOptions::on_progress` | `std::function<void(const ProgressNotificationParams&)>` | 空 | 非空即自动登记进度回调并在返回时摘除 | `McpTypes.hpp:402`；`McpClient.hpp:27`、`McpClient.cpp:937-973` |
| `RequestOptions::timeout` | `std::chrono::milliseconds` | `kDefaultRequestTimeout`（60000ms） | **只有裸 `SendRequest`（`RequestHandle` 重载）读取**；typed 方法各有固定来源 | `McpTypes.hpp:26`、`403`；`McpClient.cpp:1857-1858` |
| `RequestOptions::cancel_on_timeout` | `bool` | `true` | 超时即向对端发 `notifications/cancelled` | `McpTypes.hpp:404`；`McpClient.cpp:1864` |
| `RequestOptions::cancel_token` | `CancelToken` | 默认构造 | 与 `RequestHandle::Cancel` 共用取消通道 | `McpTypes.hpp:405`；`McpClient.cpp:1867-1872` |
| `CacheableRequestOptions::cache_mode` | `std::optional<std::string>` | `nullopt` → `"use"` | `"bypass"` 跳过读与写缓存；`"refresh"` 跳过读但仍写 | `McpTypes.hpp:408-411`；`McpClient.cpp:1441-1468` |
| `CacheableRequestOptions::max_age_ms` | `std::optional<int64_t>` | `nullopt` | 允许的新鲜度上限 | `McpTypes.hpp:410`；`McpClient.cpp:1445-1451` |

## 裸接口与取消

| 符号 | 签名/类型 | 默认值/返回 | 说明 | 证据 |
|---|---|---|---|---|
| `SendRequest`（句柄重载） | `RequestHandle SendRequest(std::string_view method, JsonValue params, const RequestOptions& options)` | `RequestHandle` | **三个形参均无默认实参**；不经 MRTR，也不在协议错误时抛异常，错误以 `ErrorData` 形态落在 future 里 | `McpClient.hpp:130-133`；`McpClient.cpp:1844-1882` |
| `SendRequest`（旧语义重载） | `std::future<JsonValue> SendRequest(std::string_view method, JsonValue params, const RequestMeta& meta = {}, std::chrono::milliseconds timeout = kDefaultRequestTimeout)` | `future<JsonValue>` | 转调 `SendRequestRaw`，保留旧行为；`meta` 全空时自动补齐客户端 `_meta` | `McpClient.hpp:134-138`；`McpClient.cpp:1884-1903` |
| `SendRequestRaw` | `std::future<JsonValue> SendRequestRaw(std::string_view method, JsonValue params, const RequestMeta& meta = {}, std::chrono::milliseconds timeout = kDefaultRequestTimeout)` | `future<JsonValue>` | 与上一行等价，仅名字更显式 | `McpClient.hpp:139-143`；`McpClient.cpp:1893-1903` |
| `SendNotification` | `void SendNotification(std::string_view method, JsonValue params = {})` | — | 直发出站通知，无回执 | `McpClient.hpp:144`；`McpClient.cpp:1905-1907` |
| `CancelRequest` | `void CancelRequest(const RequestId& id, std::string_view reason = {})` | — | 按 id 发 `notifications/cancelled` | `McpClient.hpp:145`；`McpClient.cpp:1909-1911` |
| `RequestHandle::id` | `RequestId`（`std::variant<int64_t, std::string>`） | 已分配 id | 可直接交给 `CancelRequest` | `RequestHandle.hpp:15`；`include/mcp/JsonRpc.hpp:15` |
| `RequestHandle::future` | `std::future<RequestOutcome>` | `RequestOutcome = std::variant<JsonValue, ErrorData>` | 变体定义处为 `include/mcp/protocol/IncomingRequestMeta.hpp:29` | `include/mcp/client/RequestHandle.hpp:16`；`IncomingRequestMeta.hpp:29` |
| `RequestHandle::Cancel` | `void Cancel(std::string_view reason = {})` | — | 未绑定取消函数时静默无操作；句柄只_move_able（拷贝已删除） | `RequestHandle.hpp:34-36`、`29-32` |

## MRTR 多轮履行

| 字段 | 类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `ClientOptions::input_required_config` | `std::optional<InputRequiredConfig>` | **默认已置值**（`InputRequiredConfig{}`） | 置 `nullopt` 即关闭自动履行，`input_required` 结果原样返回调用方 | `include/mcp/client/ClientOptions.hpp:55-63`；`McpClient.cpp:991-993` |
| `input_required_config->auto_fulfill` | `bool` | `true` | 为假时不做任何 handler 调用 | `ClientOptions.hpp:56`；`McpClient.cpp:991-993` |
| `input_required_config->max_rounds` | `int` | `10` | 循环上界为 `round <= max_rounds`，超出抛 `InternalError`（`"MRTR: exceeded max_rounds (n)"`） | `ClientOptions.hpp:57`；`McpClient.cpp:1076`、`1086`、`1133-1134` |
| `input_required_config->round_timeout` | `std::chrono::seconds` | `600` | 每轮超时，并与调用方给定超时取较小值 | `ClientOptions.hpp:58`；`McpClient.cpp:1077-1081` |
| `input_required_config->max_total_timeout` | `std::chrono::seconds` | `0`（不限） | 整个多轮流程的硬预算，耗尽抛 `RequestTimeout`（`"MRTR: exceeded max_total_timeout"`） | `ClientOptions.hpp:60`；`McpClient.cpp:1082`、`1088-1097` |

行为要点（履行逻辑在实现内部，用户只见配置与结果）：

- 只有 `resultType == "input_required"` 才进入履行；否则直接返回结果（`McpClient.cpp:986-989`）。
- 缺对应 handler 时**不会**部分履行：`elicitation/create`（非 url 模式）缺 `elicitation_handler`、`sampling/createMessage` 缺 `sampling_handler`、`roots/list` 缺 `roots_handler` 都让本轮履行判定为假，`input_required` 原样回给调用方（`McpClient.cpp:997-1009`、`1113-1130`）。
- URL 模式使用 `url_elicitation_handler` 槽位：**未设该 handler 时不会抛异常，而是直接合成 `action="decline"` 的结果**；只有已设置的 handler 返回非法 `action` 才抛 `HandlerError`（`McpClient.cpp:285-294`、`296-306`）。自动履行的存在性前置检查只门控表单模式（`params.mode != "url"` 且缺 form handler 才判定为不可履行），URL 模式不受该门控（`McpClient.cpp:1000-1003`）；`applyDefaults` 回填只作用于表单模式（`McpClient.cpp:1024-1032`）。
- 仅带 `requestState` 而无 `inputRequests` 的响应按“状态轮”处理：指数退避 50ms→250ms 后重试（`McpClient.cpp:1013-1017`、`1116-1121`）。
- 不认识的 `inputRequests.method` 抛 `HandlerError`（`"unsupported input request method: <method>"`）（`McpClient.cpp:1041-1044`）。
- 会话过期（`SessionExpired`）且 `reinit_on_expired_session=true` 时重新协商并恰好重放一次；关闭该开关则直接抛（`McpClient.cpp:1051-1067`、`1137-1143`）。此恢复**只覆盖 MRTR 路径**，`DoSendRequest` 系（列表类、`Complete`、`GetTask` 等）与裸 `SendRequest` 不享有。

## 被动侧 handler（服务端发起）

| 符号 | 签名/类型 | 默认值/返回 | 说明 | 证据 |
|---|---|---|---|---|
| `SetElicitationHandler` | `void SetElicitationHandler(ElicitationHandler handler)`，`ElicitationHandler = std::function<ElicitResult(const ElicitRequestParams&)>` | 未设则不自动履行 | **返回 `ElicitResult`（不是 void）**；`action` 常用 `"accept"`/`"decline"`/`"cancel"` | `McpClient.hpp:117`；`ClientOptions.hpp:26-27`；`McpClient.cpp:885-887` |
| `SetUrlElicitationHandler` | `void SetUrlElicitationHandler(UrlElicitationHandler handler)`，`UrlElicitationHandler = std::function<ElicitResult(const ElicitRequestParams&)>` | 同上 | 同样返回 `ElicitResult`；`params.url`/`params.elicitation_id` 由此读取 | `McpClient.hpp:118`；`ClientOptions.hpp:29-30`；`McpClient.cpp:889-891` |
| `SetNotificationHandler` | `void SetNotificationHandler(std::string_view method, ClientNotificationHandler handler)`，`ClientNotificationHandler = std::function<void(const JsonRpcNotification&)>` | — | 特例：`method` 为 `subscriptions/acknowledged` 时不进会话处理器，而是登记为用户 ack 回调 | `McpClient.hpp:122-124`、`29-30`；`McpClient.cpp:905-918` |
| `SetLoggingHandler` | `void SetLoggingHandler(std::function<void(const LoggingMessageNotificationParams&)> handler)` | — | 注册即接管 `notifications/message`；handler 抛异常只记内部日志，不影响连接 | `McpClient.hpp:126`；`McpClient.cpp:920-935` |
| `SubscribeAsync` | `void SubscribeAsync(const SubscriptionsListenRequestParams& params = {})` | — | 生成或复用 `_meta` 中的订阅 id，发送 `subscriptions/listen` 并等待 acknowledged 帧（上限 5000ms）；未收到抛 `InternalError`；非 modern 版本由服务端回 `MethodNotFound` | `McpClient.hpp:128`；`McpClient.cpp:1927-1990`、`23` |
| `SetLoggingLevel` | `void SetLoggingLevel(LoggingLevel level)` | — | 发 `logging/setLevel`（60s 超时）；**modern 版本抛 `ProtocolViolation`**，提示改用每请求 `logLevel` | `McpClient.hpp:146`；`McpClient.cpp:1913-1925` |
| `SendRootsListChanged` | `void SendRootsListChanged()` | — | modern 版本抛 `ProtocolViolation`（已移除）；版本低于 `2025-06-18` 同样抛 `ProtocolViolation` | `McpClient.hpp:148`；`McpClient.cpp:1992-2016` |

## 已废弃但仍可调用

以下四项在头文件中带 `[[deprecated]]` 标记：**仍然存在、仍然可调用**，只是新代码不该用（旧文档把它们写成“已移除/不可用”是错的）。

| 符号 | 标记与签名 | 替代方案 | 实际行为 | 证据 |
|---|---|---|---|---|
| `Ping` | `[[deprecated("Ping is deprecated in 2026-07-28 protocol version")]] EmptyResult Ping()` | 用任一轻量 typed 调用或 `GetTask` | 仍可调用，10s 超时，忽略响应内容后返回空结果 | `McpClient.hpp:108-109`；`McpClient.cpp:1822-1830`、`22` |
| `SetSamplingHandler` | `[[deprecated("Use SetElicitationHandler instead (SEP-2577)")]] void SetSamplingHandler(SamplingHandler)` | `SetElicitationHandler`（MRTR 内履行 `sampling/createMessage`） | 仍可调用：装 `sampling/createMessage` 入站处理器，未设 handler 时该项回 `MethodNotFound`（`"sampling not supported"`） | `McpClient.hpp:113-114`；`McpClient.cpp:838-860` |
| `SetRootsHandler` | `[[deprecated("Roots are deprecated in 2026-07-28 (SEP-2577)")]] void SetRootsHandler(RootsHandler)` | `SetElicitationHandler` + `SendRootsListChanged` 不再需要 | 仍可调用：装 `roots/list` 入站处理器，未设时回 `MethodNotFound`（`"roots not supported"`） | `McpClient.hpp:115-116`；`McpClient.cpp:862-883` |
| `SetLegacyUrlElicitationHandler` | `[[deprecated("use ElicitResult-returning handler")]] void SetLegacyUrlElicitationHandler(std::function<void(const ElicitRequestParams&)>)` | `SetUrlElicitationHandler` | 仍可调用：把 void 回调包装成恒返回 `action="accept"` 的 `UrlElicitationHandler` | `McpClient.hpp:119-121`；`McpClient.cpp:893-903` |

## `ClientOptions` 逐字段

| 字段 | 类型 | 默认值 | 说明 / 前置条件 | 证据 |
|---|---|---|---|---|
| `client_info` | `Implementation` | `{"mcp-cpp-client", std::string(kSdkVersion)}` | 版本用符号表述（`kSdkVersion` 定义见 `include/mcp/McpVersion.hpp:7`），不写死数字 | `ClientOptions.hpp:39`；`McpVersion.hpp:7` |
| `capabilities` | `std::optional<ClientCapabilities>` | `nullopt` | 构造期被 `DeriveClientCapabilities()` 的推导结果覆盖写入（见被动 handler 与 4 个 handler 字段） | `ClientOptions.hpp:40`；`McpClient.cpp:611-622`、`360-392` |
| `elicitation_handler` | `std::optional<ElicitationHandler>` | `nullopt` | 等价于构造后立即 `SetElicitationHandler`，并派生 `elicitation.form` 能力 | `ClientOptions.hpp:42`；`McpClient.cpp:620`、`373-377` |
| `url_elicitation_handler` | `std::optional<UrlElicitationHandler>` | `nullopt` | 等价于构造后立即 `SetUrlElicitationHandler`，并派生 `elicitation.url` 能力 | `ClientOptions.hpp:43`；`McpClient.cpp:621`、`378-382` |
| `sampling_handler` | `std::optional<SamplingHandler>` | `nullopt` | 派生 `sampling` 能力并在构造期安装入站处理器；类型已随 SEP-2577 废弃 | `ClientOptions.hpp:44`；`McpClient.cpp:618`、`634`、`383-385` |
| `roots_handler` | `std::optional<RootsHandler>` | `nullopt` | 派生 `roots.list_changed` 能力并安装处理器；类型已废弃 | `ClientOptions.hpp:45`；`McpClient.cpp:619`、`635`、`386-390` |
| `connect_mode` | `ConnectMode` | `ConnectMode::Auto` | 三档语义见下节 | `ClientOptions.hpp:47`；`McpClient.cpp:411-473` |
| `pin_protocol_version` | `std::optional<std::string>` | `nullopt` | 仅 `Pin` 读取；未设时用 `kLatestProtocolVersion` | `ClientOptions.hpp:48`；`McpClient.cpp:429-434` |
| `initialization_timeout` | `std::chrono::seconds` | `60` | 只约束 `initialize` 握手（`Legacy` 与 `Auto` 回退路径） | `ClientOptions.hpp:50`；`McpClient.cpp:415-417`、`461-463`、`587-605` |
| `discover_probe_timeout` | `std::chrono::seconds` | `5` | 只约束 `Auto` 模式的 `server/discover` 探测 | `ClientOptions.hpp:51`；`McpClient.cpp:444-446`、`475-500` |
| `max_total_timeout` | `std::chrono::seconds` | `0`（不限） | 移交会话处理器，作为单请求总预算（含 progress 续期不得越过的期限） | `ClientOptions.hpp:53`；`McpClient.cpp:627` |
| `input_required_config` | `std::optional<InputRequiredConfig>` | `InputRequiredConfig{}`（已置值） | MRTR 行为包，逐项见上表 | `ClientOptions.hpp:55-63` |
| `extensions` | `std::optional<JsonValue>` | `nullopt` | 合入出站 `_meta.clientCapabilities.extensions`；2026 时代要接收 task 结果须含 `"io.modelcontextprotocol/tasks"` | `ClientOptions.hpp:65`；`McpClient.cpp:394-409`、`181-193` |
| `reinit_on_expired_session` | `bool` | `true` | 仅在 MRTR 路径生效（详见该节） | `ClientOptions.hpp:67`；`McpClient.cpp:1059-1066` |
| `span_handler` | `SpanHandler`（`std::function<void(const SpanEvent&)>`） | 空 | 移交会话处理器 | `ClientOptions.hpp:69`；`McpClient.cpp:628-630`；`include/mcp/SpanHooks.hpp:32` |
| `enable_tasks_extension` | `bool` | `false` | 门控 `UpdateTask`/`PollTaskToCompletion`；与 `extensions` 共同决定是否接受 task 结果 | `ClientOptions.hpp:71`；`McpClient.cpp:1751-1753`、`1782-1784`、`1286-1290` |
| `enable_skills_extension` | `bool` | `false` | 门控 `ListSkills`/`GetSkill`/`ReadDirectory` | `ClientOptions.hpp:72`；`McpClient.cpp:1563-1565`、`1597-1599`、`1638-1640` |
| `enable_progressive_discovery` | `bool` | `false` | 决定 `SearchTools` 是否下发 `query`/`pageSize` | `ClientOptions.hpp:73`；`McpClient.cpp:1263-1266` |
| `enable_etag_cache` | `bool` | **`true`** | 与服务端相反；还需进程级 `IsEtagCacheEnabled()` 为真（该开关会被 `ServerOptions::enable_etag_cache` 改写） | `ClientOptions.hpp:75`；`McpClient.cpp:1228`、`1463-1467` |
| `cache_auth_context` | `std::function<std::string()>` | 空 | 返回值变化即清空私有作用域缓存，实现按租户隔离 | `ClientOptions.hpp:77`；`McpClient.cpp:1165-1176` |
| `on_transport_close` | `std::function<void()>` | 空 | 仅当传输为 `TransportBase` 子类才接上，否则记 Warning 后忽略；回调内异常被吞 | `ClientOptions.hpp:79`；`McpClient.cpp:643-661` |
| `on_transport_error` | `std::function<void(std::string_view)>` | 空 | 同上，异常同样被吞 | `ClientOptions.hpp:80`；`McpClient.cpp:662-670` |
| `DeriveClientCapabilities` | `std::optional<ClientCapabilities> DeriveClientCapabilities(const ClientOptions&)`（自由函数） | 全空时 `nullopt` | 公开的推导入口，可用于自检声明集 | `ClientOptions.hpp:84-85`；`McpClient.cpp:360-392` |

## 版本协商与 `ConnectMode`

| 档位 | 取值 | 行为 | 证据 |
|---|---|---|---|
| `Auto` | `ConnectMode::Auto`（默认） | 先 `server/discover` 探测（`discover_probe_timeout`），成功即 modern 直连并按 `SelectSharedVersion` 选版；失败回退 `initialize` 握手 | `ClientOptions.hpp:32-36`、`47`；`McpClient.cpp:444-473` |
| `Legacy` | `ConnectMode::Legacy` | 只走 `initialize` + `notifications/initialized`，`is_modern=false` | `ClientOptions.hpp:34`；`McpClient.cpp:414-427`、`587-609` |
| `Pin` | `ConnectMode::Pin` | 不发包：直接把版本设为 `pin_protocol_version`（未设则 `kLatestProtocolVersion`）并合成一份 `discover` 结果 | `ClientOptions.hpp:35`、`48`；`McpClient.cpp:429-442` |

`Auto` 的探测失败按传输分类处理（`include/mcp/client/ClientOptions.hpp:32-36`、`src/client/McpClient.cpp:174-179`、`475-590`）：

- **stdio 类**（RTTI 名字命中 `InMemoryTransportImpl` / `StdioClientSessionTransport`）：探测超时或异常一律视为“对端不支持 discover”，静默回退 `initialize`。
- **网络类**：探测超时抛 `RequestTimeout`（`"server/discover timed out"`），任何异常抛 `ConnectionClosed`（`"server/discover failed: …"`），不回退。
- 收到 `-32022`（`UnsupportedProtocolVersion`）且 `data.supported` 含 `kLatestProtocolVersion` 时，做一次 corrective 重试；重试再被拒绝即为硬错误（`McpError(UnsupportedProtocolVersion)`）。
- 收到 `-32022` 且 supported 不含最新版本但含其他 modern 版本时，直接抛 `UnsupportedProtocolVersion`；supported 全无 modern 版本时回退 `initialize`。
- `discover` 应答解析失败或与客户端声明集无交集，同样按“探测失败”处理（`McpClient.cpp:576-584`）。

## 错误语义与超时归属

| 场景 | 表现 | 证据 |
|---|---|---|
| typed 方法（`DoSendRequest` 路径：列表类、`Complete`、`GetTask`/`UpdateTask`/`CancelTask`、`Ping`、`Discover`、`SetLoggingLevel`）收到协议错误 | 抛 `McpError(code, message)`，**错误 `data` 被丢弃** | `McpClient.cpp:62-90` |
| MRTR 路径（`CallTool`、`GetPrompt`、`ReadResource`、`GetSkill`、`ReadDirectory`、`CallToolAsTask`）收到协议错误 | 抛 `McpError(code, message, data)`，保留 `data` | `McpClient.cpp:1101-1108` |
| 裸 `SendRequest`（`RequestHandle` 重载）收到协议错误 | **不抛**，`future` 给出 `RequestOutcome` 中的 `ErrorData` | `McpClient.cpp:1853-1864` |
| `CallTool` 得到 task 句柄 | 自动 `PollTaskToCompletion`；`failed`/`cancelled` 转抛 `InternalError`；modern 时代未声明 tasks 扩展则抛 `HandlerError` | `McpClient.cpp:1280-1310` |
| 分页不收敛 | `ListToolsAll`/`SearchTools`/`ListResourcesAll`/`ListResourceTemplatesAll`/`ListPromptsAll` 超 64 页抛 `ProtocolViolation`（`"list pagination did not converge within 64 pages"`） | `McpClient.cpp:52`、`143-171`、`1240-1249` |
| 扩展开关缺失 | `ListSkills`/`GetSkill`/`ReadDirectory` 抛 `MethodNotFound`；`UpdateTask`/`PollTaskToCompletion` 同样抛 `MethodNotFound` | `McpClient.cpp:1563`、`1597`、`1638`、`1751`、`1782` |
| 超时来源 | typed 方法**不读** `RequestOptions::timeout`：`CallTool`/`CallToolAsTask` 用 `input_required_config->round_timeout`（默认 600s），`GetPrompt`/`ReadResource`/`GetSkill`/`ReadDirectory` 用 `read_timeout_ms` 或 60s，列表类与 `Complete`/`Discover` 用 60s，`GetTask`/`UpdateTask`/`CancelTask` 用 600s，`Ping` 用 10s | `McpClient.cpp:21-22`、`1335-1342`、`1546-1550`、`1704-1708`、`1815-1835` |

::: tip
需要严格单请求超时或拿 `ErrorData` 时，用 `SendRequest(method, params, RequestOptions{...})` 句柄重载；typed 方法的超时语义按上表固定，无法由 `RequestOptions::timeout` 覆盖。
:::

## 技能校验辅助函数

公开自由函数，供客户端在采纳技能前后做一致性核验（`include/mcp/client/SkillVerify.hpp`）：

| 符号 | 签名/类型 | 默认值/返回 | 说明 | 证据 |
|---|---|---|---|---|
| `kMaxSkillFiles` | `constexpr size_t` | `512` | 单技能文件数上限 | `SkillVerify.hpp:12` |
| `kMaxSkillTotalBytes` | `constexpr int64_t` | `16 * 1024 * 1024` | 单技能总字节上限 | `SkillVerify.hpp:13` |
| `CheckSkillLimits` | `bool CheckSkillLimits(const Skill&, std::string& error_out)` | `false` + 原因 | 校验文件数与总大小 | `SkillVerify.hpp:15` |
| `VerifyFileBytes` | `bool VerifyFileBytes(const SkillResource&, std::string_view bytes, std::string& error_out)` | `false` + 原因 | 比对摘要与长度 | `SkillVerify.hpp:16` |
| `VerifyFrontmatter` | `bool VerifyFrontmatter(const Skill& advertised, const JsonValue& fetched, std::string& error_out)` | `false` + 原因 | 比对广告与实取 frontmatter | `SkillVerify.hpp:17` |
| `IsApprovalStillValid` | `bool IsApprovalStillValid(const Skill& approved, const Skill& current)` | `bool` | 已批准的技能是否仍然有效 | `SkillVerify.hpp:18` |
| `ApprovalKey` | `std::string ApprovalKey(const std::string& host_label, const Skill&)` | 键串 | 审批记录的键 | `SkillVerify.hpp:19` |

## 内部符号（非用户 API，勿当接口引用）

- `SendRequestWithMrtr` / `SendRequestWithMrtrOnce` / `RecoverExpiredSession` / `WireClientHandlers` / `CacheIfHinted` / `MaybeAttachEtag` / `NegotiationResult negotiation_` 均为 `McpClient` 私有成员（`McpClient.hpp:152-226`）；MRTR 对用户只体现为 `ClientOptions::input_required_config` 与返回的 `input_required` 结果。
- `VersionNegotiation::Negotiate/ProbeDiscover/HandshakeInitialize` 与 `NegotiationResult` 在公开头（`include/mcp/client/VersionNegotiation.hpp:9-36`）中声明，但属协商机制内部组件，用户只应通过 `ConnectMode` 与超时字段影响其行为。
- `detail::ResponseCache`、`detail::Base64Url`、`detail::JoinThreadSafely` 等 `detail` 命名空间符号不是公共契约，文档与代码不得把它们当用户 API 引用。
- `kElicitTimeout`、`kMaxListPages`、`kMrtrStateOnlyBackoff*`、`kSubscriptionAckTimeout` 等常量位于 `src/` 匿名 namespace，仅在行为说明中作为“实现内部值”引用。

相关任务写法：[/zh/howto/connect-to-a-server](/zh/howto/connect-to-a-server)、[/zh/howto/call-tools-and-read-resources](/zh/howto/call-tools-and-read-resources)、[/zh/howto/cancel-a-request](/zh/howto/cancel-a-request)。

## 本页对应源码

- `include/mcp/client/McpClient.hpp`
- `include/mcp/client/ClientOptions.hpp`
- `include/mcp/client/RequestHandle.hpp`
- `include/mcp/client/VersionNegotiation.hpp`
- `include/mcp/client/SkillVerify.hpp`
- `include/mcp/protocol/IncomingRequestMeta.hpp`（`RequestOutcome`、`IncomingRequestMeta`）
- `include/mcp/McpTypes.hpp`（`RequestOptions`、`CacheableRequestOptions`、各 `*Result`/`*RequestParams`）
- `include/mcp/Meta.hpp`（`RequestMeta`）、`include/mcp/JsonRpc.hpp`（`RequestId`、`ErrorData`）
- `include/mcp/Capabilities.hpp`、`include/mcp/SpanHooks.hpp`、`include/mcp/McpVersion.hpp`、`include/mcp/ProtocolVersion.hpp`
- `src/client/McpClient.cpp`
