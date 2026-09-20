---
type: Concept
title: 并发与生命周期
description: 线程模型（消息循环/超时检查/响应回发）、Close self-join 陷阱、异步 handler 收尾。
tags: [并发, 线程, 生命周期, 死锁]
timestamp: 2026-09-20T19:34:31+08:00
resource: include/mcp/detail/ThreadUtils.hpp
---

# 并发与生命周期

无 asio——裸线程 + 标准库原语（mutex、condition_variable）。

## 线程模型

| 组件 | 线程 |
|------|------|
| McpSessionHandler | 消息循环线程 + 超时检查线程（100ms 轮询）+ **响应回发线程**（response_worker_） |
| Stdio 传输 | 读循环线程 |
| SSE 客户端 | SSE 读线程 + 发送线程 |
| Streamable HTTP 客户端 | 发送线程（请求 POST 队列 + 同步读 POST 的 SSE 响应）+ 监听线程（`enable_listen_stream` 默认开启，`notifications/initialized` 后拉起 GET SSE，最多 5 次重连）+ 通知/响应走短命 detached POST 线程 |
| HttpServer | accept 线程 + 每连接线程（上限 256）+ SSE keepalive 线程 |

## 响应回发机制（response_worker_）

每请求**不再创建 OS 线程**（`SendResponseAsync`/`ReapCompletedResponses` 已删除）：handler 的 promise future 由单一 `response_worker_` 线程按队列串行消费（[McpSessionHandler.hpp:203](../../include/mcp/protocol/McpSessionHandler.hpp)）。

- `EnqueueResponse`：`response_queue_mutex_` 锁内检查 `closed_`（已关闭则**跳过投递直接丢弃**）
- 队列任务：先 `wait_for(0)` 快检（同步 handler 零延迟），未就绪则 10ms 间隔轮询 future；等待中观察到 `closed_` 立即中止——**Close 永不阻塞在未被满足的 promise 上**
- `Close()`：置 `closed_` → 关通道唤醒消息循环 → `JoinThreadSafely` 依次 join 消息循环线程与超时线程 → `response_cv_.notify_all()` → join response_worker_ → pending 全部以 `ConnectionClosed` 回调（**锁外执行**，防回调死锁）→ `transport_->Close()`

## 易翻车点

- **IO 线程回调内调用 `Close()` 会 self-join**：stdio/SSE/HTTP 传输的 IO 线程直接执行用户回调（`on_transport_close`/`on_transport_error`），回调里调 `Close()` 会 join 自身线程抛异常。所有 `Close()` 必须用 `detail::JoinThreadSafely`（[ThreadUtils.hpp](../../include/mcp/detail/ThreadUtils.hpp)）：self 时 detach，否则 join
- **detach 后线程仍会访问 `this`**：`JoinThreadSafely` detach 自身线程后该线程继续执行直到退出——`StdioServerTransport::ReadLoop` 入口 `shared_from_this()` 自持有，保证 IO 线程回调内 Close（detach）后成员访问不 UAF（[StdioServerTransport.cpp:55](../../src/transport/StdioServerTransport.cpp)）；同理，Streamable HTTP 客户端的立即 POST 线程在 detach 前捕获 `shared_from_this()`，Close 后线程存活期间对象仍有效（[StreamableHttpClientTransport.cpp:998](../../src/http/StreamableHttpClientTransport.cpp)）
- `Start()` 在 `closed_` 之后调用抛 `std::logic_error`（[McpSessionHandler.cpp:203](../../src/protocol/McpSessionHandler.cpp)）
- `Streamable HTTP` 的 detached immediate POST 线程只用于通知/响应：线程捕获 `shared_from_this()` 保活对象，HTTP 超时限制寿命；`Close()` 会收停 GET 监听和主发送线程，但不等待这些短命 POST 线程。它们的发送分流和异常语义见 [/transports/streamable-http.md](../transports/streamable-http.md)
- `SendRequest`：注册 pending 后复查 `closed_`，已关闭则立即以 `ConnectionClosed` 满足 promise（锁内注册防竞态，[McpSessionHandler.cpp:764](../../src/protocol/McpSessionHandler.cpp)）
- `negotiated_version_` 为 `shared_ptr<const std::string>`：`SetNegotiatedProtocolVersion` 在 `codec_mutex_`（`shared_mutex`，读并发写独占）下与 codec 原子交换，`NegotiatedProtocolVersion()` 读锁下拷贝 shared_ptr、锁外解引用返回 `std::string`
- 入站验证只构造轻量视图（method/_meta/initialize params），不完整序列化请求
- `HttpServer::Stop()`：`running_` 原子化（`exchange(false)`）；自研 `Impl::Stop()` 关 listen/连接 fd 解除阻塞后 join accept 与全部连接线程，故在独立 stopper 线程中执行并 `JoinThreadSafely(stopper)`（self-join 防护，[HttpServer.cpp:51](../../src/http/HttpServer.cpp)）
- 超时回调同样在锁外执行
- `MessageChannel`：回调在锁外调用；`Send` 满则阻塞（`TrySend` 不阻塞）
- 写侧串行化：`FileTaskStore` 双锁（详见 [/concepts/storage.md](storage.md)）、`OAuthClientProvider::GetAccessToken` 的 `refresh_mutex_`（详见 [/concepts/oauth.md](oauth.md)）

## 相关页面

- [/classes/mcp-session-handler.md](../classes/mcp-session-handler.md) — 引擎线程
- [/classes/message-channel.md](../classes/message-channel.md) — 队列语义
- [/transports/stdio.md](../transports/stdio.md) — 读循环判停
- [/AGENTS.md](../../AGENTS.md) — 并发与生命周期陷阱清单
