---
type: Module
title: mcp-server 服务端库
description: McpServer 门面：注册工具/资源/提示词、请求分发、能力推导、progress 推送、requestState 签发、x-mcp-header 参数头解析、任务后台执行与 URL elicitation。
tags: [server, 工具注册, 资源, 提示词, 任务, progress, elicitation]
timestamp: 2026-09-20T03:14:18+08:00
resource: src/server/McpServer.cpp
---

# mcp-server 服务端库

依赖 `mcp-protocol`。对外提供 `McpServer`（见 [/classes/mcp-server.md](../classes/mcp-server.md)）。

## 注册 API

- `RegisterTool(name, ToolOptions, fn)` / `RegisterResource / RegisterResourceTemplate / RegisterPrompt`——每次注册后重跑 `WireHandlers()` + `DeriveCapabilities()`；`RegisterResourceTemplate` 先经 `detail::UriTemplate::Parse` 校验模板，**非法模板抛 `McpError(InvalidParams, "invalid resource template '<tmpl>': <reason>")`**（行为变更：此前不校验即注册，[McpServer.cpp:329](../../src/server/McpServer.cpp)）
- `PromptOptions::arguments`（`optional<vector<PromptArgument>>`，链式 `Arguments()`）：`prompts/list` 输出提示词参数声明，配合补全请求官方线格式 `{ref, argument:{name,value}}`（序列化修正见 [/modules/protocol.md](protocol.md)）
- 任务不注册，由 `ServerOptions::task_store` 驱动
- `RegisterTool` 保存工具 `inputSchema`；`ResolveToolParamAnnotations(method, name)` 解析合法 `x-mcp-header` 属性路径，供 Streamable HTTP 的 `resolve_param_annotations` 校验 `Mcp-Param-*` 头与 `tools/call` body 一致（详见 [/concepts/mcp-param-headers.md](../concepts/mcp-param-headers.md)）
- 能力推导（[McpServer.cpp](../../src/server/McpServer.cpp)）：有工具→`tools`（list_changed）、有资源→`resources`（subscribe + list_changed）、有提示词→`prompts`、`declare_logging`/`declare_completions` 显式声明 `logging`/`completions`（默认 false，不派生）、有 task_store→`extensions = {}`

## WireHandlers 方法清单

`WireHandlers()` 拆分为 7 个接线方法（[McpServer.cpp:554](../../src/server/McpServer.cpp)）：

- **WireToolHandlers**：`tools/list`（有工具时）、`tools/call`（无条件，含任务化执行，见下）
- **WireResourceHandlers**：`resources/list`（有非模板资源时）、`resources/templates/list`（有模板时）、`resources/read`（有资源时，含 URI 模板实例路由）、`resources/subscribe|unsubscribe`（有资源时，2025-era）
- **WirePromptHandlers**：`prompts/list`（有提示词时）、`prompts/get`（无条件）
- **WireCoreHandlers**：`initialize`、`server/discover`、`ping`、`logging/setLevel`、`completion/complete` + 通知 `notifications/initialized`（置 `initialized_`）、`notifications/progress`（延长超时截止）、`notifications/elicitation/complete`（唤醒 URL elicitation 等待）
- **WireExtensionHandlers**：`server/extensions/list`
- **WireTaskHandlers**：`tasks/get/update/cancel/result/list`——仅 `options_.task_store` 存在时注册，且**仅 2025 及更早时代可用**（`IsModernProtocolVersion` 时回 `MethodNotFound`，[McpServer.cpp:775](../../src/server/McpServer.cpp)）
- **WireSubscriptionHandlers**：`subscriptions/listen`（2026-era）

`initialized_` 守卫经 `RequireInitialized(initialized, modern_era, promise)` 统一判定：**modern era（2026-07-28）直接放行**（Pin-to-2026/纯 modern 客户端无 initialize 握手也可调用），legacy era 未初始化才回 `InvalidRequest "Server not initialized"`；`initialize`/`server/discover`/`subscriptions/listen`/`tasks/*` 不经此守卫。

## 任务化 tools/call（[McpServer.cpp](../../src/server/McpServer.cpp)）

- 声明：`ToolOptions::execution`（`ToolExecution`，`mode == ToolExecutionMode::Task`）经 `RegisterTool` 写入工具定义的 `execution` 字段
- 触发条件：工具声明 task 模式**且** `options_.task_store` 存在；`tools/call` 任务化路径仅 **2025 及更早时代**可用（modern era 回 `MethodNotFound`）
- 立即返回：登记取消标志 → `CreateTask` + 置 `Working` → 发 `notifications/tasks/status` → 同步返回 `CreateTaskResult`（wire `task.taskId/task.status/createdAt`，`resultType` 落 `"task"`）
- 后台执行：`std::async` 调用工具 handler，完成后结果写入 store 并发 `notifications/tasks/status`（Working→Completed/Failed；取消判定优先于 Failed，Cancelled 静默收尾不发通知）；失败任务把首个文本 content 作为 `error` 写入 store
- 协作取消：`RequestContext::IsCancellationRequested()` 读共享取消标志（`shared_ptr<const std::atomic<bool>>`，handler 轮询自愿退出）；`tasks/cancel` 置位标志并落 `Cancelled` 终态，**终态不迁移**（已终态直接返回空结果）；**协议级 `notifications/cancelled` 与任务级取消共用同一标志**，标志在派发前由 `GetIncomingCancellationFlag(req.id)` 注入，故普通（非任务）工具 handler 同样能感知
- future 进 `pending_async_futures_`（顺带清理已完成项），`Close()` 全部等待

