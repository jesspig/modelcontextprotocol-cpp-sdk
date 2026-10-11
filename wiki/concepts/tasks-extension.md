---
type: Concept
title: 任务扩展
description: SEP-2663 任务扩展：双时代 tasks/get|update|cancel、2025-only tasks/result|list、任务化 tools/call、inputResponses 推进、Mcp-Name 头校验与现代通知抑制。
tags: [tasks, 扩展, 双时代, 2026]
timestamp: 2026-10-11T16:35:00+08:00
resource: src/server/McpServer.cpp
---

# 任务扩展

SEP-2663 任务扩展（扩展标识 `io.modelcontextprotocol/tasks`，[Capabilities.hpp](../../include/mcp/Capabilities.hpp) 的 `kTasksExtensionId`）允许长耗时工具调用以任务句柄立即返回、后台执行、轮询取结果。

## 扩展声明

- 服务端：`ServerOptions::task_store` 配置时，`DeriveCapabilities` 在 `capabilities_.extensions` 中声明 `io.modelcontextprotocol/tasks`（空对象，`SerializeTasksExtensionSettings`）
- 客户端：`ClientOptions::capabilities->extensions` 含该键，或 `ClientOptions::extensions`（JsonValue）含该键；modern 协商版本下收到 `resultType=="task"` 而未声明时抛 `McpError(HandlerError)`（`DeclaresTasksExtension`，[McpClient.cpp](../../src/client/McpClient.cpp)），legacy 不检查

## 方法分流

| 方法 | 2025 | 2026 |
|------|------|------|
| tasks/get、tasks/update、tasks/cancel | 可用 | 可用（头校验 + 通知抑制） |
| tasks/result、tasks/list | 可用 | `MethodNotFound`（`kLegacyGetTaskPayload`/`kLegacyListTasks`，服务端硬拒绝） |
| tools/call（`ToolExecutionMode::Task` + task_store） | 立即返回 `CreateTaskResult` | 立即返回 `CreateTaskResult`（此前拒绝，现放行） |

wire 层三方法同时属于 2025 与 2026 方法集合（[WireCodec.cpp](../../src/protocol/WireCodec.cpp) 的 `k2025OnlyRequestMethods` 与 `k2026OnlyRequestMethods` 均含），`tasks/result`/`tasks/list` 仅前者含。

## 现代语义差异

- **通知抑制**：modern 下任务状态通知一律不发——任务化 `tools/call` 的初始 Working、`finish` 的终态、`tasks/update` 的 `tasks/completed`/`tasks/working`、`tasks/cancel` 的 `tasks/cancelled` 均跳过；`SendTaskStatus` 直接返回。legacy 保持 `notifications/tasks/*` 全量语义
- **`Mcp-Name` 头校验**：modern 下 tasks 三处理器在请求 meta 携带 `Mcp-Name`（同时认 `mcp-name` 小写）时经 `WireCodec::ValidateTaskHeaders` 比对 `params.taskId`——缺失/空 taskId → `InvalidParams`，头值不等 → `HeaderMismatch`；无头时跳过。HTTP 层（`ValidateMcpHeaders`）对三方法**强制**要求该头（缺失/失配 → 400），客户端对三方法按 `params.taskId` 发送 `Mcp-Name`
- **结果形态**：`CreateTaskResult` 顶层直写 `taskId/status/createdAt`（+ 可选 statusMessage/lastUpdatedAt/ttlMs/pollIntervalMs/inputRequests），同时保留嵌套 `task` 对象做兼容；反序列化顶层优先、嵌套兜底。`ResultType` 新增 `Task` 枚举值（wire `"task"`），2026 `EncodeResult` 不改写 task 结果的 `resultType`

## tasks/update 的 inputResponses 分支

`UpdateTaskRequestParams` 新增 `inputResponses`（与 `result` 互斥，同带回 `InvalidParams`）。带 `inputResponses` 时服务端要求任务存在且状态为 `InputRequired`，经 `IMcpTaskStore::AdvanceTaskWithInput`（`FileTaskStore` 实现：存 `input_responses`、状态 InputRequired→Working、刷新 `last_updated_at`、同快照回滚路径持久化）推进后回空结果；legacy 下补发 `tasks/working` 通知。客户端签名 `UpdateTask(task_id, result?, input_responses?)`，`PollTaskToCompletion`（500ms 间隔 / 300s 超时）轮询至终态。

## 存储字段

`TaskState` 新增 `status_message`、`last_updated_at`（Create/Update/Cancel/SetStatus/Advance 全路径刷新）、`ttl_ms`、`poll_interval_ms`、`input_requests`、`input_responses`，`FileTaskStore` 持久化（旧文件容缺可读），经 `MakeGetTaskResultJson` 透传到 `GetTaskResult`。详见 [/classes/file-task-store.md](../classes/file-task-store.md)。

## 相关页面

- [/classes/wire-codec.md](../classes/wire-codec.md) — 时代方法集合与 `ValidateTaskHeaders`
- [/classes/mcp-server.md](../classes/mcp-server.md) — 任务分流与扩展声明
- [/classes/mcp-client.md](../classes/mcp-client.md) — 轮询与扩展声明守卫
- [/classes/file-task-store.md](../classes/file-task-store.md) — 持久化格式
- [/concepts/version-negotiation.md](version-negotiation.md) — 门控表
