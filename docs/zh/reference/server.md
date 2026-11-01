# 服务端门面（`McpServer`）参考

本页是 `McpServer`、`RequestContext<TParams>`、`ServerOptions` 的**唯一权威签名与字段表**；端到端用法与示例见后续 how-to 页面，how-to 页只链接本页、不重复表格。协议语义见 `/zh/learn/protocol-versions`，数据类型见 `/zh/reference/types`，错误码见 `/zh/reference/errors`，传输构造见 `/zh/reference/transports`。

## 工厂与生命周期

| 符号 | 签名/类型 | 默认值/返回 | 说明 | 证据 |
|---|---|---|---|---|
| `McpServer::Create` | `static std::unique_ptr<McpServer> Create(std::shared_ptr<ITransport> transport, const ServerOptions& options = {})` | `std::unique_ptr<McpServer>` | 构造即接线入站处理器、推导能力、写入协商版本、`transport->Start()` 并启动消息线程；返回后无需 `Run()` 即可收发 | `include/mcp/server/McpServer.hpp:34-36`；`src/server/McpServer.cpp:646-652`、`654-735` |
| `Run` | `void Run()` | — | 在条件变量上阻塞直到 `Close()` 把 `running_` 置假；消息处理由会话线程完成，`Run()` 只是主线程挂起点 | `McpServer.hpp:40`；`McpServer.cpp:737-740` |
| `Close` | `void Close()` | — | 先等待全部在途异步调用（含 URL elicitation watchdog），再关闭会话处理器，最后唤醒 `Run()` | `McpServer.hpp:41`；`McpServer.cpp:742-756` |
| `IsRunning` | `bool IsRunning() const` | 首次 `initialize`/`server/discover` 前为 `false` | 等于「已完成握手且处理器在跑」；注册 API 用它决定是否补发 `*/list_changed` 通知 | `McpServer.hpp:117`；`McpServer.cpp:3357-3359`、`779-781` |
| `~McpServer` | `virtual ~McpServer() = default` | — | 虚析构，可经 `unique_ptr` 安全销毁；销毁不等价于 `Close()` | `McpServer.hpp:38` |

::: warning
传输对象由调用方持有：`Create()` 只存 `shared_ptr`，`Close()` 只关会话处理器，不会析构传输。服务端不暴露 `Connect()`，客户端侧才需要工厂连接。
:::

## 注册 API

| 符号 | 签名/类型 | 默认值/返回 | 说明 | 证据 |
|---|---|---|---|---|
| `RegisterTool`（对象路径） | `void RegisterTool(std::shared_ptr<McpServerTool> tool)` | `void` | 入表 `tools_`（按名有序）并失效工具列表缓存；成功后重推导能力 | `McpServer.hpp:43`；`McpServer.cpp:758-782` |
| `RegisterTool`（lambda 重载） | `void RegisterTool(std::string_view name, const ToolOptions& options, std::function<CallToolResult(const RequestContext<CallToolRequestParams>&)> fn)` | `void` | 内联转调 `McpServerTool::Create(name, std::move(fn), options)`，行为与对象路径完全一致 | `McpServer.hpp:44-50` |
| `McpServerTool::Create` | `template <typename Callable> static std::shared_ptr<McpServerTool> Create(std::string_view name, Callable&& callable, const ToolOptions& options = {})` | `shared_ptr<McpServerTool>` | 生成 `McpServerToolImpl<std::decay_t<Callable>>`；`options.input_schema` 缺省时补 `{"type":"object","properties":{}}` | `include/mcp/server/McpServerTool.hpp:23-27`、`46-60`、`92-102` |
| `McpServerTool` | `virtual const Tool& ProtocolTool() const` / `virtual void InvokeAsync(const RequestContext<CallToolRequestParams>&, std::promise<CallToolResult>)` | 纯虚 | 自定义执行体的扩展点；`McpServerToolImpl::InvokeAsync` 直接在调用线程同步执行 callable，异常经 promise 转成 `exception_ptr` | `McpServerTool.hpp:18-21`、`75-85` |
| `RegisterResource` | `void RegisterResource(std::string_view name, std::string_view uri, const ResourceOptions&, std::function<ReadResourceResult(const std::string& uri)> handler)` | `void` | 静态资源；handler 收实际 uri | `McpServer.hpp:55-59`；`McpServer.cpp:784-822` |
| `RegisterResourceTemplate` | `void RegisterResourceTemplate(std::string_view name, std::string_view uri_template, const ResourceOptions&, std::function<ReadResourceResult(const std::string& uri, const std::map<std::string, std::string>& vars)> handler)` | `void` | URI 模板在注册期即解析校验；handler 额外收已展开的变量表 | `McpServer.hpp:61-67`；`McpServer.cpp:824-871` |
| `RegisterPrompt` | `void RegisterPrompt(std::string_view name, const PromptOptions&, std::function<GetPromptResult(const std::string& name, const std::optional<JsonValue>& args)> handler)` | `void` | `PromptOptions::arguments` 原样进 `prompts/list` | `McpServer.hpp:69-73`；`McpServer.cpp:873-906` |
| `RegisterSkill` | `void RegisterSkill(Skill skill)` | `void` | 按 `Skill::uri` 去重；能力声明还要求 `enable_skills_extension` 与已具备 `resources` 能力；不发 `list_changed` | `McpServer.hpp:75`；`McpServer.cpp:908-925`、`1983-1990` |
| `SetSkillsDirectoryRead` | `void SetSkillsDirectoryRead(bool enabled)` | `void`（默认 `false`） | 置技能目录浏览开关，写入 skills 扩展设置 `directory_read`，影响 `skills/directory` 可用集合 | `McpServer.hpp:76`；`McpServer.cpp:927-933`、`1987` |
| `SetCompletionHandler` | `using CompletionHandler = std::function<CompleteResult(const CompleteRequestParams&)>; void SetCompletionHandler(CompletionHandler handler)` | `void` | 未注册时 `completion/complete` 回 `MethodNotFound`（`"No completion handler registered"`）；能力声明只由 `declare_completions` 决定 | `McpServer.hpp:86-87`；`McpServer.cpp:3089-3096`、`3111-3114`、`1962-1964` |