## 失败语义

- 工具不存在 → `InvalidParams "tool not found: <name>"`
- 非法 cursor → `InvalidParams "invalid cursor: ..."`
- 任务不存在 → `InvalidParams "task not found: <id>"`；store 抛异常 → `InternalError "task persist failed: ..."`
- `GetClientCapabilities()/GetClientInfo()` 返回 `shared_ptr<const T>`，调用方须持有返回值再访问

## 实现要点

- `SendProgress(token, progress, total?, message?)`：服务端向客户端发 `notifications/progress`（[McpServer.cpp:413](../../src/server/McpServer.cpp)），token 原样透传、`total`/`message` 可选；异步工具 handler 内经 `RequestContext::Server()` 调用可向发起方回报进度
- 变更通知：`SendToolListChanged()` / `SendResourceListChanged()` / `SendPromptListChanged()` 经 `SendListChangedNotification` **按时代分派**（现代走 `NotifySubscribers` 按 filter 过滤，2025 及更早直接广播）；新增 `SendResourceUpdated(uri)` 发布 `notifications/resources/updated`，以 uri 为过滤键，只投递给订阅了该资源的订阅者（[McpServer.hpp:88](../../include/mcp/server/McpServer.hpp)）
- `SendLoggingMessage` 的 wire `level` 为**官方字符串**（`SerializeLoggingLevel`，如 `"debug"`/`"error"`）——原数字枚举直写为真 bug，已修正（官方 conformance 捕获）
- `RequestContext` 持有 `JsonRpcRequest` **值**（替代指针）：异步工具 handler 延迟执行时原请求对象可能已析构，存值使 `GetRequest()` 在异步场景安全；构造多参 `cancellation_flag` 供任务化执行传入协作取消标志
- 任务状态 wire 值用官方字符串（`TaskStatusToWireString`：working/input_required/completed/failed/cancelled，`Pending→working`）；FileTaskStore 磁盘持久化仍为数字；`tasks/update`/`tasks/cancel` 完成后发送任务状态通知（`tasks/completed|working|cancelled`），`SendTaskStatus` 公开方法发送 `tasks/status`，任务化执行经 `SendTaskNotification` 发 `notifications/tasks/status`
- `ElicitUrl(url, message, timeout=600s)`（URL elicitation，SEP-1034）：发 `elicitation/create`（`mode="url"` + 自增 `elicitationId`），登记 `pending_url_elicitations_` 后等待 `notifications/elicitation/complete` 唤醒，以 `ElicitResult action="accept"` 完成；watchdog 超时抛 `RequestTimeout`
- `tools/list` 序列化缓存 `cached_tools_json_`：`RegisterTool` 置 `nullopt` 失效，`HandleListTools` double-check 重建
- `resources/read` 两轮遍历（[McpServer.cpp:1228](../../src/server/McpServer.cpp)）：第一轮**静态资源优先**（跳过 `is_template`，`uri_pattern == uri` 精确匹配调 `handler(uri)`）；未命中再逐模板 `detail::UriTemplate::Parse` + `Match`，命中调 `template_handler(uri, variables)`（变量名→pct-decoded 值，[McpServer.hpp:171](../../include/mcp/server/McpServer.hpp)）；两轮均未命中抛 `InvalidParams "resource not found: <uri>"`。命中分支同样回填 `resources/read` 的 cache hint
- 分页循环提取为 `PaginateEntries` 模板（resources/templates/prompts 三处共用）
- 任务结果填充提取为 `MakeGetTaskResultJson`；cache hint 查询用 `GetCacheHint`（`std::less<>` 透明比较器）

## requestState 签发（MRTR）

- `ServerOptions::request_state_key`（`optional<string>`）：设置且未显式提供 `request_state_verifier` 时，构造期自动接线内置 HMAC 校验器（`detail::VerifyRequestState`，[RequestState.hpp](../../include/mcp/server/RequestState.hpp)）——入站 `requestState` 签名不符或超过 `request_state_ttl`（0 禁用过期检查）即在 handler 前拒绝，回 `InvalidParams` + `data.reason="invalid_request_state"`
- 工具返回 `input_required` 结果且配置了 key 时，`HandleCallTool` 异步段自动 `MintRequestState`（`<base64url(payload)>.<hex(hmac)>` wire 格式，payload 注入 `iat` unix 秒）再序列化返回
- `RequestState.hpp` 另提供 `DecodeRequestStatePayload`（handler 读取本轮数据；接受签名态与裸 JSON 两种形态）

## 相关页面

- [/classes/mcp-server.md](../classes/mcp-server.md) — 门面实现细节
- [/classes/file-task-store.md](../classes/file-task-store.md) — 任务存储
- [/concepts/mrtr.md](../concepts/mrtr.md) — 服务端 elicitation
- [/modules/protocol.md](protocol.md) — 依赖的下层
