---
type: Concept
title: 任务扩展
description: SEP-2663 任务扩展与 Tasks v2：双时代 tasks/get|update|cancel、modern tasks/delete 与 notifications/tasks/DetailedTask/taskIds、legacy tasks/result|list 与 notifications/tasks/*、principal owner、任务化 tools/call 与订阅路由。
tags: [tasks, 扩展, 双时代, 2026, owner]
timestamp: 2026-11-25T20:55:00+08:00
resource: src/server/McpServer.cpp
---

# 任务扩展

SEP-2663 任务扩展（扩展标识 `io.modelcontextprotocol/tasks`，[Capabilities.hpp](../../include/mcp/Capabilities.hpp) 的 `kTasksExtensionId`）允许长耗时工具调用以任务句柄立即返回、后台执行、轮询取结果。

## 扩展声明

- 服务端：必须显式 `ServerOptions::enable_tasks_extension = true`，`DeriveCapabilities` 才在 `capabilities_.extensions` 中声明 `io.modelcontextprotocol/tasks`（空对象，`SerializeTasksExtensionSettings`，[McpServer.cpp](../../src/server/McpServer.cpp)）；`task_store` 存在仅决定 `WireTaskHandlers` 是否接线 `tasks/get|update|cancel` 三处理器，三处理器入口关时一律回 `MethodNotFound "tasks extension is disabled"`（[McpServer.cpp](../../src/server/McpServer.cpp)），任务化 `tools/call` 关时走同步路径（[McpServer.cpp](../../src/server/McpServer.cpp)）
- 客户端：`ClientOptions::capabilities->extensions` 含该键，或 `ClientOptions::extensions`（JsonValue）含该键；modern 协商版本下收到 `resultType=="task"` 而未声明时抛 `McpError(HandlerError)`（`DeclaresTasksExtension`，[McpClient.cpp](../../src/client/McpClient.cpp)），legacy 不检查

## 方法分流

| 方法 | 2025 | 2026 |
|------|------|------|
| tasks/get、tasks/update、tasks/cancel | 可用 | 可用（头校验 + 订阅路由 + principal owner） |
| tasks/result、tasks/list | 可用 | `MethodNotFound`（`kLegacyGetTaskPayload`/`kLegacyListTasks`，服务端硬拒绝） |
| tasks/delete | `NotInEra` | 可用（按 principal owner 校验后移除） |
| tools/call（`ToolExecutionMode::Task` + task_store） | 立即返回 `CreateTaskResult` | 立即返回 `CreateTaskResult` |

wire 层三方法同时属于 2025 与 2026 方法集合（[WireCodec.cpp](../../src/protocol/WireCodec.cpp) 的 `k2025OnlyRequestMethods` 与 `k2026OnlyRequestMethods` 均含），`tasks/result`/`tasks/list` 仅前者含，`tasks/delete` 仅后者含。

## 现代语义差异

- **通知路由**：modern 只发送/接收单 `notifications/tasks`（`notifications::kTasks`），`params` 为完整 `DetailedTask`（必填 `taskId`/`status`/`createdAt`，可选 `lastUpdatedAt`/`ttlMs`/`result`/`error`/`inputRequired`/`inputRequests`；`completed` 必带 `result`、`failed` 必带 `error`），经 `McpSessionHandler::NotifySubscribers` 与 Streamable HTTP listen tag 双重按 `taskIds` 过滤；`subscriptions/listen` 的 `_meta` 携带 `io.modelcontextprotocol/subscriptionId` 时 ack 回显同一 id 与所接受的过滤器（含 `taskIds`）。legacy 保持 `SendNotification` 广播 `notifications/tasks/status|working|completed|failed|cancelled|input_required`，modern 的 WireCodec 通知集合不再接受这些旧方法
- **`Mcp-Name` 头校验**：modern 下 tasks 三处理器在请求 meta 携带 `Mcp-Name`（同时认 `mcp-name` 小写）时经 `WireCodec::ValidateTaskHeaders` 比对 `params.taskId`——缺失/空 taskId → `InvalidParams`，头值不等 → `HeaderMismatch`；无头时跳过。HTTP 层（`ValidateMcpHeaders`）对三方法**强制**要求该头（缺失/失配 → 400），客户端对三方法按 `params.taskId` 发送 `Mcp-Name`
- **结果形态**：`CreateTaskResult` 按时代分形态——2026 时代扁平直写顶层 `taskId/status/createdAt`（+ 可选 statusMessage/lastUpdatedAt/ttlMs/pollIntervalMs/inputRequests）并抹掉嵌套 `task` 对象，2025 时代保留嵌套 `task` 对象做兼容；反序列化顶层优先、嵌套兜底（`DeserializeCreateTaskResult` 双形态接受）。`ResultType` 新增 `Task` 枚举值（wire `"task"`），2026 `EncodeResult` 不改写 task 结果的 `resultType`

## tasks/update 的 inputResponses 分支

`UpdateTaskRequestParams` 新增 `inputResponses`（与 `result` 互斥，同带回 `InvalidParams`）。带 `inputResponses` 时服务端要求任务存在且状态为 `InputRequired`，经 `IMcpTaskStore::AdvanceTaskWithInput`（`FileTaskStore` 实现：存 `input_responses`、状态 InputRequired→Working、刷新 `last_updated_at`、同快照回滚路径持久化）推进后回空结果；modern 下经订阅路由补发 `tasks/working`（legacy 广播）。客户端签名 `UpdateTask(task_id, result?, input_responses?)`，`PollTaskToCompletion`（默认 500ms 间隔 / 300s 超时）轮询至终态——任务 `poll_interval_ms > 0` 时改用服务端指示间隔（钳制 300s 上限 `kMaxServerPollInterval`），临 deadline 前停轮询直接抛超时。

## 会话作用域与生命周期守卫

- **principal owner 隔离**：任务 owner 只能来自 authenticated principal，由 `McpServer::SetSessionPrincipal`/`SetDefaultPrincipal` 注入；modern 任务操作（get/update/cancel/delete 与任务化 tools/call）在无 principal 时失败关闭（`TaskNotFound -32006`），不从 `params`、`_meta`、`taskId` 或客户端自报字段读取 owner；legacy 在无 principal 时按旧语义回退会话 id。任务 ID 形如 `task-<owner>-<16 字节随机 hex>`（不可枚举）；跨 owner 与不存在任务返回同一拒绝语义（`tasks/delete` 用 `TaskNotFound -32006`），`tasks/list` 仅列本 owner 任务
- **任务预算**：`include/mcp/server/McpTaskStore.hpp` 定义 `kMaxTasksPerPrincipal=10000`（B-77）、`kMaxTaskResultBytes=8388608`/`kMaxTaskNotificationBytes=1048576`（B-78）、`kMaxTaskIdsPerSubscription=1024`；超限分别拒绝创建/写入/过滤，modern 通知超字节上限则丢弃，跨 principal 放行数 0（B-79）
- **TTL 与回收**：任务创建默认 `ttl_ms = 300000`（5 分钟，`kDefaultTaskTtlMs`）；`IMcpTaskStore::GetExpiredTaskIds/RemoveTask` 供回收用，服务端在每次创建新任务前顺带清扫过期任务（`SweepExpiredTasks`）
- **终态守卫**：`tasks/update` 对已终态（Completed/Failed/Cancelled）任务直接回 `InvalidParams`（存储层 `UpdateTask/CancelTask/FailTask/ParkTaskForInput` 同样拒绝再变更，`SetTaskStatus` 保留同状态幂等）；服务端失败落态经 `FailTask` 一步原子完成

## 存储字段

`TaskState` 含 `status_message`、`last_updated_at`（Create/Update/Cancel/SetStatus/Advance 全路径刷新）、`ttl_ms`、`poll_interval_ms`、`input_requests`、`input_responses`、`owner`，`FileTaskStore` 与 `InMemoryTaskStore` 均持久化/保留该字段（旧记录 `owner` 为空且不删除、不重写），新增任务经 `CreateTask(id, owner, ttl)` 写入 principal。modern 通知另经 `SerializeDetailedTask`/`IsValidDetailedTask` 生成与校验，经 `MakeGetTaskResultJson` 透传到 `GetTaskResult`。持久化格式为 JSONL 追加 + 阈值压缩，详见 [/classes/file-task-store.md](../classes/file-task-store.md)。

## 官方 conformance 扩展验证（T-21，2026-11-25）

referee `@modelcontextprotocol/conformance@0.2.0-alpha.12` 的 `list --requirements 2026-07-28` 定义 10 个 server 侧 tasks 场景：`tasks-lifecycle`、`tasks-capability-negotiation`、`tasks-wire-fields`、`tasks-request-state-removal`、`tasks-mrtr-input`、`tasks-request-headers`、`tasks-dispatch-and-envelope`、`tasks-status-notifications`、`tasks-required-task-error`、`tasks-mrtr-composition`。WT-RELEASE / `6c947db` 在 Node 22.23.3 下显式执行：

- server modern `--requirements 2026-07-28 --force`：**169 passed / 26 failed / 0 warnings**，退出码 0；26 项失败全部来自上述 10 个场景（`tasks-lifecycle` 2/7、`tasks-capability-negotiation` 2/3、`tasks-wire-fields` 1/3、`tasks-request-state-removal` 1/1、`tasks-mrtr-input` 1/3、`tasks-request-headers` 3/2、`tasks-dispatch-and-envelope` 4/5、`tasks-status-notifications` 0/0、`tasks-required-task-error` 1/1、`tasks-mrtr-composition` 1/1）。referee 在 requirements 模式下把这些 extension 场景判为 expected（for visibility），不计入 core MUST，基线检查通过。
- server legacy `--suite all --force`：81 passed / 0 failed，退出码 1；唯一 unexpected 为 pending `server-sse-polling` 0/0（fixture 未覆盖该 SSE 轮询语义，不写基线、不以 skip 冒充通过）。
- 扩展 schema 侧：官方 [ext-tasks](https://github.com/modelcontextprotocol/ext-tasks) v2 固定快照 @ `cc9aaac1...`（v0.2.3）含 76 个 `$defs`，其中 `Task`/`DetailedTask`/`CreateTaskResult`/`GetTaskRequest|Result`/`UpdateTaskRequest|Result`/`CancelTaskRequest|Result`/`Error` 等 22 个正反例全部符合预期（必填字段、状态迁移、任务头 `ttlMs` 类型与错误结构均被正确约束）。

结论：本 SDK 的任务扩展**核心 wire 契约与 schema 形状已对齐官方定义**，但 conformance 的 10 个 tasks 扩展场景仍有 26 项未通过（fixture 能力覆盖缺口，非 core MUST 回归）；已逐场景留证，等待后续补齐 fixture 后复跑，不写基线、不声称全绿。证据见 `.handoff/evidence/T-21/extensions-tasks-skills/` 与 `schema-positive-negative.md`。

## 相关页面

- [/classes/wire-codec.md](../classes/wire-codec.md) — 时代方法集合与 `ValidateTaskHeaders`
- [/classes/mcp-server.md](../classes/mcp-server.md) — 任务分流与扩展声明
- [/classes/mcp-client.md](../classes/mcp-client.md) — 轮询与扩展声明守卫
- [/classes/file-task-store.md](../classes/file-task-store.md) — 持久化格式
- [/concepts/version-negotiation.md](version-negotiation.md) — 门控表
