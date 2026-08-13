---
type: Concept
title: 并发与生命周期
description: 线程模型（消息循环/超时检查/响应回发）、Close self-join 陷阱、异步 handler 收尾。
tags: [并发, 线程, 生命周期, 死锁]
timestamp: 2026-08-13T12:36:14+08:00
resource: include/mcp/detail/ThreadUtils.hpp
---

# 并发与生命周期

无 asio——裸线程 + 标准库原语（mutex、condition_variable）或 libhv 事件循环。

## 线程模型

| 组件 | 线程 |
|------|------|
| McpSessionHandler | 消息循环线程 + 超时检查线程（100ms 轮询）+ **响应回发线程**（response_worker_） |
| Stdio 传输 | 读循环线程 |
| SSE 客户端 | SSE 读线程 + 发送线程 |
| Streamable HTTP 客户端 | 发送线程 + SSE 读线程（Win32） |
| HttpServer | libhv 事件循环线程（worker 8） |

## 响应回发机制（response_worker_）

每请求**不再创建 OS 线程**（`SendResponseAsync`/`ReapCompletedResponses` 已删除）：handler 的 promise future 由单一 `response_worker_` 线程按队列串行消费（[McpSessionHandler.hpp](../../include/mcp/protocol/McpSessionHandler.hpp:190)）。

- `EnqueueResponse`：`response_queue_mutex_` 锁内检查 `closed_`（已关闭则**跳过投递直接丢弃**）
- 队列任务：50ms 轮询 future；等待中观察到 `closed_` 立即中止——**Close 永不阻塞在未被满足的 promise 上**
- `Close()`：置 `closed_` → 关通道唤醒消息循环 → `JoinThreadSafely` 依次 join 三线程 → `response_cv_.notify_all()` → join response_worker_ → pending 全部以 `ConnectionClosed` 回调（**锁外执行**，防回调死锁）

## 易翻车点

- **IO 线程回调内调用 `Close()` 会 self-join**：stdio/SSE/HTTP 传输的 IO 线程直接执行用户回调（`on_transport_close`/`on_transport_error`），回调里调 `Close()` 会 join 自身线程抛异常。所有 `Close()` 必须用 `detail::JoinThreadSafely`（[ThreadUtils.hpp](../../include/mcp/detail/ThreadUtils.hpp)）：self 时 detach，否则 join
- `Start()` 在 `closed_` 之后调用抛 `std::logic_error`（[McpSessionHandler.cpp](../../src/protocol/McpSessionHandler.cpp:74)）
- `SendRequest`：注册 pending 后复查 `closed_`，已关闭则立即以 `ConnectionClosed` 满足 promise（锁内注册防竞态，[McpSessionHandler.cpp](../../src/protocol/McpSessionHandler.cpp:494)）
- `negotiated_version_` 为 `shared_ptr<const std::string>`：`SetNegotiatedProtocolVersion` 在 `codec_mutex_` 下与 codec 原子交换，`NegotiatedProtocolVersion()` 锁下拷贝返回 `std::string`
- 入站验证只构造轻量视图（method/_meta/initialize params），不完整序列化请求
- `HttpServer::Stop()`：`running_` 原子化（`exchange(false)`）；libhv `stop()` 会 join 所有事件循环线程，故在独立 stopper 线程中执行并 `JoinThreadSafely(stopper)`（self-join 防护，[HttpServer.cpp](../../src/http/HttpServer.cpp:207)）
- 超时回调同样在锁外执行
- `MessageChannel`：回调在锁外调用；`Send` 满则阻塞（`TrySend` 不阻塞）
- 写侧串行化：`FileTaskStore` 双锁（详见 [/concepts/storage.md](storage.md)）、`OAuthClientProvider::GetAccessToken` 的 `refresh_mutex_`（详见 [/concepts/oauth.md](oauth.md)）

## 相关页面

- [/classes/mcp-session-handler.md](../classes/mcp-session-handler.md) — 引擎线程
- [/classes/message-channel.md](../classes/message-channel.md) — 队列语义
- [/transports/stdio.md](../transports/stdio.md) — 读循环判停
- [/AGENTS.md](../../AGENTS.md) — 并发与生命周期陷阱清单
