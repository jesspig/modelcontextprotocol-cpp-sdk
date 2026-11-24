---
type: Transport
title: SSE 客户端传输
description: 服务端推送事件流（GET）+ POST 消息通道，支持 Last-Event-ID 断线回放与退避重连。
tags: [transport, sse, 重连]
timestamp: 2026-11-25T01:44:25+08:00
resource: src/transport/SseClientTransport.cpp
---

# SSE 客户端传输

`SseClientTransport`（客户端工厂，[SseClientTransport.cpp](../../src/transport/SseClientTransport.cpp)）。构造 `(server_url, name = {})`，name 空回落 `"sse"`。

## 会话实现

- 持有**两个独立自研 HttpClient**（[detail/net/HttpClient](../../src/transport/detail/net/HttpClient.hpp)，基于 TcpSocket/TlsSocket 的阻塞 HTTP/1.1）：`http_client_`（GET SSE 流）+ `post_client_`（POST 消息）
- `Start()`：SSE 读线程 + 发送线程双线程，启动即 `SetConnected()`
- `Close()`：`running_.exchange(false)` 选唯一 owner → `BeginShutdown()` 进入 `Closing` → 先关 `MessageChannel` 唤醒等待者 → notify 发送线程 → 关两个 client（**靠关客户端解除阻塞**）→ `JoinThreadSafely(send_thread_ / sse_thread_)`（读线程可能已自行退出，Close 兜底 join）→ `CompleteShutdown()` 一次性通知
- generation：读循环 `DispatchSseEvent` 与 `SendMessageAsync` 均检查 `IsGenerationActive()`；旧 generation 的事件不投递新会话。工厂 `Connect()` 先关闭并等待上一 generation，再启动新会话；`SessionLifecycle` 见 [/concepts/concurrency.md](../concepts/concurrency.md)

## SSE 协议细节

- GET 带 `Accept: text/event-stream`；有 `last_event_id_` 时带 `Last-Event-ID` 头（断线回放）；GET 请求**空闲超时 5 分钟**（`kSseClientIdleTimeout`，经 `req.timeout`）——无数据超时走退避重连
- 解析支持 `event:` / `data:` / `id:` / `retry:` 行；多行 data 用 `\n` 拼接；**`last_event_id_` 仅由 `id:` 行更新**（无 id 的事件不再清空，任何时刻保留最近一次 `id:` 值，对齐规范"只读 id 行"语义）；`retry:`（全数字毫秒值）更新 `retry_ms_`，仅用于重连决策
- **纯注释帧（如 keepalive `: ping`）被忽略**：行不以 `event:`/`data:`/`id:`/`retry:` 开头即跳过；整帧无任何字段（`event_type` 默认 `"message"` 且 data/id 为空、无 retry）时直接返回，**不改变 `last_event_id_`**（服务端 keepalive 注释帧不会破坏断线回放）
- `event: endpoint` → 解析 POST 端点（相对路径拼在 `scheme://host[:port]` 后，默认端口省略）；`event: message` → 超限 `NotifyError` 并丢弃该事件，否则反序列化 + 入通道
- **增量分帧**：共享 [SseEventParser.hpp](../../include/mcp/detail/SseEventParser.hpp) 的 `SseIncrementalParser` 接受任意分块，CRLF/LF/单独 CR 与混合换行等价；末尾 `\r` 跨块保留到下一字节再判定，空行派发事件，注释行不计入事件但计入事件字节上限。`event:`/`data:`/`id:`/`retry:` 多行与单字段走同一规则（`retry:` 经 checked uint64 解析，溢出忽略）
- **缓冲超限防护**（`body_cb` 中）：`SseIncrementalParser` 上限 `SseEventLimit()`（`options_.max_sse_event_bytes`，默认 `detail::kMaxSseEventBytes` 1MB，显式置 `nullopt` 才无上限）超限 → `NotifyError` + `running_.store(false)` + `http_client_->Close()`（关闭即中止阻塞中的响应读取），并重置解析器状态
- 流结束且非显式关闭时**退避重连**：最多 2 次尝试（`kMaxReconnectAttempts`），退避 1s 起始 ×1.5 增长、封顶 30s（`kBackoffBase`/`kBackoffCap`）；服务端 `retry:` 与默认退避统一经 `sse_client_impl::ReconnectDelayForAttempt` 钳制到 **450–1000ms** 窗口（`kReconnectWaitFloor`/`kReconnectWaitCeiling`，对齐 SEP-1699 的 500ms −50ms/+200ms 口径；超大 `retry:` 先按整数比较再构造时长，避免 `milliseconds` 溢出回绕成立即重连；确定性无抖动，下探抖动会击穿 450ms 下限）；等待期间每 100ms 检查 `running_`，`Close()` 即时中断。同一钳制函数语义被 Streamable HTTP 客户端的 GET 监听流复用（见 [/transports/streamable-http.md](streamable-http.md)，其退避基数 ×1.5/封顶 30s/上限 5 次独立）
- 流结束时先 `FlushSseBuffer()` 再算重连等待：`SseIncrementalParser::Flush()` 补派发已收齐完整行但无空行的帧（含 `id:`/`retry:` 的新值覆盖旧值），无换行符的截断行丢弃；无论是否补派发都重置解析器，防止残留拼接到下一条连接的首帧。补派发复用 `DispatchSseEvent` 语义，无 `id:`/`retry:` 的纯数据残尾仍按关流丢弃，不会误触发解析错误
- 重试耗尽自行退出时：**不置 `running_`**，先 `channel_->Close()` 再 `notify_one()` 唤醒发送线程，最后 `SetDisconnected()`
- 发送线程：条件变量取队列（wait 谓词含 `channel_ && !channel_->IsOpen()` 退出条件，通道关闭时也能退出），POST 到 endpoint，`Content-Type: application/json`

## 相关页面

- [/modules/transport.md](../modules/transport.md) — 所属库
- [/transports/streamable-http.md](streamable-http.md) — HTTP 传输模式中的 SSE 回退
- [/classes/http-server.md](../classes/http-server.md) — 服务端 SSE 广播
- [/concepts/concurrency.md](../concepts/concurrency.md) — 线程模型
