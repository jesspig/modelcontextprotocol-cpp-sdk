---
type: Transport
title: Stdio 传输
description: 服务端（stdin/stdout 管道）+ 客户端（子进程）双向传输，'\n' 分隔的 JSON-RPC 行。
tags: [transport, stdio, 管道, 子进程]
timestamp: 2026-08-13T03:25:00+08:00
resource: src/transport/StdioServerTransport.cpp
---

# Stdio 传输

服务端（[StdioServerTransport.cpp](../../src/transport/StdioServerTransport.cpp)）与客户端（[StdioClientTransport.cpp](../../src/transport/StdioClientTransport.cpp)）共享同一套读循环模板：`\n` 切行 + 超限丢弃 + `IsEof()` 判停。

## 服务端

- 无参构造；`Start()` 幂等（`running_.exchange(true)`），`OpenStandardInput/Output()` 打开管道，启动读线程
- `Close()`：先关管道（**靠关管道解除读线程阻塞**）→ `JoinThreadSafely` → 关通道 → `SetDisconnected`
- 发送：`SerializeMessage(message) + "\n"` 写 stdout；写字节数不符 → `NotifyError`
- 读到真 EOF 且 `running_` 仍为真时自行收尾：关通道 + SetDisconnected

## 客户端（工厂）

- 选项：`command / arguments / name / working_directory / inherit_environment_variables(true) / environment_variables`
- `Connect()`：选项转 `ProcessStartInfo` → `CreateProcess` → 构造匿名命名空间会话传输 → 启动即 `SetConnected()`
- `Close()`：关 stdin 管道 → `process_->Terminate(5000)` → 关 stdout 管道 → join → 关通道
- `Name()` 空时返回 `"stdio"`

## 陷阱

`PipeHandle::Read` 返回 0 **不一定是 EOF**（POSIX poll 100ms 超时无数据即返回 0）——读循环必须用 `IsEof()` 区分"超时无数据"（继续轮询）与"真 EOF"（退出），否则空闲时被误判为断连。

## 相关页面

- [/modules/transport.md](../modules/transport.md) — 所属库
- [/concepts/concurrency.md](../concepts/concurrency.md) — 线程与 Close 语义
- [/classes/message-channel.md](../classes/message-channel.md) — 消息载体
- [/examples/EchoServer](../../examples/EchoServer) — 使用示例（stdio 服务端）
