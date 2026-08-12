---
type: Concept
title: 版本协商
description: 2025（initialize）与 2026（server/discover）双时代协议版本选择与 codec 重建。
tags: [协议, 版本, 协商, 2026]
timestamp: 2026-08-13T03:25:00+08:00
resource: include/mcp/client/VersionNegotiation.hpp
---

# 版本协商

支持版本数组共 5 个（"2024-11-05" / "2025-03-26" / "2025-06-18" / "2025-11-25" / "2026-07-28"），[ProtocolVersion.hpp](../../include/mcp/ProtocolVersion.hpp)。现代判定：`IsModernProtocolVersion(v)` = 字典序 `>= "2026-07-28"`。

## 核心规则

- **现代版本（2026-07-28+）绝不通过 `initialize` 协商**——只能通过 `server/discover`
- **`HandleInitialize` 必须回显客户端的旧版版本号**：返回客户端发送的版本（切勿返回 `kLatestProtocolVersion`——TS SDK v2 会校验 `result.protocolVersion` 是否在其旧版列表中）
- `server/discover` 支持版本固定为 `{kLegacy, kLatest}`，并**无条件置 `initialized_=true`**
- 每次协商后 `SetNegotiatedProtocolVersion` 重建 WireCodec（`shared_ptr<WireCodec>` + `codec_mutex_`，线程安全，消息循环运行中可调用）

## 客户端三种连接模式

| 模式 | 行为 | 结果 |
|------|------|------|
| Auto（默认） | 探测 `server/discover`（`discover_probe_timeout` 默认 5s），**任何失败（含超时）都回退** | 成功 → 现代；失败 → legacy |
| Legacy | 强制 initialize + `notifications/initialized` | legacy |
| Pin | 不发探测，版本取 `pin_protocol_version` | 现代（壳 DiscoverResult） |

> 头文件注释声明"discover 失败（-32022/-32601/超时）→ 回退"，实际实现是**任何失败都回退**。

## 时代差异

详见 [/classes/wire-codec.md](../classes/wire-codec.md)：2026 无状态（每请求 `_meta` 携带版本/客户端信息）、`subscriptions/listen` 取代 `resources/subscribe`、tasks 系列方法仅 2026 存在、2025 通知（`notifications/initialized` 等 4 种）在 2026 时代无效。

## 相关页面

- [/classes/mcp-client.md](../classes/mcp-client.md) — 协商执行方
- [/classes/wire-codec.md](../classes/wire-codec.md) — 时代编解码
- [/modules/core.md](../modules/core.md) — 版本常量定义
