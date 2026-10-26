---
type: Class
title: McpServer
description: MCP 服务端门面：注册与分发、能力推导、progress 推送、requestState 签发、任务后台执行（含双时代分流与 inputResponses 推进）、技能注册与目录读取、URL elicitation 与回调四层接线。
tags: [server, 门面, 注册, 回调, progress, tasks, skills, elicitation]
timestamp: 2026-10-27T04:48:00+08:00
resource: include/mcp/server/McpServer.hpp
---

# McpServer

服务端门面（[McpServer.hpp](../../include/mcp/server/McpServer.hpp)），工厂 `Create(transport, options)`。构造时探测 `transport_->IsStateless()`，配置 `protocol_version` 则立即 `SetNegotiatedProtocolVersion`。

## 注册 API

- `RegisterTool(name, ToolOptions, fn)` / `RegisterResource / RegisterResourceTemplate / RegisterPrompt`：每次注册后重跑 `WireHandlers()`（拆为 7 个 `Wire*Handlers` 方法）+ `DeriveCapabilities()`；**重复注册一律抛 `McpError(InvalidParams)`**——同名工具、同名/同 uri 资源与模板、同名提示词、同 uri 技能均拒绝（`RegisterSkill` 不再静默覆盖），空 handler（资源/模板/提示词/补全）同样抛 `InvalidParams`；`RegisterTool` 校验工具名 `^[A-Za-z0-9._-]{1,128}$`（`IsValidToolName`，违规抛 `McpError(InvalidParams)`，[McpServer.cpp:93](../../src/server/McpServer.cpp)），同时把 `cached_tools_json_` 置 `nullopt` 失效（[McpServer.cpp:287](../../src/server/McpServer.cpp)）
- `RegisterResourceTemplate(name, uri_template, opts, handler)`：模板先经 `detail::UriTemplate::Parse`（[UriTemplate.hpp](../../src/detail/UriTemplate.hpp)）校验，**非法模板抛 `McpError(InvalidParams, "invalid resource template '<tmpl>': <reason>")`**（[McpServer.cpp:329](../../src/server/McpServer.cpp)）——此前不校验即写入；条目存 `uri_pattern` + `is_template=true` + `template_handler`（`(uri, variables)` 回调，[McpServer.hpp:171](../../include/mcp/server/McpServer.hpp)），资源/模板/提示词同为 vector 追加
- 工具/资源/提示词条目内部结构见 [McpServer.hpp](../../include/mcp/server/McpServer.hpp)（ResourceEntry 含 uri_pattern/is_template 等；PromptEntry 含 `arguments`，经 `PromptOptions::Arguments()` 声明并随 `prompts/list` 输出）
- `RegisterTool` 持有工具 `inputSchema`；`ResolveToolParamAnnotations(method, name)` 解析其中合法的 `x-mcp-header` 属性路径，供 Streamable HTTP 的 `resolve_param_annotations` 校验 `Mcp-Param-*` 头与 `tools/call` body 一致（详见 [/concepts/mcp-param-headers.md](../concepts/mcp-param-headers.md)）
- `RegisterSkill(skill)`：技能条目 `uri` 须为 `<scheme>://<名>/SKILL.md` 形态（scheme 无关：`IsSkillEntryUri` 只要求非空 `://`、名段非空、`/SKILL.md` 结尾，违规抛 `McpError(InvalidParams, "invalid skill uri: ...")`；`skill://` 仍为约定 scheme，`https://…/SKILL.md` 等他源形态放行后由客户端 `SkillVerify` 助手做信任判定，见 [/concepts/skills-extension.md](../concepts/skills-extension.md)）；同 uri 按**覆盖**语义替换（vector 线性查找），注册后重跑 `WireHandlers()` + `DeriveCapabilities()`；`SetSkillsDirectoryRead(enabled)` 开关目录读取声明（仅影响能力扩展中的 `directoryRead` 标志位，不重跑 `WireHandlers`）
- 技能三处理器经 `WireSkillHandlers` 接线（`skills_` 为空时不注册）：`HandleListSkills` / `HandleGetSkill` / `HandleReadDirectory`——三者均仅现代版本可用（legacy 回 `MethodNotFound`），均经 `RequireInitialized` 守卫，均要求 `capabilities_.resources`（缺失回 `InvalidParams "... requires the resources capability"`）；`HandleReadDirectory` 另要求 `skills_directory_read_`（未声明回 `InvalidParams "... requires directoryRead to be declared"`，未知目录回 `InvalidParams "directory not found: ..."`）；列表/目录均走 `PaginateEntries`（`kDefaultPageSize = 100`）+ `GetCacheHint`（`skills/list`、`skills/get`、`resources/directory/read` 三键）回 `ttlMs`/`cacheScope`；目录视图由 `CollectDirectoryChildren` 从已注册技能的 manifest 派生（skill 根 + 资源子路径，子目录标 `mime_type "inode/directory"`，按 uri 排序，请求 uri 归一化去尾斜杠）

