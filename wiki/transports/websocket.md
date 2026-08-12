---
type: Transport
title: WebSocket 客户端传输
description: 基于 libhv WebSocketClient 的客户端传输，连接状态由 onopen/onclose 回调驱动。
tags: [transport, websocket, libhv]
timestamp: 2026-08-13T03:25:00+08:00
resource: src/transport/WebSocketClientTransport.cpp
---

# WebSocket 客户端传输

`WebSocketClientTransport`（客户端工厂，[WebSocketClientTransport.cpp](../../src/transport/WebSocketClientTransport.cpp)）。构造 `(url, name = "websocket")`。

## 会话实现

- 成员持有 libhv `WebSocketClient ws_`
- `Start()`：`shared_from_this()` 捕获 self 供回调（防悬挂）；`onopen` → `running_ = true; SetConnected()`；`onclose` → `running_ = false; SetDisconnected()`；`onmessage` → 反序列化 + 入通道，解析失败 `NotifyError`；最后 `ws_.open(url)`
- **注意：`SetConnected` 完全由 onopen 回调触发，`Start()` 本身不调用**
- `Close()`：`running_ = false` → `ws_.close()` → 关通道 → SetDisconnected
- `SendMessageAsync`：`!running_` 直接丢弃；序列化后 `ws_.send(json_str)`

## 相关页面

- [/modules/transport.md](../modules/transport.md) — 所属库
- [/classes/message-channel.md](../classes/message-channel.md) — 消息载体
- [/concepts/concurrency.md](../concepts/concurrency.md) — 回调线程与生命周期
