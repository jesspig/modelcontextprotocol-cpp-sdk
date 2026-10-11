---
type: Transport
title: SSE 客户端传输
description: 服务端推送事件流（GET）+ POST 消息通道，支持 Last-Event-ID 断线回放与退避重连。
tags: [transport, sse, 重连]
timestamp: 2026-10-11T16:35:00+08:00
resource: src/transport/SseClientTransport.cpp
---

# SSE 客户端传输

`SseClientTransport`（客户端工厂，[SseClientTransport.cpp](../../src/transport/SseClientTransport.cpp)）。构造 `(server_url, name = {})`，name 空回落 `"sse"`。

## 会话实现

- 持有**两个独立自研 HttpClient**（[detail/net/HttpClient](../../src/transport/detail/net/HttpClient.hpp)，基于 TcpSocket/TlsSocket 的阻塞 HTTP/1.1）：`http_client_`（GET SSE 流）+ `post_client_`（POST 消息）
- `Start()`：SSE 读线程 + 发送线程双线程，启动即 `SetConnected()`
- `Close()`：notify 发送线程 → 关两个 client（**靠关客户端解除阻塞**）→ **无条件** `JoinThreadSafely(send_thread_ / sse_thread_)`（读线程可能已自行退出，Close 兜底 join）→ 关通道 → SetDisconnected

## SSE 协议细节

- GET 带 `Accept: text/event-stream`；有 `last_event_id_` 时带 `Last-Event-ID` 头（断线回放）；GET 请求**空闲超时 5 分钟**（`kSseClientIdleTimeout`，经 `req.timeout`）——无数据超时走退避重连
- 解析支持 `event:` / `data:` / `id:` / `retry:` 行；多行 data 用 `\n` 拼接；**`last_event_id_` 仅由 `id:` 行更新**（无 id 的事件不再清空，任何时刻保留最近一次 `id:` 值，对齐规范"只读 id 行"语义）；`retry:`（全数字毫秒值）更新 `retry_ms_`，仅用于重连决策
- **纯注释帧（如 keepalive `: ping`）被忽略**：行不以 `event:`/`data:`/`id:`/`retry:` 开头即跳过；整帧无任何字段（`event_type` 默认 `"message"` 且 data/id 为空、无 retry）时直接返回，**不改变 `last_event_id_`**（服务端 keepalive 注释帧不会破坏断线回放）
- `event: endpoint` → 解析 POST 端点（相对路径拼在 `scheme://host[:port]` 后，默认端口省略）；`event: message` → 超限 `NotifyError` 并丢弃该事件，否则反序列化 + 入通道
- **缓冲超限防护**（`body_cb` 中）：`sse_buffer_` > `kMaxMessageSize`（8MB）→ 清空 + `NotifyError` + `running_.store(false)` + `http_client_->Close()`（关闭即中止阻塞中的响应读取）
- 流结束且非显式关闭时**退避重连**：最多 2 次尝试（`kMaxReconnectAttempts`），退避 1s 起始 ×1.5 增长、封顶 30s（`kBackoffBase`/`kBackoffCap`）；服务端 `retry:` 与默认退避统一经 `sse_client_impl::ReconnectDelayForAttempt` 钳制到 **450–1000ms** 窗口（`kReconnectWaitFloor`/`kReconnectWaitCeiling`，对齐 SEP-1699 的 500ms −50ms/+200ms 口径；超大 `retry:` 先按整数比较再构造时长，避免 `milliseconds` 溢出回绕成立即重连；确定性无抖动，下探抖动会击穿 450ms 下限）；等待期间每 100ms 检查 `running_`，`Close()` 即时中断。同一钳制函数语义被 Streamable HTTP 客户端的 GET 监听流复用（见 [/transports/streamable-http.md](streamable-http.md)，其退避基数 ×1.5/封顶 30s/上限 5 次独立）
- 流结束时先 `FlushSseBuffer()` 再算重连等待：尾部未以空行结尾的残留字节中，完整行里含 `id:`/`retry:` 即按一帧补派发（新值覆盖旧值，`retry-only` 尾块与关流竞态不再沿用旧值），无换行符的截断行直接丢弃；无论是否补派发都清空缓冲，防止残留拼接到下一条连接的首帧。补派发复用 `DispatchSseEvent` 语义，无 `id:`/`retry:` 的纯数据残尾仍按关流丢弃，不会误触发解析错误
- 重试耗尽自行退出时：**不置 `running_`**，先 `channel_->Close()` 再 `notify_one()` 唤醒发送线程，最后 `SetDisconnected()`
- 发送线程：条件变量取队列（wait 谓词含 `channel_ && !channel_->IsOpen()` 退出条件，通道关闭时也能退出），POST 到 endpoint，`Content-Type: application/json`

## 相关页面

- [/modules/transport.md](../modules/transport.md) — 所属库
- [/transports/streamable-http.md](streamable-http.md) — HTTP 传输模式中的 SSE 回退
- [/classes/http-server.md](../classes/http-server.md) — 服务端 SSE 广播
- [/concepts/concurrency.md](../concepts/concurrency.md) — 线程模型
