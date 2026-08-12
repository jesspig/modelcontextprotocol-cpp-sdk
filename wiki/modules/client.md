---
type: Module
title: mcp-client 客户端库
description: McpClient 门面：连接模式协商、请求/响应、OAuth 与令牌缓存。
tags: [client, oauth, 缓存, 协商]
timestamp: 2026-08-13T03:25:00+08:00
resource: src/client/McpClient.cpp
---

# mcp-client 客户端库

依赖 `mcp-protocol`。开启 Unity 但设 `UNITY_BUILD_UNIQUE_ID ON`（OAuth 匿名符号需要）。

## 组成

| 组件 | 职责 | 页 |
|------|------|----|
| McpClient | 客户端门面：创建、协商、请求、MRTR、自动翻页 | [/classes/mcp-client.md](../classes/mcp-client.md) |
| VersionNegotiation | Auto/Legacy/Pin 三种协商策略 | [/concepts/version-negotiation.md](../concepts/version-negotiation.md) |
| OAuthClientProvider | OAuth 2.0 授权码流 + PKCE + RFC 9207 iss 校验 | [/concepts/oauth.md](../concepts/oauth.md) |
| FileTokenCache / ITokenCache | 令牌持久化（Windows DPAPI） | [/classes/file-token-cache.md](../classes/file-token-cache.md) |

## 客户端行为要点

- **创建即阻塞**：`McpClient::Create` 构造后立即同步 `NegotiateProtocol()`，返回前协商完成
- **不注册通知处理器**：客户端收到通知静默丢弃；progress 处理（含超时延长）须自行 `SetNotificationHandler`
- `WireClientHandlers()` 仅注册 elicit 请求处理器 + 三个 listChanged 通知处理器（清空响应缓存）
- 懒注册：`SetSamplingHandler`/`SetRootsHandler` 未设置时收到请求抛 `MethodNotFound`；`SetLoggingHandler` 未设置时静默丢弃
- 自动翻页：无 cursor 的列表请求自动翻页，上限 `kMaxAutoPages = 64` 页
- 任务轮询：`resultType=="task"` 结果经 `PollTaskToCompletion`（500ms 间隔 / 300s 超时）
- 超时：任务类请求 `kTaskRequestTimeout = 600s`、Ping `kPingTimeout = 10s`

## 相关页面

- [/classes/mcp-client.md](../classes/mcp-client.md)
- [/concepts/version-negotiation.md](../concepts/version-negotiation.md)
- [/concepts/oauth.md](../concepts/oauth.md)
- [/classes/file-token-cache.md](../classes/file-token-cache.md)
