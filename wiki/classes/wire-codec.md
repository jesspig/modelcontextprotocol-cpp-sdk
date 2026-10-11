---
type: Class
title: WireCodec
description: 按协议时代划分的线协议词汇表：方法成员判定、消息校验、meta 处理与错误码映射。
tags: [protocol, codec, 双时代, 2026, meta, tasks, skills]
timestamp: 2026-10-11T16:35:00+08:00
resource: src/protocol/WireCodec.cpp
---

# WireCodec

抽象接口（[WireCodec.hpp](../../include/mcp/protocol/WireCodec.hpp)）：`HasRequestMethod / HasNotificationMethod / ValidateRequest / ValidateResponse / ValidateNotification / ValidateTaskHeaders / StampOutgoingRequest / ExtractIncomingMeta / EncodeResult / EncodeErrorCode / Era`。校验结果枚举 `WireValidation {Ok, NotInEra, Invalid}`。方法/通知集合为 `unordered_set<string_view>`（[WireCodec.cpp:16](../../src/protocol/WireCodec.cpp)）。

## 双时代实现

| | Rev2025Codec（"2025-11-25"） | Rev2026Codec（"2026-07-28"） |
|--|--|--|
| 请求校验 | 仅 initialize 检查 `params` 含 protocolVersion/capabilities/clientInfo | 方法不在时代→NotInEra；除 discover 外缺 `params._meta`→Invalid |
| 响应校验 | 无额外要求 | 所有响应必须含 `resultType`；5 个列表方法（tools/list、resources/list、resources/templates/list、prompts/list、skills/list）要求 `"complete"` |
| StampOutgoingRequest | 空操作 | 写 `params._meta`：protocolVersion/clientInfo/clientCapabilities |
| ExtractIncomingMeta | 无 override | 无 override——基类为非纯虚默认返回 `nullopt`（WireCodec.hpp:48），meta 解析在 McpSessionHandler 层 |
| EncodeResult | 原样 | 嵌套 `cacheHint` 扁平化为顶层 `ttlMs`/`cacheScope`（移除嵌套键）；无 resultType 补 `"complete"` |
| EncodeErrorCode | 原样 | 内部错误（RequestTimeout/ConnectionRefused/TlsHandshakeFailed）记日志并映射为 InternalError(-32603)；协议码（-32020/-32021/-32022/-32042）原样传递 |

工厂 `MakeWireCodec`：`version >= "2026-07-28"` → Rev2026，否则 Rev2025。

## 方法注册表

请求方法（并集共 26 个）：

- 公共（8）：tools/list、tools/call、resources/list、resources/read、resources/templates/list、prompts/list、prompts/get、completion/complete
- 2025 独有（13）：initialize、ping、resources/subscribe、resources/unsubscribe、logging/setLevel、roots/list、sampling/createMessage、elicitation/create、tasks/get、tasks/update、tasks/cancel、tasks/result、tasks/list
- 2026 独有（8）：server/discover、subscriptions/listen、tasks/get、tasks/update、tasks/cancel、skills/list、skills/get、resources/directory/read（2026 合计 16 个请求方法）
- 双时代交集：tasks/get、tasks/update、tasks/cancel 同时属于 2025 与 2026 集合（SEP-2663 任务扩展）；tasks/result、tasks/list 仍仅 2025（`kLegacyGetTaskPayload`/`kLegacyListTasks`）；skills/list、skills/get、resources/directory/read 仅 2026（SEP-2640 技能扩展）

通知共 **17 种**：公共 7 + 2025 独有 9（initialized、roots/list_changed、elicitation/complete、tasks/status、tasks/working、tasks/completed、tasks/failed、tasks/cancelled、tasks/input_required）+ 2026 独有 1（subscriptions/acknowledged）。2026 = 8 种、2025 = 16 种。6 个 `notifications/tasks/*` 独有通知于 2026-08-15 加回 2025-only 集合（服务端 `SendTaskStatus`/任务完成通知使用，[WireCodec.cpp:50](../../src/protocol/WireCodec.cpp)）。

2026 时代入站收到 ping、tasks/result、tasks/list、elicitation/create 等不在时代的方法：`ValidateRequest` 判定为 `NotInEra`，McpSessionHandler 回 `-32601 MethodNotFound`（initialize 豁免，[McpSessionHandler.cpp:245](../../src/protocol/McpSessionHandler.cpp)）。tasks/get、tasks/update、tasks/cancel 在 2026 时代合法，不再拒绝。

## 任务头校验

`WireCodec::ValidateTaskHeaders(method, params, mcp_name)`（基类虚方法，2025/2026 两 codec 共享实现）：仅对 tasks/get、tasks/update、tasks/cancel 生效，其余方法直接返回。要求 `params.taskId` 为非空字符串（缺失/空 → `McpError(InvalidParams, "tasks request missing valid taskId")`），且与 `Mcp-Name` 头值**精确相等**（不等 → `McpError(HeaderMismatch, "Mcp-Name header does not match body params taskId")`）。调用方为 McpServer 的 tasks 三处理器（modern era 且请求携带 `Mcp-Name` 时）与 Streamable HTTP 服务端的 `ValidateMcpHeaders`（HTTP 层 400 `HeaderMismatch`），客户端对 tasks 三方法按 `params.taskId` 发送 `Mcp-Name` 头。详见 [/concepts/tasks-extension.md](../concepts/tasks-extension.md)。

## 相关页面

- [/modules/protocol.md](../modules/protocol.md) — 所属库
- [/classes/mcp-session-handler.md](mcp-session-handler.md) — 引擎消费方
- [/concepts/version-negotiation.md](../concepts/version-negotiation.md) — 时代切换
- [/concepts/tasks-extension.md](../concepts/tasks-extension.md) — 任务扩展三方法与头校验
- [/concepts/skills-extension.md](../concepts/skills-extension.md) — 技能扩展三方法
- [/tests.md](../tests.md) — 测试守护（含无生产调用者的 `ValidateResponse`/`StampOutgoingRequest` 两个方法）
