---
type: Class
title: WireCodec
description: 按协议时代划分的线协议词汇表：方法成员判定、消息校验、meta 处理与错误码映射。
tags: [protocol, codec, 双时代, 2026]
timestamp: 2026-08-13T03:25:00+08:00
resource: src/protocol/WireCodec.cpp
---

# WireCodec

抽象接口（[WireCodec.hpp](../../include/mcp/protocol/WireCodec.hpp)）：`HasRequestMethod / HasNotificationMethod / ValidateRequest / ValidateResponse / ValidateNotification / StampOutgoingRequest / ExtractIncomingMeta / EncodeResult / EncodeErrorCode / Era`。校验结果枚举 `WireValidation {Ok, NotInEra, Invalid}`。

## 双时代实现

| | Rev2025Codec（"2025-11-25"） | Rev2026Codec（"2026-07-28"） |
|--|--|--|
| 请求校验 | 仅 initialize 必须含 protocolVersion/capabilities/clientInfo | 方法不在时代→NotInEra；除 discover 外缺 `_meta`→Invalid |
| 响应校验 | 无额外要求 | 必须含 `resultType`；6 个列表方法要求 `"complete"` |
| StampOutgoingRequest | 空操作 | 写 `_meta`：protocolVersion/clientInfo/clientCapabilities |
| ExtractIncomingMeta | 恒 nullopt | 读 `_meta` 的 6 个协议键 |
| EncodeResult | 原样 | 无 resultType 补 `"complete"` |
| EncodeErrorCode | 原样 | RequestTimeout→HeaderMismatch；ConnectionRefused→MissingRequiredClientCapability；TlsHandshakeFailed→UnsupportedProtocolVersion |

工厂 `MakeWireCodec`：`version >= "2026-07-28"` → Rev2026，否则 Rev2025。

## 方法注册表

- 公共请求（10）：ping、tools/list、tools/call、resources/list、resources/read、resources/templates/list、prompts/list、prompts/get、completion/complete、elicitation/create
- 2025 独有请求（6）：initialize、resources/subscribe、resources/unsubscribe、logging/setLevel、roots/list、sampling/createMessage
- 2026 独有请求（8）：server/discover、server/extensions/list、subscriptions/listen、tasks/get、tasks/update、tasks/cancel、tasks/result、tasks/list
- 通知共 **17 种**：公共 7 + 2025 独有 4 + 2026 独有 6（tasks 系列）

## 相关页面

- [/modules/protocol.md](../modules/protocol.md) — 所属库
- [/classes/mcp-session-handler.md](mcp-session-handler.md) — 引擎消费方
- [/concepts/version-negotiation.md](../concepts/version-negotiation.md) — 时代切换
- [/tests.md](../tests.md) — 测试守护（含无生产调用者的三个方法）
