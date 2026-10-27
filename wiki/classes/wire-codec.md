---
type: Class
title: WireCodec
description: 按协议时代划分的线协议词汇表：方法成员判定、消息校验、meta 处理与错误码映射。
tags: [protocol, codec, 双时代, 2026, meta, tasks, skills]
timestamp: 2026-10-27T04:48:00+08:00
resource: src/protocol/WireCodec.cpp
---

# WireCodec

抽象接口（[WireCodec.hpp](../../include/mcp/protocol/WireCodec.hpp)）：`HasRequestMethod / HasNotificationMethod / ValidateRequest / ValidateResponse / ValidateNotification / ValidateTaskHeaders / StampOutgoingRequest / ExtractIncomingMeta / EncodeResult / EncodeErrorCode / Era`。校验结果枚举 `WireValidation {Ok, NotInEra, Invalid}`。方法/通知集合为 `unordered_set<string_view>`（[WireCodec.cpp:16](../../src/protocol/WireCodec.cpp)）。

## 双时代实现

| | Rev2025Codec（"2025-11-25"） | Rev2026Codec（"2026-07-28"） |
|--|--|--|
| 请求校验 | 方法不在时代→NotInEra；仅 initialize 检查 `params` 含 protocolVersion/capabilities/clientInfo | 方法不在时代→NotInEra；除 discover 外缺 `params._meta`→Invalid（`WireMessageView` 的 `meta_backfilled` 视为已有） |
| 响应校验 | 无额外要求 | 所有响应必须含 `resultType`；5 个列表方法（tools/list、resources/list、resources/templates/list、prompts/list、skills/list）要求 `"complete"` |
| 通知校验 | 通知不在时代→NotInEra | 通知不在时代→NotInEra；含 id/result/error→Invalid |
| StampOutgoingRequest | 空操作 | 写 `params._meta`：protocolVersion/clientInfo/clientCapabilities |
| ExtractIncomingMeta | 无 override | 无 override——基类为非纯虚默认返回 `nullopt`（WireCodec.hpp:48），meta 解析在 McpSessionHandler 层 |
| EncodeResult | 原样 | 嵌套 `cacheHint` 扁平化为顶层 `ttlMs`/`cacheScope`（移除嵌套键）；无 resultType 补 `"complete"` |
| EncodeErrorCode | 原样 | 内部错误（RequestTimeout/ConnectionRefused/TlsHandshakeFailed）记日志并映射为 InternalError(-32603)；协议码（-32020/-32021/-32022/-32042）原样传递 |

两代 codec 均提供 `WireMessageView` 重载（`ValidateRequest/ValidateNotification`，默认转发原 `JsonValue` 签名）：视图把 `jsonrpc/method/params/_meta` 组装为线格式并允许 `_meta` 由传输/会话层注入（`meta_backfilled`），Rev2026 覆写视图版校验——`_meta` 不必物理存在于 `params` 中。`MakeWireRequestView/MakeWireNotificationView` 由 `JsonRpcRequest/JsonRpcNotification` 构造视图。

工厂 `MakeWireCodec`：`version >= "2026-07-28"` → Rev2026，否则 Rev2025。

## 方法注册表

请求方法（并集共 27 个）：

- 公共（9）：tools/list、tools/call、resources/list、resources/read、resources/templates/list、prompts/list、prompts/get、completion/complete、server/extensions/list
- 2025 独有（13）：initialize、ping、resources/subscribe、resources/unsubscribe、logging/setLevel、roots/list、sampling/createMessage、elicitation/create、tasks/get、tasks/update、tasks/cancel、tasks/result、tasks/list
- 2026 独有（8）：server/discover、subscriptions/listen、tasks/get、tasks/update、tasks/cancel、skills/list、skills/get、resources/directory/read（2026 合计 16 个请求方法）
- 双时代交集：tasks/get、tasks/update、tasks/cancel 同时属于 2025 与 2026 集合（SEP-2663 任务扩展）；tasks/result、tasks/list 仍仅 2025（`kLegacyGetTaskPayload`/`kLegacyListTasks`）；skills/list、skills/get、resources/directory/read 仅 2026（SEP-2640 技能扩展）

通知共 **18 种**：公共 15（cancelled、progress、message、resources/updated、resources/list_changed、tools/list_changed、prompts/list_changed、roots/list_changed、tasks/status、tasks/working、tasks/completed、tasks/failed、tasks/cancelled、tasks/input_required、triggers/event）+ 2025 独有 2（initialized、elicitation/complete）+ 2026 独有 1（subscriptions/acknowledged）。任务 6 通知与 triggers/event 为两代共用（服务端实际在发，[WireCodec.cpp](../../src/protocol/WireCodec.cpp)）。

2026 时代入站收到 ping、tasks/result、tasks/list、elicitation/create 等不在时代的方法：`ValidateRequest` 判定为 `NotInEra`，McpSessionHandler 回 `-32601 MethodNotFound`（未协商会话的握手入口 initialize/discover 豁免，[McpSessionHandler.cpp](../../src/protocol/McpSessionHandler.cpp)）；2025 时代同样按集合拒绝时代外方法（如 skills 族）。通知不在时代时在会话层静默丢弃并记 Warning。

## 任务头校验

`WireCodec::ValidateTaskHeaders(method, params, mcp_name)`（基类虚方法，2025/2026 两 codec 共享实现）：仅对 tasks/get、tasks/update、tasks/cancel 生效，其余方法直接返回。要求 `params.taskId` 为非空字符串（缺失/空 → `McpError(InvalidParams, "tasks request missing valid taskId")`），且与 `Mcp-Name` 头值**精确相等**（不等 → `McpError(HeaderMismatch, "Mcp-Name header does not match body params taskId")`）。调用方为 McpServer 的 tasks 三处理器（modern era 且请求携带 `Mcp-Name` 时）与 Streamable HTTP 服务端的 `ValidateMcpHeaders`（HTTP 层 400 `HeaderMismatch`），客户端对 tasks 三方法按 `params.taskId` 发送 `Mcp-Name` 头。详见 [/concepts/tasks-extension.md](../concepts/tasks-extension.md)。

## 相关页面

- [/modules/protocol.md](../modules/protocol.md) — 所属库
- [/classes/mcp-session-handler.md](mcp-session-handler.md) — 引擎消费方
- [/concepts/version-negotiation.md](../concepts/version-negotiation.md) — 时代切换
- [/concepts/tasks-extension.md](../concepts/tasks-extension.md) — 任务扩展三方法与头校验
- [/concepts/skills-extension.md](../concepts/skills-extension.md) — 技能扩展三方法
- [/tests.md](../tests.md) — 测试守护（含无生产调用者的 `ValidateResponse`/`StampOutgoingRequest` 两个方法）