## 回调接线（四层）

1. 简写：`on_method_called`、`on_client_connected`、`on_initialized`、`on_protocol_error`
2. 完整消息：`on_request`、`on_response`、`on_error`、`on_notification`
3. 生命周期 + 传输层：`on_transport_close`、`on_transport_error`（经 `dynamic_cast<TransportBase*>` 设置）
4. `incoming_filters` / `outgoing_filters`（FilterPipeline）

`on_request` 与 `on_method_called` 合并进一个回调；`on_error` 与 `on_protocol_error` 合并（后者只传 `err.error.message`）。

## 分发细节

- `tools/call`：异步执行（std::async），future 存 `pending_async_futures_`，`Close()` 先全部 wait；工具声明 `output_schema` 且返回 `structured_content` 时用 `ValidateJsonSchema` 校验
- **任务化执行**：工具经 `ToolOptions::execution`（`ToolExecutionMode::Task`）声明且有 `task_store` 时，`tools/call` 立即返回 `CreateTaskResult`（`resultType` 落 `"task"`，双时代均可；2026 时代扁平直写顶层 `taskId/status/createdAt` 并抹掉嵌套 `task` 对象，2025 时代保留嵌套 `task` 做兼容；反序列化顶层优先、嵌套兜底）——任务 ID 为 `task-<session_id>-<16 字节随机 hex>`（安全随机、不可枚举），创建时记 `owner = SessionId()` 与默认 `ttl_ms = 300000`（`kDefaultTaskTtlMs`），创建前顺带清扫过期任务（`SweepExpiredTasks`）；后台执行 Working→Completed/Failed：工具正常返回（含 `is_error` 结果）经 `SerializeCallToolResult` 入库并落 `Completed`；失败统一经 `FailTask` 一步原子落态（原「写错误结果再改状态」两步序列废除）；`GetTaskResult` 对 Failed 任务以内联 `error` 透传错误载荷（非 Failed 走 `result`）；`tasks/get/update/cancel/list` 按 owner 会话隔离（他人任务与不存在同样回 `InvalidParams`，`tasks/update` 另拒终态）；协作取消经 `RequestContext::IsCancellationRequested()`（`shared_ptr<const std::atomic<bool>>` 标志），`tasks/cancel` 置位标志、落 `Cancelled` 终态且终态不迁移，取消判定优先于 Failed；同一标志在派发前由 `handler_->GetIncomingCancellationFlag(req.id)` 注入（[McpServer.cpp:1038](../../src/server/McpServer.cpp)），故**普通（非任务）工具 handler 也能感知协议级 `notifications/cancelled`**；现代版本下任务状态通知改经 `NotifySubscribers` 按订阅路由（见下）；`HandleCallTool` 三路径（同步/现代必需/后台）在调 handler 前先经 `ValidateJsonSchema` 校验 `arguments` 合 `input_schema`，不合直接回 `is_error` 工具结果（`"invalid arguments: …"`，后台路径同时入库 `Completed`），`RegisterTool` 期另校验 `input_schema` 中的 `x-mcp-header` 注解合法性（非法直接 `InvalidParams` 拒收）；工具参数元素数超 `ServerOptions::max_tool_input_elements` 时同样回 `is_error`（`"tool arguments exceed maximum input element count"`）
- `resources/read`：先扫非模板资源做 `uri_pattern == uri` 精确匹配，再扫模板逐个 `detail::UriTemplate::Parse` + `Match` 后调 `template_handler(uri, variables)`（静态优先，变量值为 pct-decoded，[McpServer.cpp:1254](../../src/server/McpServer.cpp)）；两轮均未命中抛 `InvalidParams "resource not found: <uri>"`
- 分页：`kDefaultPageSize = 100`，cursor 为数字字符串；resources/templates/prompts/skills 三处共用 `PaginateEntries` 模板（含 include 谓词；页满时 `next_index = index - 1`，此前多计一位已修正）
- 列表响应缓存提示：按方法名查 `options_.cache_hints`（6 个方法：tools/list、resources/list、resources/templates/list、resources/read、prompts/list、server/discover；`GetCacheHint` 用 `std::less<>` 透明比较器）
- `tools/list` 序列化缓存：`cached_tools_json_` 在 `RegisterTool` 时失效，`HandleListTools` shared/unique 锁 double-check 重建（[McpServer.cpp:971](../../src/server/McpServer.cpp)）
- `HandleInitialize`：已配置 `options_.protocol_version` 时直接采用（可含现代版本），未配置才遍历 `kProtocolVersions` 选非现代公共版本；**未声明（空串）回退 `kDefaultNegotiatedProtocolVersion`，非空未知版本回退 `kLegacyProtocolVersion`**（[McpServer.cpp:1400](../../src/server/McpServer.cpp)）；`result.protocol_version` 回显协商结果
- `HandleDiscover`：无条件置 `initialized_=true`；**协商版本 = `options_.protocol_version`（配置时）否则 `kLatestProtocolVersion`**（[McpServer.cpp:1338](../../src/server/McpServer.cpp)）；支持版本 = `EffectiveSupportedVersions()`（默认全表 5 个，`ServerOptions::supported_protocol_versions` 非空时窄化，见 [/concepts/version-negotiation.md](../concepts/version-negotiation.md)）
- **初始化守卫 era 门控**：除 `initialize`/`server/discover`/`subscriptions/listen`/`tasks/*` 外的处理器经 `RequireInitialized` 统一判定——modern era（2026-07-28）直接放行（无 initialize 握手的纯 modern 客户端可用），legacy era 未初始化回 `InvalidRequest "Server not initialized"`
- **任务分流（双时代，SEP-2663）**：tasks/get、tasks/update、tasks/cancel 在 2025 与 2026 双时代均可用；tasks/result、tasks/list 仍仅 2025（modern 回 `MethodNotFound`，[McpServer.cpp:1025](../../src/server/McpServer.cpp)）。modern 下三处理器先做 `Mcp-Name` 头校验（请求 meta 携带 `Mcp-Name` 时经 `MakeWireCodec` 的 `ValidateTaskHeaders` 比对 `params.taskId`，`TryGetMcpName` 同时认 `Mcp-Name`/`mcp-name` 两种大小写；无头时跳过校验），失败直接回异常；三处理器成功路径的任务通知在 modern 下改经 `handler_->NotifySubscribers` **按订阅路由**（投递给全部现存订阅者、每帧 `_meta` 带 `subscriptionId`，无订阅者即丢弃），legacy 保持 `SendNotification` 广播；`SendTaskStatus(task_id, status)` 同样按协商时代二选一（modern 路由、legacy 广播，不再直接返回）。任务 wire 状态为官方字符串 `TaskStatusToWireString`（working/input_required/completed/failed/cancelled，`Pending→working`，[McpServer.cpp:76](../../src/server/McpServer.cpp)）；`GetTaskResult` 填充提取为 `MakeGetTaskResultJson`（含 include_optional_fields 开关，新字段 statusMessage/createdAt/lastUpdatedAt/ttlMs/pollIntervalMs/inputRequests/error 透传，见 [/classes/file-task-store.md](file-task-store.md)）
- **tasks/update 的 inputResponses 分支**：`result` 与 `inputResponses` 不得同带（同带回 `InvalidParams "tasks/update must not carry both result and inputResponses"`）；带 `inputResponses` 时要求任务存在且状态为 `InputRequired`（否则 `InvalidParams "task not found: ..."` / `"task is not waiting for input: ..."`），经 `store->AdvanceTaskWithInput`（状态 InputRequired→Working + 存 `input_responses` + 刷新 `last_updated_at`，持久化失败回 `InternalError "task persist failed: ..."`），legacy 下补发 `tasks/working` 通知（modern 抑制），回空 `UpdateTaskResult`
- **请求 `_meta` 合并**：`HandleCallTool` 先反序列化请求顶层 `meta`（非法回 `InvalidParams "invalid _meta: ..."`），再与 `params.meta` 合并——params 侧缺失项逐项回填（progressToken/protocolVersion/clientInfo/clientCapabilities/logLevel/extensions/traceparent/tracestate/baggage），fixture 此前经 `GetRequest().meta` 直读的 progressToken/logLevel/clientCapabilities 现走 `Params().meta` 即可
- **非工具结果的 input_required**：`HandleReadResource`（静态与模板两分支）与 `HandleGetPrompt` 在 handler 返回 `input_required` 时直接回 `InputRequiredResult`（`resultType` 落 `"input_required"`，`request_state_key` 配置时自动签名 `request_state`，`resources/read` 的 cache hint 照常回填）；`resources/read` 与 `prompts/get` 的请求类型新增 `input_responses` 与 `request_state` 字段（重试闭环与工具调用一致，见 [/concepts/mrtr.md](../concepts/mrtr.md)）
- `SendProgress(token, progress, total?, message?)`（[McpServer.cpp:413](../../src/server/McpServer.cpp)）：向客户端发 `notifications/progress`，token 原样透传不校验归属，`total`/`message` 可选；异步工具 handler 内经 `RequestContext::Server()` 调用——`RequestContext` 持有 `JsonRpcRequest` **值**（替代指针），异步场景 `GetRequest()` 安全；modern 协商版本下先经 `ITransport::BufferRequestStreamNotification` 按 progressToken 路由进所属 POST 的 SSE 流（命中则不再走 `SendNotification`，stateless 下 progress 与最终响应同流到达）
- `subscriptions/listen`：仅现代版本（另受 `ServerOptions::subscriptions_enabled` 总开关门控，关闭回 `MethodNotFound "subscriptions are disabled"`）：订阅 ID 单调分配（原子量从 1 起）；订阅后**同步回发 `subscriptions/acknowledged` 通知帧**（`SendSubscriptionsAcknowledged`，[McpServer.cpp:1465](../../src/server/McpServer.cpp)）——`honored` 回显 filter、meta 带 `protocolVersion` + `subscriptionId`（优先客户端 `_meta` 传入 ID，未设置回退服务端自增 ID）
- **变更通知分发**：`SendToolListChanged()` / `SendResourceListChanged()` / `SendPromptListChanged()` 经 `SendListChangedNotification` **按时代分派**——现代版本走 `NotifySubscribers`（只发订阅了对应 filter 的订阅者），2025 及更早版本直接广播；`SendResourceUpdated(uri)`（[McpServer.hpp:88](../../include/mcp/server/McpServer.hpp)）发布 `notifications/resources/updated`，以 uri 作为订阅过滤键，只投递给订阅了该资源的订阅者
- `SendLoggingMessage`：低于当前级别直接丢弃，logger 固定 `"mcp-server"`；wire `level` 为官方字符串（`SerializeLoggingLevel`）——原数字直写已修正；modern 协商版本下先经 `BufferRequestStreamNotification` 广播进全部 hold-open 请求流（命中则不再走 `SendNotification`）
- **requestState 签发**：`options_.request_state_key` 配置时（无显式 verifier）构造期接线内置 HMAC 校验器（`detail::VerifyRequestState`，`request_state_ttl` 控制 `iat` 过期，0 禁用）；`HandleCallTool` 异步段对 `input_required` 结果自动 `MintRequestState` 后返回；校验失败回 `InvalidParams` + `data.reason="invalid_request_state"`
- `DeriveCapabilities`：`options_.declare_logging`/`declare_completions` 显式声明对应能力（默认 false，不派生）；`options_.enable_tasks_extension` 为真时声明扩展 `io.modelcontextprotocol/tasks`（空对象，由 `SerializeTasksExtensionSettings` 生成，[McpServer.cpp:1637](../../src/server/McpServer.cpp)；`task_store` 存在仅决定三处理器是否接线，关时入口回 `MethodNotFound "tasks extension is disabled"`）；`options_.enable_skills_extension` 为真**且** `skills_` 非空**且** `capabilities_.resources` 具备时声明扩展 `io.modelcontextprotocol/skills`（`{directoryRead?: true}`，仅 `SetSkillsDirectoryRead(true)` 后携带该键，[McpServer.cpp:1655](../../src/server/McpServer.cpp)；关时三处理器入口回 `MethodNotFound "skills extension is disabled"`）
- `Elicit`：无 config 时超时 600s；结果 `code` 为负抛 McpError
- `ElicitUrl(url, message, timeout=600s)`（URL elicitation，SEP-1034）：legacy 时代发 `elicitation/create`（`mode="url"` + 自增 `elicitationId`），登记 `pending_url_elicitations_`（`PendingUrlElicitation`：promise + completed 标志）后等待 `notifications/elicitation/complete` 唤醒，以 `ElicitResult action="accept"` 完成；watchdog 超时抛 `RequestTimeout`，连接关闭经 `AbandonPendingUrlElicitation` 以错误结算。modern 时代不走该等待——直接抛 `UrlElicitationInputRequired`（携 `inputRequests["elicitation"]` 的 `elicitation/create` 入参，不含 `elicitationId`），由 `HandleCallTool` 三路径捕获后转为 `InputRequiredResult`（`request_state_key` 配置时同步签 `request_state`），客户端经 `SetUrlElicitationHandler` 承接、经 `tasks/update` `inputResponses` 履行；legacy 的 `elicitation/complete` 通知处理器在 modern 下直接返回
- `SetCacheable(resolver)` / `ServerOptions::cache_hint_resolver`：按方法动态解析 `CacheHint` 的逃生钩子（`ResolveCacheHint` 优先调钩子，未命中才查 `options_.cache_hints` 静态表；构造期从 `ServerOptions` 接线，运行时可经 `SetCacheable` 替换，`cache_hook_mutex_` 保护）
- `ServerOptions` 新增旋钮：`supported_protocol_versions`（窄化 discover/initialize/`_meta` 门控三处，见 [/concepts/version-negotiation.md](../concepts/version-negotiation.md)）、`subscriptions_enabled`（默认 true，关闭则 `subscriptions/listen` 不可用）、`max_tool_input_elements`（工具参数元素数上限，超限回 `is_error` 工具结果）
- **六开关门控位置**：etag 构造期同步（`SetEtagCacheEnabled(options_.enable_etag_cache)`，[McpServer.cpp:510](../../src/server/McpServer.cpp)）、触发器发射门（`EmitTriggerEvent` 开头，关直接 return，[McpServer.cpp:832](../../src/server/McpServer.cpp)）、渐进发现（`HandleListTools` 开头，关走缓存快照路径，[McpServer.cpp:1690](../../src/server/McpServer.cpp)）、文件输入（`HandleCallTool` 参数解析段，关跳过 file 引用消解，[McpServer.cpp:1824](../../src/server/McpServer.cpp)）、apps `ui://` 读门（`HandleReadResource` 内 `HasAppsUriScheme` 分支，关报 `HandlerError "apps extension is disabled"`，[McpServer.cpp:2432](../../src/server/McpServer.cpp)）、server-card 获取门（`GetServerCard` 开头，关报 `HandlerError "server card is disabled"`，[McpServer.cpp:2917](../../src/server/McpServer.cpp)）；注意 apps/server-card 关态为 `HandlerError` 而非 `MethodNotFound`
- `GetClientCapabilities()/GetClientInfo()` 返回 `shared_ptr<const T>`；`GetNegotiatedProtocolVersion()` 返回 `std::string`（转发自 handler）；`GetCapabilities()` **按值返回** `ServerCapabilities`（锁内拷贝，调用方不得在锁外持引用访问可变成员）；`SessionId()` 返回构造期生成的安全随机会话标识（8 字节 hex，任务 owner 锚定用）
- **锁外回调**：`resources/read`、`prompts/get`、`completion/complete` 的用户 handler 改为锁内快照、`std::async` 锁外执行（future 入 `pending_async_futures_`，`Close()` 统一 wait）——handler 内可安全再入注册 API，不再持 `registry_mutex_` 执行用户代码；`resources/read` 未命中任何资源/模板时回 `InvalidParams "resource not found: <uri>"`（data 带 uri）