## 注册期校验与错误

| 触发条件 | 抛出 | 消息/依据 | 证据 |
|---|---|---|---|
| 工具名为空、超 128 字符，或含 `[A-Za-z0-9._-]` 之外字符 | `McpError(InvalidParams)` | `invalid tool name: <name>` | `McpServer.cpp:760-763`、`275-283` |
| `ToolOptions::input_schema` 的 `x-mcp-header` 参数注解非法 | `McpError(InvalidParams)` | 解析器给出的 `invalid_reason` | `McpServer.cpp:764-767` |
| 同名工具重复注册 | `McpError(InvalidParams)` | `tool already registered: <name>` | `McpServer.cpp:770-773` |
| 传入空 `shared_ptr<McpServerTool>` | 不抛，直接解引用空指针 | 实现无 null 检查（`tool->ProtocolTool()`） | `McpServer.cpp:758-759` |
| 资源/模板/提示/技能 handler 为空 | `McpError(InvalidParams)` | `resource handler must not be empty: <name>`、`resource template handler must not be empty: <name>`、`prompt handler must not be empty: <name>` | `McpServer.cpp:790-793`、`832-835`、`880-883` |
| URI 模板语法非法 | `McpError(InvalidParams)` | `invalid resource template '<uri>': <error>` | `McpServer.cpp:836-841` |
| 资源名或 uri_pattern（含模板）与既有资源冲突 | `McpError(InvalidParams)` | `resource already registered: <name>` / `resource uri already registered: <uri>`；静态资源与模板共用同一张冲突表 | `McpServer.cpp:805-814`、`854-863` |
| 提示名重复 | `McpError(InvalidParams)` | `prompt already registered: <name>` | `McpServer.cpp:893-898` |
| 技能 uri 不合法 | `McpError(InvalidParams)` | `invalid skill uri: <uri>`；规则为必须以 `/SKILL.md` 结尾且具 `scheme://host/…` 结构 | `McpServer.cpp:909-912`、`125-139` |
| 技能 uri 重复 | `McpError(InvalidParams)` | `skill already registered: <uri>` | `McpServer.cpp:915-919` |
| 补全 handler 为空 | `McpError(InvalidParams)` | `completion handler must not be empty` | `McpServer.cpp:3089-3093` |

## 反向请求：`Elicit` / `ElicitUrl`

| 符号 | 签名/类型 | 默认值/返回 | 说明 | 证据 |
|---|---|---|---|---|
| `Elicit` | `std::future<ElicitResult> Elicit(const ElicitRequestParams& params, std::string_view session_id = {})` | `future<ElicitResult>` | 表单模式反向请求；超时取 `options.input_required_config->round_timeout`，未配置时 600s | `McpServer.hpp:78-79`；`McpServer.cpp:1138-1141`、`1173-1177`、`48` |
| `ElicitUrl` | `std::future<ElicitResult> ElicitUrl(const std::string& url, const std::string& message, std::chrono::seconds timeout = std::chrono::seconds(600), std::string_view session_id = {})` | `future<ElicitResult>` | URL 模式（SEP-1034）；legacy 时代自增 `elicitationId`（`elicit-<n>`）并等 `notifications/elicitation/complete`，watchdog 以 `RequestTimeout` 结算 | `McpServer.hpp:80-83`；`McpServer.cpp:1193-1214`、`1246-1279` |

