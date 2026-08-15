---
type: Module
title: mcp-protocol 协议库
description: JSON-RPC 引擎（McpSessionHandler）与双时代线协议编解码（WireCodec）。
tags: [protocol, jsonrpc, codec, 双时代]
timestamp: 2026-08-15T03:15:00+08:00
resource: src/protocol/McpSessionHandler.cpp
---

# mcp-protocol 协议库

依赖 `mcp-transport`。显式关闭 Unity 构建——McpSessionHandler.cpp 与 WireCodec.cpp 共享匿名命名空间辅助函数，Unity 合并导致重复符号。

## 组成

| 组件 | 职责 | 页 |
|------|------|----|
| McpSessionHandler | JSON-RPC 引擎：分发、请求/响应关联、超时、取消、过滤器 | [/classes/mcp-session-handler.md](../classes/mcp-session-handler.md) |
| WireCodec | 按时代划分的线协议词汇表：方法判定、校验、meta 处理 | [/classes/wire-codec.md](../classes/wire-codec.md) |
| MessageChannel | 有界异步队列（替代 asio channel） | [/classes/message-channel.md](../classes/message-channel.md) |
| MessageFilter / FilterPipeline | 入站/出站过滤器管线 | [/concepts/meta-and-filters.md](../concepts/meta-and-filters.md) |
| IncomingRequestMeta | 请求侧 meta 解析结果 | [/concepts/meta-and-filters.md](../concepts/meta-and-filters.md) |

## 通知处理器

WireCodec 编解码器集合共 **17 种**通知：公共 7 + 2025 独有 9（initialized、roots/list_changed、elicitation/complete、tasks/status、tasks/working、tasks/completed、tasks/failed、tasks/cancelled、tasks/input_required）+ 2026 独有 1（subscriptions/acknowledged）。[Methods.hpp](../../include/mcp/Methods.hpp) 的 `notifications` 命名空间保留 17 个常量，与编解码器集合一致（tasks 系列 5 个独有通知 2026-08-15 加回，服务端 `SendTaskStatus`/任务完成通知使用）。服务端在 `McpServer::WireHandlers()` 接线；客户端注册 5 个处理器（`tools/list_changed`、`resources/list_changed`、`prompts/list_changed` 于 [McpClient.cpp:481-486](../../src/client/McpClient.cpp)，`resources/updated` 于 [McpClient.cpp:487-496](../../src/client/McpClient.cpp)，`subscriptions/acknowledged` 于 [McpClient.cpp:497-516](../../src/client/McpClient.cpp)——匹配 `SubscribeAsync` 的待确认订阅并转发用户处理器，`notifications/message` 于 [McpClient.cpp:607](../../src/client/McpClient.cpp)，另有公共 `SetNotificationHandler` 转发 [McpClient.cpp:588](../../src/client/McpClient.cpp)，对 `subscriptions/acknowledged` 特判存储不覆盖内部处理器）。`notifications/cancelled` 在 `OnNotification` 中硬编码处理，先于处理器表查找。

## 关键语义

- `McpSessionHandler::OnRequest` 对 handler 抛出的 `McpError` 直接回 `e.Code()`；其他异常一律 `InternalError`（"handler error: ..."）
- 超时：默认 60s（`kDefaultRequestTimeout`），超时检查线程每 100ms 轮询；progress 通知延长截止时间 30s（仅当剩余时间 < 30s 时）
- 响应回发：单一 `response_worker_` 线程 + 有界 `response_queue_`（`deque<std::function>`）取代每请求一线程；任务内先 `wait_for(0)` 快检（同步 handler 零延迟）、未就绪 10ms 兜底轮询（`kResponsePollInterval`），`closed_` 时中止（保证 `Close()` 不阻塞）
- `SendRequest` 注册 pending 后复查 `closed_`，已关闭则以 `ConnectionClosed` 错误满足 promise（[McpSessionHandler.cpp:494](../../src/protocol/McpSessionHandler.cpp)）
- 2026 时代 `SendRequest` 顶层 stamp `_meta`；`SendNotification` 的 meta 只带 `negotiated_version_`

## 相关页面

- [/classes/mcp-session-handler.md](../classes/mcp-session-handler.md)
- [/classes/wire-codec.md](../classes/wire-codec.md)
- [/concepts/version-negotiation.md](../concepts/version-negotiation.md) — codec 时代切换
- [/modules/server.md](server.md) 与 [/modules/client.md](client.md) — 上下层消费方
