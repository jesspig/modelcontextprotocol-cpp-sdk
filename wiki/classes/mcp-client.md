---
type: Class
title: McpClient
description: MCP 客户端门面：创建即协商、请求/通知 API、MRTR、响应缓存与任务轮询。
tags: [client, 门面, 协商, mrt]
timestamp: 2026-08-15T03:15:00+08:00
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

- `WireClientHandlers()` 注册 5 个通知处理器（[McpClient.cpp:484](../../src/client/McpClient.cpp)）：三个 listChanged → `response_cache_->Clear()`；`resources/updated` → 按 uri 键**单键失效**（`Invalidate`，其余缓存保留）；`subscriptions/acknowledged` → 匹配 `SubscribeAsync` 待确认订阅并转发用户处理器（经 `SetNotificationHandler` 特判存储的 `user_ack_notification_handler_`，不覆盖内部逻辑）；另注册 elicit 请求处理器
- 懒注册：`SetSamplingHandler`/`SetRootsHandler` 未设置 → `MethodNotFound`；`SetLoggingHandler` 未设置 → 静默丢弃
- 响应缓存（SEP-2549）：键 = `CacheKey(method, context)`——列表方法带 cursor 键（`<method>\x1F<cursor>`，无 cursor 为空串），`resources/read` 带 uri 键；`ttlMs > 0` 才缓存，TTL **钳制 24h**（`kMaxTtl`）；按 `cacheScope` 分 **public/private 双分区**（private 连接关闭时 `ClearPrivate` 丢弃，public 保留），读取 `GetAny` 双分区查（public 优先）；`resources/updated` 只失效对应 uri 键；`ExtractCacheHint`/`CacheIfHinted` 兼容两种形态（顶层 `ttlMs`/`cacheScope` 优先，回退嵌套 `cacheHint`）；`ReadResource` 支持 `cache_mode`（`use`/`bypass`/`refresh`）与 `max_age_ms`
- MRTR：`SendRequestWithMrtr` 循环 `input_required`，`max_rounds`（默认 10）超限抛 InternalError、`max_total_timeout` 超限抛 RequestTimeout；`input_requests` 三类型（elicit/confirm → elicitation、sampling、roots）分派对应 handler，无请求项时 state-only 退避（50ms ×2、封顶 250ms，见 [/concepts/mrtr.md](../concepts/mrtr.md)）
- 自动翻页上限 `kMaxAutoPages = 64` 页；任务轮询 500ms 间隔 / 300s 超时（`PollTaskToCompletion` 默认参）
- 超时：任务类请求 600s、Ping 10s（Ping 已标记 deprecated）；`SubscribeAsync` 发送后等待 `subscriptions/acknowledged` 首帧（**5s**，`kSubscriptionAckTimeout`），超时抛 `McpError(InternalError)`；请求携带 `_meta` `subscriptionId`（调用方提供或自动生成 `client-sub-<时钟>-<计数>`）
- `ClientOptions` 默认：`client_info {"mcp-cpp-client","0.3.0"}`、`initialization_timeout 60s`、`discover_probe_timeout 5s`

## 相关页面

- [/modules/client.md](../modules/client.md) — 所属库
- [/concepts/version-negotiation.md](../concepts/version-negotiation.md)
- [/concepts/mrtr.md](../concepts/mrtr.md) — MRTR 循环
- [/classes/mcp-session-handler.md](mcp-session-handler.md) — 底层引擎