会话尾参（两者一致）由 `EffectiveSessionId()` 解析，顺序为：显式传入的非空 `session_id` > 全局能力已设置（返回空串，走全局）> `client_sessions_` 恰有一个非空会话时回退该会话 > 其余情况返回空串（即读不到按会话能力）。stateful HTTP 下能力只落在 `client_sessions_`，无参调用会落到「全局为空」分支而误抛 `-32021`，因此工具 handler 内应传 `RequestContext::SessionId()`。证据：`src/server/McpServer.cpp:1060-1068`、`1140`、`1198`。

协议时代差异（重要）：

- **modern 时代**（协商版本 `>= 2026-07-28`）：两者**不发送** `elicitation/create`，而是抛出实现内部异常 `ElicitationInputRequired`，携带 `inputRequests["elicitation"]`；该异常由 `HandleCallTool` 三条路径捕获并转成 `CallToolResult::input_required`，由客户端 `SetElicitationHandler` / `SetUrlElicitationHandler` 履行。`ElicitUrl` 的 `timeout` 实参在此分支被忽略。证据：`McpServer.cpp:1142-1149`、`1199-1212`、`2630-2632`。
- **legacy 时代**：先校验客户端能力，缺失即抛 `McpError(MissingRequiredClientCapability)` 且 `data.requiredCapabilities.elicitation` 带 `form`/`url` 空对象。表单模式的「已声明」判定为 `elicitation.form` 存在 **或** 未声明 `url`；URL 模式要求 `elicitation.url` 存在。证据：`McpServer.cpp:1151-1167`。
- 响应里带已知 JSON-RPC 错误码时，`Elicit` 的 future 以 `McpError(code, message)` 结算（`McpServer.cpp:1179-1188`）。

::: warning
`ElicitationInputRequired` 只存在于 `src/server/McpServer.cpp:100-105` 的匿名 namespace，未在任何导出头中出现：用户代码不得按名字捕获它，只能观察工具结果的 `input_required` 字段。
:::

## 通知与推送 API

| 符号 | 签名/类型 | 默认值/返回 | 说明 | 证据 |
|---|---|---|---|---|
| `SendToolListChanged` | `void SendToolListChanged()` | — | 转 `SendListChangedNotification(tools/list_changed)` | `McpServer.hpp:89`；`McpServer.cpp:935-937` |
| `SendResourceListChanged` | `void SendResourceListChanged()` | — | 同上，`resources/list_changed` | `McpServer.hpp:90`；`McpServer.cpp:939-941` |
| `SendPromptListChanged` | `void SendPromptListChanged()` | — | 同上，`prompts/list_changed` | `McpServer.hpp:92`；`McpServer.cpp:949-951` |
| `SendResourceUpdated` | `void SendResourceUpdated(const std::string& uri)` | — | 只经 `NotifySubscribers` 投递给订阅了该 uri 的订阅者；modern 专用 | `McpServer.hpp:91`；`McpServer.cpp:943-947` |
| （路由规则） | `SendListChangedNotification` 内部 | — | modern 时代 → `NotifySubscribers`；legacy 时代 → `SendNotification` 广播 | `McpServer.cpp:953-959` |
| `SendLoggingMessage` | `void SendLoggingMessage(LoggingLevel level, std::string_view data)` | — | **modern 时代直接返回不发送**；legacy 时代按 `logging/setLevel` 记录的级别过滤 | `McpServer.hpp:93`；`McpServer.cpp:961-972` |
| `SendLoggingMessage`（重载） | `void SendLoggingMessage(LoggingLevel level, std::string_view data, std::optional<LoggingLevel> min_level)` | `min_level = nullopt` | 给了 `min_level` 就只按它过滤并发送（可越过 `setLevel` 记录）；`nullopt` 时行为回落到二参版本 | `McpServer.hpp:94`；`McpServer.cpp:974-982` |
| `SendTaskStatus` | `void SendTaskStatus(std::string_view task_id, TaskStatus status)` | — | 从 `task_store` 反查 owner 会话，再按该会话的协商版本决定订阅路由或广播；须配 `enable_tasks_extension` + `task_store` 才有可用任务 | `McpServer.hpp:96`；`McpServer.cpp:1004-1012`、`285-303` |
| `SendProgress` | `void SendProgress(const ProgressToken& token, double progress, std::optional<double> total = std::nullopt, std::optional<std::string> message = std::nullopt)` | — | modern 时代先尝试写入请求流缓冲（`BufferRequestStreamNotification`），失败则发全局 `notifications/progress` | `McpServer.hpp:97-99`；`McpServer.cpp:1090-1110` |
| `SetCacheable` | `void SetCacheable(std::function<std::optional<CacheHint>(std::string_view method)> resolver)` | — | 运行时替换缓存钩子，优先于 `ServerOptions::cache_hints`；modern 时代仍校验 `ttlMs`/`cacheScope` | `McpServer.hpp:95`；`McpServer.cpp:640-644`、`594-621` |

