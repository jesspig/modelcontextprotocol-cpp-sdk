---
type: Module
title: mcp-protocol 协议库
description: JSON-RPC 引擎（McpSessionHandler，含 idle/总量双超时）与双时代线协议编解码（WireCodec）。
tags: [protocol, jsonrpc, codec, 双时代, meta, 超时]
timestamp: 2026-09-12T11:26:01+08:00
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

WireCodec 编解码器集合共 **17 种**通知：公共 7 + 2025 独有 9（initialized、roots/list_changed、elicitation/complete、tasks/status、tasks/working、tasks/completed、tasks/failed、tasks/cancelled、tasks/input_required）+ 2026 独有 1（subscriptions/acknowledged）。[Methods.hpp](../../include/mcp/Methods.hpp) 的 `notifications` 命名空间保留 17 个常量，与编解码器集合一致（tasks 系列 6 个独有通知 2026-08-15 加回，服务端 `SendTaskStatus`/任务完成通知使用）。服务端在 `McpServer::WireHandlers()` 接线；客户端注册 6 个处理器（`tools/list_changed`、`resources/list_changed`、`prompts/list_changed` 于 [McpClient.cpp:570-575](../../src/client/McpClient.cpp)，`resources/updated` 于 [McpClient.cpp:576](../../src/client/McpClient.cpp)，`notifications/progress` 于 [McpClient.cpp:589](../../src/client/McpClient.cpp)——重置超时 + 分发 `on_progress` 回调，`subscriptions/acknowledged` 于 [McpClient.cpp:617](../../src/client/McpClient.cpp)——匹配 `SubscribeAsync` 的待确认订阅并转发用户处理器，`notifications/message` 于 [McpClient.cpp:731](../../src/client/McpClient.cpp)，另有公共 `SetNotificationHandler` 转发 [McpClient.cpp:712](../../src/client/McpClient.cpp)，对 `subscriptions/acknowledged` 特判存储不覆盖内部处理器）。`notifications/cancelled` 在 `OnNotification` 中硬编码处理，先于处理器表查找。

## 关键语义

- `McpSessionHandler::OnRequest` 对 handler 抛出的 `McpError` 直接回 `e.Code()`；其他异常一律 `InternalError`（"handler error: ..."）
- 超时：默认 60s（`kDefaultRequestTimeout`），超时检查线程每 100ms 轮询；progress 通知延长截止时间 30s（仅当剩余时间 < 30s 时）
- 总量超时封顶：`SetMaxTotalTimeout`（会话运行中可调，0 禁用）——每请求在 `SendRequest` 时记录**绝对截止**（`absolute_deadlines_`，pending 锁保护），progress 续命只顺延 idle deadline、不可越过总量
- 响应回发：单一 `response_worker_` 线程 + 有界 `response_queue_`（`deque<std::function>`）取代每请求一线程；任务内先 `wait_for(0)` 快检（同步 handler 零延迟）、未就绪 10ms 兜底轮询（`kResponsePollInterval`），`closed_` 时中止（保证 `Close()` 不阻塞）
- `SendRequest` 注册 pending 后复查 `closed_`，已关闭则以 `ConnectionClosed` 错误满足 promise（[McpSessionHandler.cpp:535](../../src/protocol/McpSessionHandler.cpp)）
- 双 era meta 落点：modern era `req.meta` 写 `_meta` 信封、legacy era progressToken 写 `params._meta.progressToken`；`SendNotification` 的 meta 只带 `negotiated_version_`。序列化层（[JsonRpc.cpp](../../src/core/JsonRpc.cpp)）统一把 `meta` 合并进 `params._meta`，反序列化后从 params 移除 `_meta` 键——线上形态一律 `params._meta`，内存结构 `req.meta`/`notif.meta` 语义不变
- `CompleteRequestParams` 线格式修正为官方 `{ref, argument: {name, value}}`（原扁平 `argumentName`/`argumentValue` 为真 bug，官方 conformance 捕获）；反序列化优先官方形状，容缺回退旧扁平形状
- 空结果不再写 `resultType`：`SerializeEmptyResult` 仅剩可选 meta（官方 conformance 期望空对象）；`CallToolResult` 仅在 `input_required` 分支写 `resultType: "input_required"` + `inputRequests`/`requestState`
- `requestState` 校验失败的错误响应带 `data.reason="invalid_request_state"`（handler 前拒绝）

## 相关页面

- [/classes/mcp-session-handler.md](../classes/mcp-session-handler.md)
- [/classes/wire-codec.md](../classes/wire-codec.md)
- [/concepts/version-negotiation.md](../concepts/version-negotiation.md) — codec 时代切换
- [/modules/server.md](server.md) 与 [/modules/client.md](client.md) — 上下层消费方
