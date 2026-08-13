---
type: Transport
title: InMemory 传输
description: 测试用同步传输：CreatePair 生成 client/server 双向通道，消息即时交付。
tags: [transport, 测试, 同步]
timestamp: 2026-08-13T12:36:14+08:00
resource: src/transport/InMemoryTransport.cpp
---

# InMemory 传输

`InMemoryTransport::CreatePair()`（[InMemoryTransport.cpp](../../src/transport/InMemoryTransport.cpp)）返回 `Pair{client, server}`（各为 `shared_ptr<ITransport>`）。

## 实现

- 创建两个 `MessageChannel(kChannelCapacity)`：`c2s` 与 `s2c`；client 绑定 (recv=s2c, send=c2s)，server 反向绑定
- `InMemoryTransportImpl`（匿名命名空间）：`SendMessageAsync` → `send_channel_->Send()`（失败记 Warning）；`Close()` 幂等——已 Disconnected 直接返回
- **同步语义**：消息在 `Send()`/`AsyncReceive()` 时即时交付，无外部事件循环——测试无需等待异步

## 注意

`CreatePair` 创建的实现**不调用 `SetConnected()`**（状态保持 Initial）——测试经 `dynamic_cast<TransportBase*>` 访问状态机时可观察。

## 相关页面

- [/modules/transport.md](../modules/transport.md) — 所属库
- [/classes/message-channel.md](../classes/message-channel.md) — 通道实现
- [/tests.md](../tests.md) — 同步测试语义
- [/examples/SimpleClient](../../examples/SimpleClient) — 使用示例
