---
type: Transport
title: WebSocket 客户端传输
description: 基于 libhv WebSocketClient 的客户端传输，连接状态由 onopen/onclose 回调驱动。
tags: [transport, websocket, libhv]
timestamp: 2026-08-13T12:36:14+08:00
resource: src/transport/WebSocketClientTransport.cpp
---

# WebSocket 客户端传输

`WebSocketClientTransport`（客户端工厂，[WebSocketClientTransport.cpp](../../src/transport/WebSocketClientTransport.cpp)）。构造 `(url, name = "websocket")`。

## 会话实现

- 成员持有 libhv `WebSocketClient ws_`
- `Start()`：回调捕获 **`std::weak_ptr`**（`lock()` 判空，防悬挂且不持有会话）；`onopen` → `running_ = true; SetConnected()`；`onclose` → `running_ = false; SetDisconnected()`；`onmessage` → 反序列化 + 入通道，解析失败 `NotifyError`；最后 `ws_.open(url)`
- **注意：`SetConnected` 完全由 onopen 回调触发，`Start()` 本身不调用**
- `Close()`：`running_ = false` → **显式置空三个回调（`onopen/onclose/onmessage = nullptr`）打破循环引用**（原实现每连接永久泄漏对象与线程）→ `ws_.close()` → 关通道 → SetDisconnected
- `SendMessageAsync`：`!running_` 直接丢弃；序列化后 `ws_.send(json_str)`

## 相关页面

- [/modules/transport.md](../modules/transport.md) — 所属库
- [/classes/message-channel.md](../classes/message-channel.md) — 消息载体
- [/concepts/concurrency.md](../concepts/concurrency.md) — 回调线程与生命周期
