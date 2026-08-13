---
type: Module
title: mcp-http HTTP 库
description: HttpServer（自研实现）、EventStore（SSE 回放）、Streamable HTTP 双端传输。
tags: [http, sse, webserver]
timestamp: 2026-08-13T16:30:00+08:00
resource: src/http/HttpServer.cpp
---

# mcp-http HTTP 库

依赖 `mcp-transport`；HTTP 服务端与客户端均为自研实现，Win32 客户端额外链接 winhttp。

## 组成

| 组件 | 职责 | 页 |
|------|------|----|
| HttpServer | 自研 HTTP/1.1 服务端的 PIMPL 封装 | [/classes/http-server.md](../classes/http-server.md) |
| EventStore | SSE 事件存储与断线回放 | 同下 |
| StreamableHttpServerTransport | Streamable HTTP 服务端传输 | [/transports/streamable-http.md](../transports/streamable-http.md) |
| StreamableHttpClientTransport | Streamable HTTP 客户端传输（WinHTTP / 自研 HttpClient 双实现） | 同上 |

## 关键行为

- accept 线程 + 每连接一线程（上限 256，超出 503）；stateless 并发上限 8（超出 503 `"server busy"`），同步等待超时 30s 返回 **504**（非 500）
- 错误体均为 JSON-RPC 格式：413（超限 body）`-32700`、400（解析失败）`-32700 Parse error`、400（头不匹配）`-32020 HeaderMismatch`、503 `-32000 server closed`、504 `-32000`
- SSE 广播带 `id:` 行；GET 支持 `Last-Event-ID` 断线回放（stateless 不回放）
- `Mcp-Method` 头动态生成：解析 JSON-RPC body 的 method 字段写入（SEP-2243）；`mcp-param-*` 头往返镜像
- EventStore：每会话上限 1024 事件，超出从头部裁剪
- `Stop()` 的关闭序列（关 listen/连接 fd 解除阻塞 → join accept 与全部连接线程 → 释放 impl）移入独立 `std::thread` + `detail::JoinThreadSafely`（self-join 防护，[HttpServer.cpp:66](../../src/http/HttpServer.cpp)）
- `running_` 为 `std::atomic<bool>`：`Start` 用 `exchange(true)`、`Stop` 用 `exchange(false)`、`SetHandler` 用 `load()` 检查
- `on_disconnect` 三条移除路径（SSE `onclose` / `RemoveSseClient` / `BroadcastSse` 写失败）统一"恰好一次"：`removed` 标志保证回调只在真正移除时触发一次，且回调在锁外执行

## 相关页面

- [/classes/http-server.md](../classes/http-server.md)
- [/transports/streamable-http.md](../transports/streamable-http.md)
- [/transports/sse.md](../transports/sse.md) — SSE 客户端
- [/modules/transport.md](transport.md) — 依赖的下层
