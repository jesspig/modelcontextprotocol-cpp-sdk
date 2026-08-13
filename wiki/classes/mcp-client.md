---
type: Class
title: McpClient
description: MCP 客户端门面：创建即协商、请求/通知 API、MRTR、响应缓存与任务轮询。
tags: [client, 门面, 协商, mrt]
timestamp: 2026-08-13T12:36:14+08:00
resource: include/mcp/client/McpClient.hpp
---

# McpClient

客户端门面（[McpClient.hpp](../../include/mcp/client/McpClient.hpp)）。**创建即阻塞**：`Create(transport, options)` 构造后立即同步 `NegotiateProtocol()`，返回前协商完成（[McpClient.cpp](../../src/client/McpClient.cpp)）。

## 协商（详见 [/concepts/version-negotiation.md](../concepts/version-negotiation.md)）

| 模式 | 行为 |
|------|------|
| Auto（默认） | 探测 `server/discover`；失败按传输类型分派（见下） |
| Legacy | 强制 initialize 握手 + `notifications/initialized` |
| Pin | 不发送任何探测，版本取 `pin_protocol_version` |

Auto 回退分派（对齐官方 TS SDK，[McpClient.cpp:248](../../src/client/McpClient.cpp)）：

- **stdio 类传输**（RTTI：typeid name 含 `InMemoryTransportImpl`/`StdioClientSessionTransport`）超时/网络失败 → 回退 initialize
- **HTTP 类传输**超时 → `McpError(RequestTimeout)`；网络异常 → `McpError(ConnectionClosed)`
- **-32022 三分支**：`data.supported` 与 `2026-07-28` 有交集 → corrective 重发一次、再失败抛 `UnsupportedProtocolVersion`；仅 legacy → 回退 initialize；无交集 → 抛 `UnsupportedProtocolVersion`
- **-32001/-32020/-32021/-32601 及其他错误码** → 回退 initialize

四个分支协商完成后都调用 `SetNegotiatedProtocolVersion`。

## 行为要点

- `WireClientHandlers()` 仅注册 elicit 请求处理器 + 三个 listChanged 通知（清空 `response_cache_`）；其余通知须自行 `SetNotificationHandler`
- 懒注册：`SetSamplingHandler`/`SetRootsHandler` 未设置 → `MethodNotFound`；`SetLoggingHandler` 未设置 → 静默丢弃
- 响应缓存（SEP-2549）：仅当响应含 `ttlMs > 0` 的 cache hint 才缓存；列表请求在**无 cursor 时**先查缓存；`ExtractCacheHint`/`CacheIfHinted` 兼容两种形态（顶层 `ttlMs`/`cacheScope` 优先，回退嵌套 `cacheHint`）
- MRTR：`SendRequestWithMrtr` 循环 `input_required`，`max_rounds` 超限抛 InternalError、`max_total_timeout` 超限抛 RequestTimeout
- 自动翻页上限 `kMaxAutoPages = 64` 页；任务轮询 500ms 间隔 / 300s 超时（`PollTaskToCompletion` 默认参）
- 超时：任务类请求 600s、Ping 10s（Ping 已标记 deprecated）
- `ClientOptions` 默认：`client_info {"mcp-cpp-client","0.1.0"}`、`initialization_timeout 60s`、`discover_probe_timeout 5s`

## 相关页面

- [/modules/client.md](../modules/client.md) — 所属库
- [/concepts/version-negotiation.md](../concepts/version-negotiation.md)
- [/concepts/mrtr.md](../concepts/mrtr.md) — MRTR 循环
- [/classes/mcp-session-handler.md](mcp-session-handler.md) — 底层引擎