## URI 模板已知限制

`detail::UriTemplate`（[UriTemplate.hpp](../../src/detail/UriTemplate.hpp)，RFC 6570 子集）在 `RegisterResourceTemplate` 校验与 `resources/read` 匹配路径上仍有以下限制：

- **不支持前缀修饰符 `{var:N}`**（RFC 6570 Level 4）：`ParseExpression` 只接受 `varchar` 与 `.` 分隔的变量名，`:` 既非算子也非合法变量名字符，注册期即被 `Parse` 以 `invalid uri template variable name` 拒绝并抛 `InvalidParams`
- **拒绝歧义模板**（设计选择）：两个相邻且无字面量分隔的变量（如 `{a}{b}`）报 `adjacent uri template variables without a literal separator`；两个多段变量（如 `{+a}{+b}`）报 `multiple multi-segment variables`；均在注册期抛 `InvalidParams`
- **匹配不了「键名内联进字面量」表达式的部分变量缺失展开**：`BuildMatchParts` 把 `{?x,y}` / `{;x,y}` 每个变量的键名前缀固化进前一个 `MatchPart.literal`（`?x=`/`&y=`、`;x`/`;y`）；`Expand` 在 `x` 未定义时产出 `?y=2`（`;y=2`），而 `Match` 的字面量链仍要求 `?x=`（`;x`）前缀，故这类 URI 匹配失败

## 相关页面

- [/modules/server.md](../modules/server.md) — 所属库
- [/classes/mcp-session-handler.md](mcp-session-handler.md) — 底层引擎
- [/concepts/mrtr.md](../concepts/mrtr.md) — 服务端 elicitation
- [/concepts/mcp-param-headers.md](../concepts/mcp-param-headers.md) — 工具参数头校验
- [/classes/file-task-store.md](file-task-store.md) — 任务存储集成
- [/concepts/tasks-extension.md](../concepts/tasks-extension.md) — 任务扩展双时代语义
- [/concepts/skills-extension.md](../concepts/skills-extension.md) — 技能扩展三方法与目录读取
