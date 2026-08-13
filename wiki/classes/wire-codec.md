---
type: Class
title: WireCodec
description: 按协议时代划分的线协议词汇表：方法成员判定、消息校验、meta 处理与错误码映射。
tags: [protocol, codec, 双时代, 2026]
timestamp: 2026-08-14T00:57:41+08:00
resource: src/protocol/WireCodec.cpp
---

# WireCodec

抽象接口（[WireCodec.hpp](../../include/mcp/protocol/WireCodec.hpp)）：`HasRequestMethod / HasNotificationMethod / ValidateRequest / ValidateResponse / ValidateNotification / StampOutgoingRequest / ExtractIncomingMeta / EncodeResult / EncodeErrorCode / Era`。校验结果枚举 `WireValidation {Ok, NotInEra, Invalid}`。方法/通知集合为 `unordered_set<string_view>`（[WireCodec.cpp:17](../../src/protocol/WireCodec.cpp)）。

## 双时代实现

| | Rev2025Codec（"2025-11-25"） | Rev2026Codec（"2026-07-28"） |
|--|--|--|
| 请求校验 | 仅 initialize 检查 `params` 含 protocolVersion/capabilities/clientInfo | 方法不在时代→NotInEra；除 discover 外缺 `_meta`→Invalid |
| 响应校验 | 无额外要求 | 所有响应必须含 `resultType`；4 个列表方法（tools/list、resources/list、resources/templates/list、prompts/list）要求 `"complete"` |
| StampOutgoingRequest | 空操作 | 写 `_meta`：protocolVersion/clientInfo/clientCapabilities |
| ExtractIncomingMeta | 无 override | 无 override——基类为非纯虚默认返回 `nullopt`（WireCodec.hpp:48），meta 解析在 McpSessionHandler 层 |
| EncodeResult | 原样 | 嵌套 `cacheHint` 扁平化为顶层 `ttlMs`/`cacheScope`（移除嵌套键）；无 resultType 补 `"complete"` |
| EncodeErrorCode | 原样 | RequestTimeout→HeaderMismatch；ConnectionRefused→MissingRequiredClientCapability；TlsHandshakeFailed→UnsupportedProtocolVersion |

工厂 `MakeWireCodec`：`version >= "2026-07-28"` → Rev2026，否则 Rev2025。

## 方法注册表

请求方法（集合共 23 个）：

- 公共（8）：tools/list、tools/call、resources/list、resources/read、resources/templates/list、prompts/list、prompts/get、completion/complete
- 2025 独有（13）：initialize、ping、resources/subscribe、resources/unsubscribe、logging/setLevel、roots/list、sampling/createMessage、elicitation/create、tasks/get、tasks/update、tasks/cancel、tasks/result、tasks/list
- 2026 独有（2）：server/discover、subscriptions/listen（2026 合计 10 个请求方法）

通知共 **12 种**：公共 7 + 2025 独有 4（initialized、roots/list_changed、elicitation/complete、tasks/status）+ 2026 独有 1（subscriptions/acknowledged）。2026 = 8 种、2025 = 11 种。5 个 `notifications/tasks/*` 独有通知已从集合移除（[Methods.hpp](../../include/mcp/Methods.hpp) 中常量仍保留）。

2026 时代入站收到 ping、tasks/*、elicitation/create 等不在时代的方法：`ValidateRequest` 判定为 `NotInEra`，McpSessionHandler 回 `-32601 MethodNotFound`（initialize 豁免，[McpSessionHandler.cpp:227](../../src/protocol/McpSessionHandler.cpp)）。

## 相关页面

- [/modules/protocol.md](../modules/protocol.md) — 所属库
- [/classes/mcp-session-handler.md](mcp-session-handler.md) — 引擎消费方
- [/concepts/version-negotiation.md](../concepts/version-negotiation.md) — 时代切换
- [/tests.md](../tests.md) — 测试守护（含无生产调用者的 `ValidateResponse`/`StampOutgoingRequest` 两个方法）
