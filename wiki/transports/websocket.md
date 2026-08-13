---
type: Transport
title: WebSocket 客户端传输
description: 基于自研 WebSocketClient（RFC 6455）的客户端传输，连接状态由 IO 线程回调驱动。
tags: [transport, websocket]
timestamp: 2026-08-13T16:30:00+08:00
resource: src/transport/WebSocketClientTransport.cpp
---

# WebSocket 客户端传输

`WebSocketClientTransport`（客户端工厂，[WebSocketClientTransport.cpp](../../src/transport/WebSocketClientTransport.cpp)）。构造 `(url, name = "websocket")`。

## 会话实现

- 成员持有自研 `detail::net::WebSocketClient ws_`（[WebSocketClient.hpp](../../src/transport/detail/net/WebSocketClient.hpp)，RFC 6455，独立 IO 线程）
- `Start()`：回调捕获 **`std::weak_ptr`**（`lock()` 判空，防悬挂且不持有会话）；`SetCallbacks(on_message, on_close, on_error)`：消息回调 → 反序列化 + 入通道，解析失败 `NotifyError`；关闭回调 → `running_ = false; SetDisconnected()`；错误回调 → `NotifyError`；随后 `running_ = true; SetConnected(); ws_.Open(url, 30s, true)`（IO 线程执行 连接+TLS+HTTP 握手，失败经 on_error/on_close 回退）
- **注意：自研客户端无 onopen——`SetConnected` 由 `Start()` 直接调用**，握手失败由 on_error/on_close 路径回退
- `Close()`：`running_ = false` → **显式置空回调（`SetCallbacks(nullptr, nullptr, nullptr)`）打破循环引用**（原实现每连接永久泄漏对象与线程）→ `ws_.Close()`（中断并 join IO 线程，触发 on_close）→ 关通道 → SetDisconnected
- `SendMessageAsync`：`!running_` 直接丢弃；序列化后 `ws_.Send(json_str)`

## 相关页面

- [/modules/transport.md](../modules/transport.md) — 所属库
- [/classes/message-channel.md](../classes/message-channel.md) — 消息载体
- [/concepts/concurrency.md](../concepts/concurrency.md) — 回调线程与生命周期