`TaskStatus` 取值：`Pending`、`Working`、`Completed`、`Failed`、`Cancelled`、`InputRequired`（`include/mcp/server/McpTaskStore.hpp:16-23`）。

## 客户端状态查询

| 符号 | 签名/类型 | 默认值/返回 | 说明 | 证据 |
|---|---|---|---|---|
| `GetClientCapabilities` | `std::shared_ptr<const ClientCapabilities> GetClientCapabilities() const` | 未握手为 `nullptr` | 全局槽位；`initialize`（空会话 id）与 modern 的 `server/discover`（`_meta.clientCapabilities`）都会写入 | `McpServer.hpp:106`；`McpServer.cpp:3296-3299`、`3190-3201`、`3150-3157` |
| `GetClientCapabilities`（按会话） | `GetClientCapabilities(std::string_view session_id) const` | 未知会话为 `nullptr` | 精确查 `client_sessions_`，**不做** `EffectiveSessionId` 回退 | `McpServer.hpp:107-108`；`McpServer.cpp:3301-3304`、`1028-1035` |
| `GetClientInfo` / `GetClientInfo(session_id)` | `std::shared_ptr<const Implementation>` | 未握手为 `nullptr` | 全局槽位只由 `initialize` 写入；modern 直连（discover）不带 `clientInfo` 时仍为 `nullptr`，需从 `RequestContext` 的 `_meta` 读 | `McpServer.hpp:109-111`；`McpServer.cpp:3306-3314`、`3194-3200` |
| `GetCapabilities` | `ServerCapabilities GetCapabilities() const` | **按值返回** | 锁内拷贝推导结果，调用方不得持引用；能力无手工赋值入口 | `McpServer.hpp:114`；`McpServer.cpp:3325-3328` |
| `GetNegotiatedProtocolVersion` | `std::string GetNegotiatedProtocolVersion() const` / `(std::string_view session_id) const` | 未协商且未设 `protocol_version` 时返回**空串** | 按会话重载在会话未知时回退全局版本；`IsModernProtocolVersion("")` 为假，故空串按 legacy 处理。legacy `initialize` 未声明版本 → `kDefaultNegotiatedProtocolVersion`（`2025-03-26`）；声明了但不在支持表或属 modern → `kLegacyProtocolVersion`（`2025-11-25`） | `McpServer.hpp:112-113`；`McpServer.cpp:3316-3324`、`3213-3223`；`src/protocol/McpSessionHandler.cpp:681-689` |
| `IsMrtrSupported` | `bool IsMrtrSupported() const` | `false` | 非 stateless 且 `EffectiveSessionId({})` 解析到的会话声明了 `elicitation` | `McpServer.hpp:116`；`McpServer.cpp:3351-3355` |
| `GetServerCard` | `ext::ServerCard GetServerCard() const` | 值 | `enable_server_card=false` 时抛 `McpError(HandlerError, "server card is disabled")`；未给 `server_card` 时按 `server_info`/`kSdkVersion`/全版本表合成，端点为占位 URL | `McpServer.hpp:115`；`McpServer.cpp:3330-3349` |
| `SessionId` | `const std::string& SessionId() const` | 构造期随机 8 字节 hex | **实例级**标识，仅用于任务 owner 锚定：任务 id 形如 `task-<session>-<16 字节 hex>` | `McpServer.hpp:118`；`McpServer.cpp:654-660`、`1015-1017` |
| `RequestContext::SessionId` | `std::string_view SessionId() const` | stdio / stateless HTTP 为空串 | **请求级** MCP 会话 id（stateful HTTP 为 `mcp-session-id`） | `include/mcp/server/RequestContext.hpp:62`；`src/server/McpServer.cpp:2317-2319` |
| `GetSessionHandler` | `McpSessionHandler& GetSessionHandler()` | 引用 | 逃生口，直取会话处理器；常规场景不需要 | `McpServer.hpp:120` |

