---
type: Module
title: mcp-server 服务端库
description: McpServer 门面：注册工具/资源/提示词、请求分发、能力推导、任务存储集成。
tags: [server, 工具注册, 资源, 提示词, 任务]
timestamp: 2026-08-13T03:25:00+08:00
resource: src/server/McpServer.cpp
---

# mcp-server 服务端库

依赖 `mcp-protocol`。对外提供 `McpServer`（见 [/classes/mcp-server.md](../classes/mcp-server.md)）。

## 注册 API

- `RegisterTool(name, ToolOptions, fn)` / `RegisterResource / RegisterResourceTemplate / RegisterPrompt`——每次注册后重跑 `WireHandlers()` + `DeriveCapabilities()`
- 任务不注册，由 `ServerOptions::task_store` 驱动
- 能力推导（[McpServer.cpp](../../src/server/McpServer.cpp)）：有工具→`tools`（list_changed）、有资源→`resources`（subscribe + list_changed）、有提示词→`prompts`、有 task_store→`extensions = {}`

## WireHandlers 方法清单

- 条件注册（仅当存在对应注册项）：`tools/list`、`resources/list`、`resources/templates/list`、`resources/read`、`prompts/list`、`resources/subscribe`/`resources/unsubscribe`（2025-era）
- 无条件注册：`tools/call`、`prompts/get`、`initialize`、`server/discover`、`ping`、`server/extensions/list`、`completion/complete`、`subscriptions/listen`、`logging/setLevel`
- 任务方法（仅 `options_.task_store` 存在时）：`tasks/get/update/cancel/result/list`，非现代版本回 `MethodNotFound`
- 通知处理器：`notifications/initialized`（置 `initialized_`）、`notifications/progress`（延长超时截止）

`initialized_` 标志守护除 `initialize`/`server/discover`/`subscriptions/listen`/`tasks/*` 外的所有处理器（现代协议经 discover 直接视为已初始化）。

## 失败语义

- 工具不存在 → `InvalidParams "tool not found: <name>"`
- 非法 cursor → `InvalidParams "invalid cursor: ..."`
- 任务不存在 → `InvalidParams "task not found: <id>"`；store 抛异常 → `InternalError "task persist failed: ..."`
- `GetClientCapabilities()/GetClientInfo()` 返回 `shared_ptr<const T>`，调用方须持有返回值再访问

## 相关页面

- [/classes/mcp-server.md](../classes/mcp-server.md) — 门面实现细节
- [/classes/file-task-store.md](../classes/file-task-store.md) — 任务存储
- [/concepts/mrtr.md](../concepts/mrtr.md) — 服务端 elicitation
- [/modules/protocol.md](protocol.md) — 依赖的下层
