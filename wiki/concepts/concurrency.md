---
type: Concept
title: 并发与生命周期
description: 线程模型（消息循环/超时检查/IO 线程）、Close self-join 陷阱、异步 handler 收尾。
tags: [并发, 线程, 生命周期, 死锁]
timestamp: 2026-08-13T03:25:00+08:00
resource: include/mcp/detail/ThreadUtils.hpp
---

# 并发与生命周期

无 asio——裸线程 + 标准库原语（mutex、condition_variable）或 libhv 事件循环。

## 线程模型

| 组件 | 线程 |
|------|------|
| McpSessionHandler | 消息循环线程 + 超时检查线程（100ms 轮询） |
| Stdio 传输 | 读循环线程 |
| SSE 客户端 | SSE 读线程 + 发送线程 |
| Streamable HTTP 客户端 | 发送线程 + SSE 读线程（Win32） |
| HttpServer | libhv 事件循环线程（worker 8） |

## 易翻车点

- **IO 线程回调内调用 `Close()` 会 self-join**：stdio/SSE/HTTP 传输的 IO 线程直接执行用户回调（`on_transport_close`/`on_transport_error`），回调里调 `Close()` 会 join 自身线程抛异常。所有 `Close()` 必须用 `detail::JoinThreadSafely`（[ThreadUtils.hpp](../../include/mcp/detail/ThreadUtils.hpp)）：self 时 detach，否则 join
- `McpSessionHandler::Close()`：置标志 → 关通道唤醒循环 → join → pending 全部以 `ConnectionClosed` 回调（**锁外执行**，防回调死锁）
- `SendResponseAsync`：`std::async` + 50ms 轮询 promise，`closed_` 时放弃等待（保证 Close 不阻塞）
- 超时回调同样在锁外执行
- `MessageChannel`：回调在锁外调用；`Send` 满则阻塞（`TrySend` 不阻塞）

## 相关页面

- [/classes/mcp-session-handler.md](../classes/mcp-session-handler.md) — 引擎线程
- [/classes/message-channel.md](../classes/message-channel.md) — 队列语义
- [/transports/stdio.md](../transports/stdio.md) — 读循环判停
- [/AGENTS.md](../../AGENTS.md) — 并发与生命周期陷阱清单
