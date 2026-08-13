---
type: Module
title: mcp-server 服务端库
description: McpServer 门面：注册工具/资源/提示词、请求分发、能力推导、任务存储集成。
tags: [server, 工具注册, 资源, 提示词, 任务]
timestamp: 2026-08-13T12:36:14+08:00
resource: src/server/McpServer.cpp
---

# mcp-server 服务端库

依赖 `mcp-protocol`。对外提供 `McpServer`（见 [/classes/mcp-server.md](../classes/mcp-server.md)）。

## 注册 API

- `RegisterTool(name, ToolOptions, fn)` / `RegisterResource / RegisterResourceTemplate / RegisterPrompt`——每次注册后重跑 `WireHandlers()` + `DeriveCapabilities()`
- 任务不注册，由 `ServerOptions::task_store` 驱动
- 能力推导（[McpServer.cpp](../../src/server/McpServer.cpp)）：有工具→`tools`（list_changed）、有资源→`resources`（subscribe + list_changed）、有提示词→`prompts`、有 task_store→`extensions = {}`

## WireHandlers 方法清单

`WireHandlers()` 拆分为 7 个接线方法（[McpServer.cpp:386](../../src/server/McpServer.cpp)）：

- **WireToolHandlers**：`tools/list`（有工具时）、`tools/call`（无条件）
- **WireResourceHandlers**：`resources/list`（有非模板资源时）、`resources/templates/list`（有模板时）、`resources/read`（有资源时）、`resources/subscribe|unsubscribe`（有资源时，2025-era）
- **WirePromptHandlers**：`prompts/list`（有提示词时）、`prompts/get`（无条件）
- **WireCoreHandlers**：`initialize`、`server/discover`、`ping`、`logging/setLevel`、`completion/complete` + 通知 `notifications/initialized`（置 `initialized_`）、`notifications/progress`（延长超时截止）
- **WireExtensionHandlers**：`server/extensions/list`
- **WireTaskHandlers**：`tasks/get/update/cancel/result/list`——仅 `options_.task_store` 存在时注册，且**仅 2025 及更早时代可用**（`IsModernProtocolVersion` 时回 `MethodNotFound`，[McpServer.cpp:576](../../src/server/McpServer.cpp)）
- **WireSubscriptionHandlers**：`subscriptions/listen`（2026-era）

`initialized_` 标志守护除 `initialize`/`server/discover`/`subscriptions/listen`/`tasks/*` 外的所有处理器（现代协议经 discover 直接视为已初始化）。

## 失败语义

- 工具不存在 → `InvalidParams "tool not found: <name>"`
- 非法 cursor → `InvalidParams "invalid cursor: ..."`
- 任务不存在 → `InvalidParams "task not found: <id>"`；store 抛异常 → `InternalError "task persist failed: ..."`
- `GetClientCapabilities()/GetClientInfo()` 返回 `shared_ptr<const T>`，调用方须持有返回值再访问

## 实现要点

- 任务状态 wire 值用官方字符串（`TaskStatusToWireString`：working/input_required/completed/failed/cancelled，`Pending→working`）；FileTaskStore 磁盘持久化仍为数字
- `tools/list` 序列化缓存 `cached_tools_json_`：`RegisterTool` 置 `nullopt` 失效，`HandleListTools` double-check 重建
- 分页循环提取为 `PaginateEntries` 模板（resources/templates/prompts 三处共用）
- 任务结果填充提取为 `MakeGetTaskResultJson`；cache hint 查询用 `GetCacheHint`（`std::less<>` 透明比较器）

## 相关页面

- [/classes/mcp-server.md](../classes/mcp-server.md) — 门面实现细节
- [/classes/file-task-store.md](../classes/file-task-store.md) — 任务存储
- [/concepts/mrtr.md](../concepts/mrtr.md) — 服务端 elicitation
- [/modules/protocol.md](protocol.md) — 依赖的下层