::: danger
`McpServer::SessionId()` 与 `RequestContext::SessionId()` 语义无关，禁止混用：前者是进程内实例随机标识（任务 owner），后者是当次请求的协议会话。给 `Elicit`/`ElicitUrl` 传 `McpServer::SessionId()` 会解析到错误会话能力。
:::

## `RequestContext<TParams>` 成员

| 符号 | 签名/类型 | 默认值/返回 | 说明 | 证据 |
|---|---|---|---|---|
| `Params` | `const TParams& Params() const` | 值引用 | 反序列化后的请求参数（工具路径为 `CallToolRequestParams`） | `RequestContext.hpp:58` |
| `Server` | `McpServer& Server() const` | 引用 | 反向调用门面（`Elicit`、`SendProgress` 等）的入口 | `RequestContext.hpp:59` |
| `GetRequest` | `const mcp::JsonRpcRequest& GetRequest() const` | 引用 | 原始 JSON-RPC 请求，含 `id`/`method`/`params`/`meta` | `RequestContext.hpp:60` |
| `LogLevel` | `std::optional<LoggingLevel> LogLevel() const` | 缺 `_meta.logLevel` 为 `nullopt` | 取自 `_meta["io.modelcontextprotocol/logLevel"]`，整数或字符串形式均可解析，解析失败静默忽略 | `RequestContext.hpp:43-55`、`61` |
| `SessionId` | `std::string_view SessionId() const` | 空串 | 请求级会话 id，内部持 `std::string` 拷贝，异步 handler 中仍可安全读取 | `RequestContext.hpp:62`、`84` |
| `Log` | `void Log(LoggingLevel level, std::string_view data) const` | — | 两级门控：要求 `logLevel` 但未提供时丢弃；低于 `LogLevel()` 时丢弃 | `RequestContext.hpp:64-70` |
| `IsCancellationRequested` | `bool IsCancellationRequested() const` | 无标志时 `false` | 协作取消；`notifications/cancelled` 与 `tasks/cancel` 置位同一标志，普通工具 handler 同样可轮询 | `RequestContext.hpp:72-74`；`McpServer.cpp:2313-2319` |

`using LogFn = std::function<void(LoggingLevel, std::string_view)>`（`RequestContext.hpp:25`）。构造器虽公开，仅供 SDK 在 `HandleCallTool` 等处注入 `log_fn`、取消标志、`require_request_log_level`（modern 时代为真）与会话 id；用户代码不得自建 `RequestContext`，应经 `Params()` 等访问器取用（`RequestContext.hpp:27-41`）。

## `ServerOptions` 逐字段

