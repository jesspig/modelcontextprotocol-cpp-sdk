---
type: Class
title: McpServer
description: MCP 服务端门面：注册与分发、能力推导、回调四层接线、任务与 elicitation。
tags: [server, 门面, 注册, 回调]
timestamp: 2026-08-15T03:15:00+08:00
resource: include/mcp/server/McpServer.hpp
---

# McpServer

服务端门面（[McpServer.hpp](../../include/mcp/server/McpServer.hpp)），工厂 `Create(transport, options)`。构造时探测 `transport_->IsStateless()`，配置 `protocol_version` 则立即 `SetNegotiatedProtocolVersion`。

## 注册 API

- `RegisterTool(name, ToolOptions, fn)` / `RegisterResource / RegisterResourceTemplate / RegisterPrompt`：同名覆盖，每次注册后重跑 `WireHandlers()`（拆为 7 个 `Wire*Handlers` 方法）+ `DeriveCapabilities()`；`RegisterTool` 校验工具名 `^[A-Za-z0-9._-]{1,128}$`（`IsValidToolName`，违规抛 `McpError(InvalidParams)`，[McpServer.cpp:88](../../src/server/McpServer.cpp)），同时把 `cached_tools_json_` 置 `nullopt` 失效（[McpServer.cpp:228](../../src/server/McpServer.cpp)）
- 工具/资源/提示词条目内部结构见 [McpServer.hpp](../../include/mcp/server/McpServer.hpp)（ResourceEntry 含 uri_pattern/is_template 等）

## 回调接线（四层）

1. 简写：`on_method_called`、`on_client_connected`、`on_initialized`、`on_protocol_error`
2. 完整消息：`on_request`、`on_response`、`on_error`、`on_notification`
3. 生命周期 + 传输层：`on_transport_close`、`on_transport_error`（经 `dynamic_cast<TransportBase*>` 设置）
4. `incoming_filters` / `outgoing_filters`（FilterPipeline）

`on_request` 与 `on_method_called` 合并进一个回调；`on_error` 与 `on_protocol_error` 合并（后者只传 `err.error.message`）。

## 分发细节

- `tools/call`：异步执行（std::async），future 存 `pending_async_futures_`，`Close()` 先全部 wait；工具声明 `output_schema` 且返回 `structured_content` 时用 `ValidateJsonSchema` 校验
- 分页：`kDefaultPageSize = 100`，cursor 为数字字符串；resources/templates/prompts 三处共用 `PaginateEntries` 模板（含 include 谓词）
- 列表响应缓存提示：按方法名查 `options_.cache_hints`（6 个方法：tools/list、resources/list、resources/templates/list、resources/read、prompts/list、server/discover；`GetCacheHint` 用 `std::less<>` 透明比较器）
- `tools/list` 序列化缓存：`cached_tools_json_` 在 `RegisterTool` 时失效，`HandleListTools` shared/unique 锁 double-check 重建（[McpServer.cpp:743](../../src/server/McpServer.cpp)）
- `HandleInitialize`：已配置 `options_.protocol_version` 时直接采用（可含现代版本），未配置才遍历 `kProtocolVersions` 选非现代公共版本；**未声明（空串）回退 `kDefaultNegotiatedProtocolVersion`，非空未知版本回退 `kLegacyProtocolVersion`**（[McpServer.cpp:1092](../../src/server/McpServer.cpp)）；`result.protocol_version` 回显协商结果
- `HandleDiscover`：无条件置 `initialized_=true`；**协商版本 = `options_.protocol_version`（配置时）否则 `kLatestProtocolVersion`**（[McpServer.cpp:1043](../../src/server/McpServer.cpp)）；支持版本 = `kProtocolVersions` 全表（5 个，2024-11-05 至 2026-07-28）
- tasks 处理器守卫反转：仅 2025 及更早时代可用，`IsModernProtocolVersion` 时回 `MethodNotFound`（[McpServer.cpp:582](../../src/server/McpServer.cpp)）；任务 wire 状态为官方字符串 `TaskStatusToWireString`（working/input_required/completed/failed/cancelled，`Pending→working`，[McpServer.cpp:70](../../src/server/McpServer.cpp)）；`GetTaskResult` 填充提取为 `MakeGetTaskResultJson`（含 include_optional_fields 开关）；`tasks/update` 完成时发 `tasks/completed` 或 `tasks/working` 通知、`tasks/cancel` 发 `tasks/cancelled` 通知；公开方法 `SendTaskStatus(task_id, status)` 直接发送 `tasks/status` 通知（[McpServer.hpp:86](../../include/mcp/server/McpServer.hpp)）
- `subscriptions/listen`：仅现代版本，订阅 ID 单调分配（原子量从 1 起）；订阅后**同步回发 `subscriptions/acknowledged` 通知帧**（`SendSubscriptionsAcknowledged`，[McpServer.cpp:1170](../../src/server/McpServer.cpp)）——`honored` 回显 filter、meta 带 `protocolVersion` + `subscriptionId`（优先客户端 `_meta` 传入 ID，未设置回退服务端自增 ID）
- `SendLoggingMessage`：低于当前级别直接丢弃，logger 固定 `"mcp-server"`
- `Elicit`：无 config 时超时 600s；结果 `code` 为负抛 McpError
- `GetClientCapabilities()/GetClientInfo()` 返回 `shared_ptr<const T>`；`GetNegotiatedProtocolVersion()` 返回 `std::string`（转发自 handler）

## 相关页面

- [/modules/server.md](../modules/server.md) — 所属库
- [/classes/mcp-session-handler.md](mcp-session-handler.md) — 底层引擎
- [/concepts/mrtr.md](../concepts/mrtr.md) — 服务端 elicitation
- [/classes/file-task-store.md](file-task-store.md) — 任务存储集成
