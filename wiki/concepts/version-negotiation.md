---
type: Concept
title: 版本协商
description: 2025（initialize）与 2026（server/discover）双时代协议版本选择与 codec 重建。
tags: [协议, 版本, 协商, 2026]
timestamp: 2026-08-14T23:52:10+08:00
resource: include/mcp/client/VersionNegotiation.hpp
---

# 版本协商

支持版本数组共 5 个（"2024-11-05" / "2025-03-26" / "2025-06-18" / "2025-11-25" / "2026-07-28"），[ProtocolVersion.hpp](../../include/mcp/ProtocolVersion.hpp)。现代判定：`IsModernProtocolVersion(v)` = 字典序 `>= "2026-07-28"`。

## 核心规则

- **现代版本（2026-07-28+）绝不通过 `initialize` 协商**——只能通过 `server/discover`
- **`HandleInitialize` 在支持表中回显客户端的旧版版本号**：客户端版本命中支持表（且非现代）时返回客户端发送的版本（切勿返回 `kLatestProtocolVersion`——TS SDK v2 会校验 `result.protocolVersion` 是否在其旧版列表中）；**未声明（空串）回退 `kDefaultNegotiatedProtocolVersion`**（"2025-03-26"，对齐 5 语言的 `DEFAULT_NEGOTIATED_PROTOCOL_VERSION`）；**非空但未知版本回退 `kLegacyProtocolVersion`**（"2025-11-25"，对齐 python `LATEST_HANDSHAKE_VERSION` 与 rust 服务端默认）
- `server/discover` 支持版本为 `kProtocolVersions` 全表（5 个，2024-11-05 至 2026-07-28），并**无条件置 `initialized_=true`**
- 每次协商后 `SetNegotiatedProtocolVersion` 重建 WireCodec（`shared_ptr<WireCodec>` + `codec_mutex_`，原子交换 `shared_ptr<const std::string>`，线程安全，消息循环运行中可调用）；`NegotiatedProtocolVersion()` 锁下拷贝返回 `std::string`
- **`initialize` 在 2026 时代豁免**：入站验证遇 `NotInEra` 时仅拒绝非 initialize 请求，现代服务端仍须应答遗留握手（[McpSessionHandler.cpp](../../src/protocol/McpSessionHandler.cpp:227)）

## 客户端三种连接模式

| 模式 | 行为 | 结果 |
|------|------|------|
| Auto（默认） | 探测 `server/discover`（`discover_probe_timeout` 默认 5s），失败处理按传输分类（见下） | 成功 → 现代；回退 → legacy |
| Legacy | 强制 initialize + `notifications/initialized` | legacy |
| Pin | 不发探测，版本取 `pin_protocol_version` | 现代（壳 DiscoverResult） |

## Auto 模式回退语义（对齐 TS SDK probeClassifier）

`ProbeDiscover` 先用 **RTTI**（`typeid` name 子串）分类传输：`InMemoryTransportImpl` / `StdioClientSessionTransport` → **stdio 类**；其余（streamable-http/sse/websocket）→ **网络类**。

| 场景 | stdio 类 | 网络类 |
|------|---------|--------|
| probe 超时 | 回退 initialize | 抛 `McpError(RequestTimeout)` |
| future 异常 | 回退 initialize | 抛 `McpError(ConnectionClosed)` |
| `-32022` 且 `data.supported` 含 "2026-07-28" | corrective 用共享版本重发一次；再失败抛 `UnsupportedProtocolVersion` | 同左 |
| `-32022` 且 supported 仅 legacy | 回退 initialize | 同左 |
| `-32022` 且 supported 含现代但无交集 | 抛 `UnsupportedProtocolVersion` | 同左 |
| `-32022` 且 data 缺失/畸形 | 回退 initialize | 同左 |
| `-32001` / `-32020` / `-32021` / `-32601` 及其他错误码 | 回退 initialize | 同左 |

（[McpClient.cpp](../../src/client/McpClient.cpp:248)）

## 时代差异

详见 [/classes/wire-codec.md](../classes/wire-codec.md)：2026 无状态（每请求 `_meta` 携带版本/客户端信息）、`subscriptions/listen` 取代 `resources/subscribe`、tasks 系列方法仅 2025 存在（2026 时代被 `NotInEra` 拒绝，入站验证抛 MethodNotFound）、2025 通知（`notifications/initialized` 等 4 种）在 2026 时代无效。

## 相关页面

- [/classes/mcp-client.md](../classes/mcp-client.md) — 协商执行方
- [/classes/wire-codec.md](../classes/wire-codec.md) — 时代编解码
- [/modules/core.md](../modules/core.md) — 版本常量定义