| 字段 | 类型 | 默认值 | 说明 / 前置条件 | 证据 |
|---|---|---|---|---|
| `server_info` | `std::optional<Implementation>` | `nullopt` → `{"mcp-server", kSdkVersion}` | `initialize`/`discover` 回体的服务端标识 | `ServerOptions.hpp:30`；`McpServer.cpp:635-638` |
| `protocol_version` | `std::optional<std::string>` | `nullopt` | 固定线路编解码版本并覆盖协商结果（每会话写入）；不改变能力集合 | `ServerOptions.hpp:31`；`McpServer.cpp:662-663`、`682-684`、`3159-3165`、`3209-3211` |
| `server_instructions` | `std::optional<std::string>` | `nullopt` | 随 `initialize`/`discover` 下发 `instructions` | `ServerOptions.hpp:32`；`McpServer.cpp:3230-3232`、`3174-3176` |
| `initialization_timeout` | `std::chrono::seconds` | `60` | **服务端实现当前无任何读取点**（仅客户端同名项生效）；不要依赖它约束握手 | `ServerOptions.hpp:34`；`src/server` 全量检索无引用 |
| `cache_hints` | `std::optional<std::map<std::string, CacheHint, std::less<>>>` | `nullopt` | 按方法静态映射 `ttlMs`/`cacheScope`；modern 时代缺字段或非法值抛 `InvalidParams` | `ServerOptions.hpp:37`；`McpServer.cpp:608-614`、`66-88` |
| `cache_hint_resolver` | `std::function<std::optional<CacheHint>(std::string_view method)>` | 空 | 优先级高于 `cache_hints`，可被 `SetCacheable` 运行时替换 | `ServerOptions.hpp:76`；`McpServer.cpp:677-680`、`598-607` |
| `request_state_verifier` | `std::function<bool(std::string_view)>` | 空 | MRTR `requestState` 校验器；给了它就不再用 `request_state_key` 内置校验 | `ServerOptions.hpp:39`；`McpServer.cpp:686-687` |
| `request_state_key` | `std::optional<std::string>` | `nullopt` | HMAC 密钥：置值即接内置校验器，并在回 `input_required` 时签入 `iat` | `ServerOptions.hpp:41`；`McpServer.cpp:688-696`、`2403-2410`、`2661-2667` |
| `request_state_ttl` | `std::chrono::seconds` | `0`（不校验过期） | 仅随内置校验器生效，比对载荷 `iat` | `ServerOptions.hpp:43`；`McpServer.cpp:690-695` |
| `input_required_config` | `std::optional<InputRequiredConfig>` | `nullopt` | MRTR/elicitation 参数包 | `ServerOptions.hpp:45-49` |
| `input_required_config->max_rounds` | `int` | `10` | **服务端实现无读取点**（轮次上限只在客户端 MRTR 循环生效） | `ServerOptions.hpp:46`；`src/server` 无引用 |
| `input_required_config->round_timeout` | `std::chrono::seconds` | `600` | 用作 `Elicit` 的等待超时；配置存在时覆盖默认 600s | `ServerOptions.hpp:47`；`McpServer.cpp:1173-1177` |
| `declare_logging` | `bool` | `false` | 即使尚无 handler 也显式声明 `logging` 能力 | `ServerOptions.hpp:51`；`McpServer.cpp:1959-1961` |
| `declare_completions` | `bool` | `false` | 显式声明 `completions` 能力（与是否注册 handler 无关） | `ServerOptions.hpp:52`；`McpServer.cpp:1962-1964` |
| `on_method_called` | `std::function<void(std::string_view method)>` | 空 | 与 `on_request` 链式同时触发 | `ServerOptions.hpp:54`；`McpServer.cpp:698-704` |
| `on_client_connected` | `std::function<void(const Implementation& client_info)>` | 空 | `initialize` 解析出 `clientInfo` 后触发 | `ServerOptions.hpp:55`；`McpServer.cpp:3204-3206` |
| `on_initialized` | `std::function<void()>` | 空 | 首次收到该会话的 `notifications/initialized` 时触发（重复通知不再触发），回调内异常被吞并记日志 | `ServerOptions.hpp:56`；`McpServer.cpp:1414-1424` |
| `on_protocol_error` | `std::function<void(std::string_view error)>` | 空 | 与 `on_error` 链式同时触发，只收错误消息 | `ServerOptions.hpp:57`；`McpServer.cpp:708-714` |
| `on_request` | `std::function<void(std::string_view method, const JsonRpcRequest&)>` | 空 | 每个入站请求 | `ServerOptions.hpp:59`；`McpServer.cpp:698-704` |
| `on_response` | `std::function<void(const JsonRpcResponse&)>` | 空 | 每个出站成功响应 | `ServerOptions.hpp:60`；`McpServer.cpp:705-707` |
| `on_error` | `std::function<void(const JsonRpcErrorResponse&)>` | 空 | 每个出站错误响应 | `ServerOptions.hpp:61`；`McpServer.cpp:708-714` |
| `on_notification` | `std::function<void(const JsonRpcNotification&)>` | 空 | 每个入站通知 | `ServerOptions.hpp:62`；`McpServer.cpp:715-717` |
| `span_handler` | `SpanHandler`（`std::function<void(const SpanEvent&)>`） | 空 | 观测钩子，移交会话处理器 | `ServerOptions.hpp:64`；`include/mcp/SpanHooks.hpp:32`、`McpServer.cpp:718-720` |
| `incoming_filters` / `outgoing_filters` | `std::shared_ptr<FilterPipeline>` | `nullptr` | 入站/出站消息管道，建 handler 时一次性注入 | `ServerOptions.hpp:66-67`；`McpServer.cpp:664-667` |
| `on_transport_close` | `std::function<void()>` | 空 | 仅当传输是 `TransportBase` 子类才接上（`dynamic_cast` 失败即静默忽略） | `ServerOptions.hpp:69`；`McpServer.cpp:721-728` |
| `on_transport_error` | `std::function<void(std::string_view)>` | 空 | 同上 | `ServerOptions.hpp:70`；`McpServer.cpp:721-728` |
| `max_tool_input_elements` | `std::optional<std::size_t>` | `nullopt`（不限） | 对象按键数、数组按长度、标量按 1 计；超限回 `is_error` 工具结果而非协议错误 | `ServerOptions.hpp:72`；`McpServer.cpp:2323-2328`、`563-568` |
| `supported_protocol_versions` | `std::optional<std::vector<std::string>>` | `nullopt`/空 → 全表 `kProtocolVersions` | 同时收窄 `discover` 应答、`initialize` 候选集与错误 `data.supported` | `ServerOptions.hpp:74`；`McpServer.cpp:624-633`、`676`、`3216-3221` |
| `subscriptions_enabled` | `bool` | `true` | 置 `false` 时 `subscriptions/listen` 回 `MethodNotFound`（`"subscriptions are disabled"`）；非 modern 版本本就回 `MethodNotFound` | `ServerOptions.hpp:75`；`McpServer.cpp:3239-3249` |
| `task_store` | `std::shared_ptr<IMcpTaskStore>` | `nullptr` | 任务化执行的持久化后端；缺省即不产生任务句柄 | `ServerOptions.hpp:78`；`McpServer.cpp:2342-2348`、`2414` |
| `enable_tasks_extension` | `bool` | `false` | 声明扩展并放行 `tasks/*`；`task_store` 存在但此开关为假时 `tasks/*` 被拒 | `ServerOptions.hpp:80`；`McpServer.cpp:1965-1970`、`1514`、`1556`、`1796`、`2347-2348` |
| `enable_skills_extension` | `bool` | `false` | 放行 `skills/list`、`skills/get`、`skills/directory`（否则 `MethodNotFound`） | `ServerOptions.hpp:81`；`McpServer.cpp:1983-1990`、`2954`、`2995`、`3038` |
| `enable_apps_extension` | `bool` | `false` | 声明 apps 扩展并放行 apps 相关请求 | `ServerOptions.hpp:82`；`McpServer.cpp:1971-1976`、`2766` |
| `enable_server_card` | `bool` | `false` | 声明 server-card 扩展，`GetServerCard()` 才可用 | `ServerOptions.hpp:83`；`McpServer.cpp:1977-1982`、`3330-3333` |
| `server_card` | `std::optional<ext::ServerCard>` | `nullopt` | 提供则原样校验后返回；否则合成默认卡片 | `ServerOptions.hpp:84`；`McpServer.cpp:3335-3348` |
| `enable_etag_cache` | `bool` | `false` | **进程级副作用**：构造期写全局 `SetEtagCacheEnabled`，同进程内其他客户端/服务端的 etag 行为一并受影响 | `ServerOptions.hpp:85`；`McpServer.cpp:661`、`include/mcp/McpTypes.hpp:263-264` |
| `enable_progressive_discovery` | `bool` | `false` | 关闭时 `tools/list` 走整表缓存并忽略 `query`/`pageSize`；开启后按页与关键字过滤 | `ServerOptions.hpp:86`；`McpServer.cpp:2014-2043` |
| `enable_file_input` | `bool` | `false` | 展开 `arguments` 中的 `FileRef`（`file://`、`resource://`）；单片上限 8MB、内联 256KB（常量 `kFileInputMaxBytes`/`kFileInputInlineMaxBytes`） | `ServerOptions.hpp:87`；`McpServer.cpp:2153-2212`、`503-504`、`527-547` |
| `enable_triggers_events` | `bool` | `false` | `EmitTriggerEvent` 在此为假时静默返回（注册/注销不受控） | `ServerOptions.hpp:88`；`McpServer.cpp:1122-1136` |
| `TasksImplicitlyEnabled()` | `[[deprecated]] bool`（方法） | 计算值 | 已废弃的隐式推导（`enable_tasks_extension \|\| task_store`），改为显式置 `enable_tasks_extension` | `ServerOptions.hpp:90-92` |
| `SkillsImplicitlyEnabled()` | `[[deprecated]] bool`（方法） | `enable_skills_extension` | 已废弃，改显式置 `enable_skills_extension` | `ServerOptions.hpp:93-95` |

