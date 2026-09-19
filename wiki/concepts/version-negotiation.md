---
type: Concept
title: 版本协商
description: 2025（initialize）与 2026（server/discover）双时代协议版本选择、supportedVersions 交集、codec 重建与 HTTP 版本头自学习。
tags: [协议, 版本, 协商, 2026]
timestamp: 2026-09-20T01:45:19+08:00
resource: include/mcp/client/VersionNegotiation.hpp
---

# 版本协商

支持版本数组共 5 个（"2024-11-05" / "2025-03-26" / "2025-06-18" / "2025-11-25" / "2026-07-28"），[ProtocolVersion.hpp](../../include/mcp/ProtocolVersion.hpp)。现代判定：`IsModernProtocolVersion(v)` = 字典序 `>= "2026-07-28"`。

## 核心规则

- **现代版本（2026-07-28+）绝不通过 `initialize` 协商**——只能通过 `server/discover`
- **`HandleInitialize` 在支持表中回显客户端的旧版版本号**：客户端版本命中支持表（且非现代）时返回客户端发送的版本（切勿返回 `kLatestProtocolVersion`——TS SDK v2 会校验 `result.protocolVersion` 是否在其旧版列表中）；**未声明（空串）回退 `kDefaultNegotiatedProtocolVersion`**（"2025-03-26"，对齐 5 语言的 `DEFAULT_NEGOTIATED_PROTOCOL_VERSION`）；**非空但未知版本回退 `kLegacyProtocolVersion`**（"2025-11-25"，对齐 python `LATEST_HANDSHAKE_VERSION` 与 rust 服务端默认）
- `server/discover` 支持版本为 `kProtocolVersions` 全表（5 个，2024-11-05 至 2026-07-28），并**无条件置 `initialized_=true`**；服务端 `HandleDiscover` 总是回 `serverInfo`（`options_.server_info` 缺失回退 `{"mcp-server", kSdkVersion}`）；discover 响应**解析侧**（客户端 `DeserializeDiscoverResult`）对 `serverInfo` 容缺——顶层缺失时从 `_meta["io.modelcontextprotocol/serverInfo"]` 提取（官方 TS 服务器即此形态，[McpTypesResults.cpp](../../src/core/McpTypesResults.cpp)）
- **客户端 discover 响应的版本交集**（[McpClient.cpp](../../src/client/McpClient.cpp)）：`DeclaresSharedClientVersion` 判定响应 `supportedVersions` 与客户端支持表（`kProtocolVersions`）是否相交——字段缺失/非数组视为**未声明**，接受探测版本；`SelectSharedVersion` 从交集中取客户端支持的**最新**版本（数组从新到旧找第一个命中）；声明的列表为空或无交集则回退 `initialize`；仅字段未声明时保留探测版本（服务器应答探测即隐式接受）
- 每次协商后 `SetNegotiatedProtocolVersion` 重建 WireCodec（`shared_ptr<WireCodec>` + `codec_mutex_`，`shared_mutex` 内整体交换 `negotiated_version_` 与 `codec_`，线程安全，消息循环运行中可调用）；`NegotiatedProtocolVersion()` 锁下拷贝返回 `std::string`
- **`initialize` 在 2026 时代豁免**：入站验证遇 `NotInEra` 时仅拒绝非 initialize 请求，现代服务端仍须应答遗留握手（[McpSessionHandler.cpp](../../src/protocol/McpSessionHandler.cpp:244)）

## 服务端拒绝不受支持版本

`McpSessionHandler` 在派发前检查非 `initialize` 请求的 `_meta.protocolVersion`：不在 `kProtocolVersions` 内（`IsSupportedProtocolVersion`）即回 `UnsupportedProtocolVersion`（-32022），`data` 同时携带 `requested` 与服务端 `supported` 列表（[McpSessionHandler.cpp](../../src/protocol/McpSessionHandler.cpp:308)）。上表客户端 Auto 模式的 corrective 重试正是针对该错误码。

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

（[McpClient.cpp](../../src/client/McpClient.cpp:329)）

## Streamable HTTP 客户端的版本头自学习

transport 层无协商状态，改为**从流量中学习**（[StreamableHttpClientTransport.cpp:52](../../src/http/StreamableHttpClientTransport.cpp)）：

- initialize 请求**不带** `MCP-Protocol-Version` 头（版本尚未确定，由 body 的 `params.protocolVersion` 表达）
- 从 initialize 响应 `result.protocolVersion` 学习协商版本——**只认 initialize 的 POST 响应**（单 JSON 体或 SSE 块；GET 监听流不参与，`DispatchSseBlock` 仅在 `is_initialize` 时学习，`NegotiatedVersionFromResponse`）
- 后续所有请求与 GET 接收流按学习到的版本携带 `MCP-Protocol-Version` 头；尚无学习值时兜底 `2026-07-28`（`EffectiveProtocolVersion`）

## 客户端通知的版本门控

`McpClient::SendRootsListChanged()`（notifications/roots/list_changed）要求协商版本 **>= 2025-06-18**（按 `kProtocolVersions` 序位比较），不满足抛 `McpError(ProtocolViolation)`。

## 时代差异

详见 [/classes/wire-codec.md](../classes/wire-codec.md)：2026 无状态（每请求 `_meta` 携带版本/客户端信息）、`subscriptions/listen` 取代 `resources/subscribe`、tasks 系列方法仅 2025 存在（2026 时代被 `NotInEra` 拒绝，入站验证抛 MethodNotFound）、2025 通知（`notifications/initialized`、`notifications/tasks/*` 等 9 种）在 2026 时代无效。

## 官方互通验证（TS conformance 服务器，端口 3010）

- **Auto 模式 modern era（2026-07-28）首次走通**：discover（serverInfo 容缺 + 版本交集）→ tools/list → tools/call
- **Legacy 模式（2025-11-25）form elicitation 官方互通全链路**：`elicitation/create` → 客户端 handler → `ElicitResult` `content` 数据被官方服务器消费（wire 键已由 `values` 修正为 `content`，对齐规范与官方服务器）
- modern era 下 TS 服务器自身按规范拒绝 server→client 请求（`elicitation/create not supported in wire era 2026-07-28`），属官方正确行为

## 相关页面

- [/classes/mcp-client.md](../classes/mcp-client.md) — 协商执行方
- [/classes/wire-codec.md](../classes/wire-codec.md) — 时代编解码
- [/modules/core.md](../modules/core.md) — 版本常量定义
