---
type: Class
title: MessageChannel
description: 有界异步消息队列（std::queue + mutex + condition_variable），替代 asio::experimental::channel。
tags: [并发, 队列, 传输]
timestamp: 2026-08-13T12:36:14+08:00
resource: include/mcp/protocol/MessageChannel.hpp
---

# MessageChannel

有界异步队列（[MessageChannel.hpp](../../include/mcp/protocol/MessageChannel.hpp)），默认容量 `max_buffer = 64`（`TransportBase` 显式传 `detail::kChannelCapacity`）。双 condition_variable（收/发）+ 原子 `closed_`。

## API

- `AsyncReceive(cb)`：阻塞等消息；closed 时回调 `errc::operation_canceled`；**回调在锁外调用**
- `Send(msg)`：缓冲区满则阻塞；closed 时返回 false 丢弃
- `TrySend(msg)`：非阻塞，满/关闭返回 false
- `Close()`：唤醒所有等待者；`IsOpen()` / `Empty()`

## 语义要点

- 同步传输的关键：`InMemoryTransport` 的消息在 `Send()`/`AsyncReceive()` 时**即时交付**（无外部事件循环）——测试因此是同步的
- `Send` 返回 false（通道已关）时 `TransportBase::WriteMessage` 记 Warning `"message dropped: channel closed"` 不抛异常

## 相关页面

- [/modules/transport.md](../modules/transport.md) — 所属库
- [/transports/in-memory.md](../transports/in-memory.md) — 最直接的使用方
- [/concepts/concurrency.md](../concepts/concurrency.md) — 线程模型
- [/tests.md](../tests.md) — 同步测试语义
