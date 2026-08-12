---
type: Transport
title: SSE 客户端传输
description: 服务端推送事件流（GET）+ POST 消息通道，支持 Last-Event-ID 断线回放与指数退避重连。
tags: [transport, sse, libhv, 重连]
timestamp: 2026-08-13T03:25:00+08:00
resource: src/transport/SseClientTransport.cpp
---

# SSE 客户端传输

`SseClientTransport`（客户端工厂，[SseClientTransport.cpp](../../src/transport/SseClientTransport.cpp)）。构造 `(server_url, name = {})`，name 空回落 `"sse"`。

## 会话实现

- 持有**两个独立 libhv HttpClient**：`http_client_`（GET SSE 流）+ `post_client_`（POST 消息）
- `Start()`：SSE 读线程 + 发送线程双线程，启动即 `SetConnected()`
- `Close()`：notify 发送线程 → 关两个 client（**靠关客户端解除阻塞**）→ join → 关通道 → SetDisconnected

## SSE 协议细节

- GET 带 `Accept: text/event-stream`；有 `last_event_id_` 时带 `Last-Event-ID` 头（断线回放）
- 解析支持 `event:` / `data:` / `id:` 行；多行 data 用 `\n` 拼接；`id:` 更新 `last_event_id_`（无 id 则清空）
- `event: endpoint` → 解析 POST 端点（相对路径拼在 `scheme://host[:port]` 后，默认端口省略）；`event: message` → 超限丢弃 + 反序列化 + 入通道
- 流结束且非显式关闭时**指数退避重连**：`kBackoffBase = 1000ms`、最多 5 步（1s/2s/4s/8s/16s），期间每 100ms 检查 `running_`
- 发送线程：条件变量取队列，POST 到 endpoint，`Content-Type: application/json`

## 相关页面

- [/modules/transport.md](../modules/transport.md) — 所属库
- [/transports/streamable-http.md](streamable-http.md) — HTTP 传输模式中的 SSE 回退
- [/classes/http-server.md](../classes/http-server.md) — 服务端 SSE 广播
- [/concepts/concurrency.md](../concepts/concurrency.md) — 线程模型
