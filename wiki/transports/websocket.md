---
type: Transport
title: WebSocket 客户端传输
description: 基于自研 WebSocketClient（RFC 6455）的客户端传输，连接状态由 IO 线程回调驱动。
tags: [transport, websocket]
timestamp: 2026-11-25T01:00:00+08:00
resource: src/transport/WebSocketClientTransport.cpp
---

# WebSocket 客户端传输

`WebSocketClientTransport`（客户端工厂，[WebSocketClientTransport.cpp](../../src/transport/WebSocketClientTransport.cpp)）。构造 `(url, name = "websocket")`。定位为**非标超集**（spec 未要求 WebSocket 传输，不计入 Tier1 对等口径，见 [/concepts/tier1-parity.md](../concepts/tier1-parity.md)）。

## 会话实现

- 成员持有自研 `detail::net::WebSocketClient ws_`（[WebSocketClient.hpp](../../src/transport/detail/net/WebSocketClient.hpp)，RFC 6455，独立 IO 线程）
- 协议细节（[WebSocketClient.cpp](../../src/transport/detail/net/WebSocketClient.cpp)）：客户端发送掩码帧并校验握手 `101`/`Sec-WebSocket-Accept`；服务端掩码帧、非零 RSV、非法控制帧或超 8MB 帧直接判 `ProtocolViolation`，`ping` 自动回 `pong`（读帧空闲上限 24h，写入 30s 超时）
- 握手状态机：`WebSocketClient::HandshakeState` 为 `Connecting → Open → Closing → Closed`，唯一终止态 `Closed` 只通知一次。`Open()` 置 `Connecting`，只有状态行、`Upgrade: websocket`、`Connection: Upgrade` 与 `Sec-WebSocket-Accept` 全部验证成功才置 `Open` 并调用 `on_open`；失败、超时、对端关闭与显式 `Close()` 都收敛到 `Closed`。总截止时间（默认 30s）覆盖 DNS/TCP/TLS/写请求/读响应/重定向，`Close()` 关 socket 中断阻塞读写并经 `JoinThreadSafely` join
- 101 前有界队列：Connecting 时 `Send()` 入队，条数上限 `kMaxWebSocketHandshakeQueueMessages`（64）且序列化字节上限 `kMaxWebSocketHandshakeQueueBytes`（1 MiB）；满时经 `on_error` 明确拒绝；Open 后按入队顺序冲刷；Closing/Closed 拒新消息；握手失败/冲刷失败逐条经 `on_error` 报错，不静默丢弃
- 安全重定向：仅同主机同端口或 `ws:80 → wss:443` 升级，最多 `kMaxWebSocketRedirects`（5）跳；拒降级、跨主机、其他跨端口、带 userinfo 与畸形 Location；每次跳转关旧 socket、按新 scheme 重建并生成新 `Sec-WebSocket-Key`，TLS 校验不放宽（[Url.hpp](../../include/mcp/transport/detail/Url.hpp) 的 `IsSafeWebSocketRedirect`）
- `Start()`：回调捕获 **`std::weak_ptr`**（`lock()` 判空，防悬挂且不持有会话）；`SetCallbacks(on_message, on_close, on_error)`：消息回调 → 反序列化 + 入通道，解析失败 `NotifyError`；关闭回调 → `running_ = false; CompleteShutdown()`；错误回调 → `NotifyError`；随后 `running_ = true; ws_.SetOpenCallback(...)` 在握手成功时 `SetConnected()` 完成 `Connecting → Open`，再 `ws_.Open(url, 30s, true)`（IO 线程执行连接+TLS+握手+重定向）
- `Close()`：`BeginShutdown()` 进入 `Closing` 停接收 → 关 `MessageChannel` 唤醒等待者 → `ws_.Close()`（发 Close 帧、单一所有者关 socket 并 `JoinThreadSafely` join IO 线程、self 时 detach）→ `CompleteShutdown()` 一次性触发 `on_close_`。IO 线程的 `on_close` 回调在 `Closing` 期间被 generation 守卫忽略，不再提前通知
- 回调捕获创建时 generation：`message/error/close` 在新 generation 建立后一律丢弃，不写旧会话通道；工厂 `Connect()` 先关闭并等待上一 generation 完成，再启动新会话
- `SendMessageAsync`：`!running_ || !IsGenerationActive()` 直接丢弃；序列化后 `ws_.Send(json_str)`，101 前由此进入有界队列，握手失败时逐条报错
- 单一所有者：`WebSocketClient::Close()` 以 `closed_.exchange(true)` 选出唯一关闭方；`TlsSocket` 在 `io_mutex_` 内换出 `SSL*`，`TcpSocket::Close()` 自身幂等，显式关闭、对端关闭、析构与握手失败并发下 SSL 与原生句柄各释放一次

## 相关页面

- [/modules/transport.md](../modules/transport.md) — 所属库
- [/classes/message-channel.md](../classes/message-channel.md) — 消息载体
- [/concepts/concurrency.md](../concepts/concurrency.md) — 回调线程与生命周期