## 能力推导规则

能力没有赋值入口，全部由注册状态与开关推导（每次注册后重跑，`McpServer.cpp:777-778`、`817-818`、`866-867`、`901-902`、`923-924`、`932`）：

| 能力 | 触发条件 | 证据 |
|---|---|---|
| `tools.list_changed` | `tools_` 非空 | `McpServer.cpp:1946-1949` |
| `resources.subscribe` + `list_changed` | `resources_` 非空（静态与模板共用此表） | `McpServer.cpp:1950-1954` |
| `prompts.list_changed` | `prompts_` 非空 | `McpServer.cpp:1955-1958` |
| `logging` | 仅 `declare_logging` | `McpServer.cpp:1959-1961` |
| `completions` | 仅 `declare_completions` | `McpServer.cpp:1962-1964` |
| `extensions["io.modelcontextprotocol/tasks"]` | `enable_tasks_extension` | `McpServer.cpp:1965-1970` |
| `extensions[apps]` / `extensions[server-card]` | `enable_apps_extension` / `enable_server_card`（id 见 `ext::kAppsExtensionId`、`ext::kServerCardExtensionId`） | `McpServer.cpp:1971-1982` |
| `extensions[skills]` | `enable_skills_extension` **且** 已注册技能 **且** 已有 `resources` 能力；`directory_read` 取 `SetSkillsDirectoryRead` | `McpServer.cpp:1983-1990` |

