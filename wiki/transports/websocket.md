---
type: Transport
title: WebSocket 客户端传输
description: 基于自研 WebSocketClient（RFC 6455）的客户端传输，连接状态由 IO 线程回调驱动。
tags: [transport, websocket]
timestamp: 2026-11-18T20:55:40+08:00
resource: src/transport/WebSocketClientTransport.cpp
---

# WebSocket 客户端传输

`WebSocketClientTransport`（客户端工厂，[WebSocketClientTransport.cpp](../../src/transport/WebSocketClientTransport.cpp)）。构造 `(url, name = "websocket")`。定位为**非标超集**（spec 未要求 WebSocket 传输，不计入 Tier1 对等口径，见 [/concepts/tier1-parity.md](../concepts/tier1-parity.md)）。

## 会话实现

- 成员持有自研 `detail::net::WebSocketClient ws_`（[WebSocketClient.hpp](../../src/transport/detail/net/WebSocketClient.hpp)，RFC 6455，独立 IO 线程）
- 协议细节（[WebSocketClient.cpp](../../src/transport/detail/net/WebSocketClient.cpp)）：客户端发送掩码帧并校验握手 `101`/`Sec-WebSocket-Accept`；服务端掩码帧、非零 RSV、非法控制帧或超 8MB 帧直接判 `ProtocolViolation`，`ping` 自动回 `pong`（读帧空闲上限 24h，写入 30s 超时）
- `Start()`：回调捕获 **`std::weak_ptr`**（`lock()` 判空，防悬挂且不持有会话）；`SetCallbacks(on_message, on_close, on_error)`：消息回调 → 反序列化 + 入通道，解析失败 `NotifyError`；关闭回调 → `running_ = false; SetDisconnected()`；错误回调 → `NotifyError`；随后 `running_ = true; SetConnected(); ws_.Open(url, 30s, true)`（IO 线程执行 连接+TLS+HTTP 握手，失败经 on_error/on_close 回退）
- **注意：自研客户端无 onopen——`SetConnected` 由 `Start()` 直接调用**，握手失败由 on_error/on_close 路径回退
- `Close()`：`running_ = false` → `ws_.Close()`（发 Close 帧并关底层 socket，`JoinThreadSafely` join IO 线程、self 时 detach；期间会话回调仍有效，`NotifyClose` 会触发一次 on_close → 会话侧 `running_ = false; SetDisconnected()`）→ **显式置空回调（`SetCallbacks(nullptr, nullptr, nullptr)`）清理悬挂引用** → 关通道 → SetDisconnected
- `SendMessageAsync`：`!running_` 直接丢弃；序列化后 `ws_.Send(json_str)`

## 相关页面

- [/modules/transport.md](../modules/transport.md) — 所属库
- [/classes/message-channel.md](../classes/message-channel.md) — 消息载体
- [/concepts/concurrency.md](../concepts/concurrency.md) — 回调线程与生命周期