## 工具调用的错误语义

以 `src/server/McpServer.cpp` 同步路径（无 `task_store` 或未任务化）为准：

| 场景 | 对外表现 | 证据 |
|---|---|---|
| handler 抛 `McpError` | 结果 `is_error=true`，`content` 为单条 `text` = `ErrorMessage()`；**错误码与 `data` 被丢弃** | `McpServer.cpp:2633-2636`、`496-501` |
| handler 抛其它 `std::exception` 或未知异常 | 对外文本泛化为 `Error executing tool <name>`；原始 `what()` 只写内部日志（默认日志全关，故外部不可见） | `McpServer.cpp:2637-2650`、`549-551` |
| handler 抛 `ElicitationInputRequired`（`Elicit`/`ElicitUrl` 在 modern 时代抛出） | 转为 `resultType=input_required` 结果，`inputRequests["elicitation"]` 携发起参数 | `McpServer.cpp:2630-2632`、`1142-1149` |
| `arguments` 不合 `input_schema` | SDK 主动 JSON Schema 校验并回 `is_error` + `"invalid arguments: <原因>"`，handler 不被调用 | `McpServer.cpp:2616-2623`、`553-561`；任务化路径 `2359-2362`、`2508-2515` |
| `structured_content` 不合 `output_schema` | **协议错误**而非 `is_error`：`InvalidParams` + `"tool output does not match outputSchema: <原因>"`，该次调用不会产出工具结果 | `McpServer.cpp:2673-2681` |
| 参数元素数超 `max_tool_input_elements` | `is_error` + `"tool arguments exceed maximum input element count"` | `McpServer.cpp:2323-2328` |
| 工具名未注册 | 协议错误 `InvalidParams`，`tool not found: <name>`（不是 `is_error`） | `McpServer.cpp:2145-2151` |
| legacy 时代未完成握手 | 协议错误 `InvalidRequest`，`Server not initialized`；modern 时代跳过该检查 | `McpServer.cpp:587-592`、`2086` |
| 任务化路径的 handler 异常 | 不走 `is_error`：`McpError` 经 `FailTask` 保留 code/message/data，其它异常统一 `InternalError` + `Error executing tool <name>` | `McpServer.cpp:2523-2552`、`2494-2504` |

## 内部符号（非用户 API，勿当接口引用）

- `ElicitationInputRequired`、`ToolExecutionFailureMessage`、`IsValidToolName`、`IsSkillEntryUri`、`kElicitTimeout`（600s）、`kFileInputMaxBytes`（8MB）均为 `src/server/McpServer.cpp` 匿名 namespace 内的实现细节，未从头文件导出。
- `detail::MintRequestState` / `detail::VerifyRequestState` / `detail::DecodeRequestStatePayload`（`include/mcp/server/RequestState.hpp:43-98`）是 `mcp::detail` 的内部 HMAC/base64url 工具，用户侧只需配置 `request_state_key`/`request_state_verifier`。
- `McpServer` 的 `Wire*`、`Handle*`、`EffectiveSessionId`、`ClientCapabilitiesFor` 等为私有成员函数（`McpServer.hpp:122-188`），不可作为公共契约引用。
- `ResolveToolParamAnnotations` 虽是公开方法，返回值却是 `std::vector<detail::McpParamAnnotation>`（`McpServer.hpp:52-53`），属高级用法，常规注册无需触碰。

## 本页对应源码

- `include/mcp/server/McpServer.hpp`
- `include/mcp/server/ServerOptions.hpp`
- `include/mcp/server/RequestContext.hpp`
- `include/mcp/server/McpServerTool.hpp`
- `include/mcp/server/McpTaskStore.hpp`
- `include/mcp/server/RequestState.hpp`（实现内部）
- `include/mcp/McpTypes.hpp`（`ToolOptions`、`ResourceOptions`、`PromptOptions`、`CallToolResult`、`Skill`、`ElicitRequestParams`、`ElicitResult`）
- `include/mcp/Meta.hpp`（`RequestMeta`、`CacheHint`、`LoggingLevel`、`ProgressToken`）
- `include/mcp/Capabilities.hpp`、`include/mcp/SpanHooks.hpp`、`include/mcp/McpVersion.hpp`、`include/mcp/ProtocolVersion.hpp`
- `src/server/McpServer.cpp`
