---
type: Module
title: mcp-http HTTP 库
description: HttpServer（自研实现）、EventStore（SSE 回放）、Streamable HTTP 双端传输、客户端发送侧独立 POST 与边读边分发。
tags: [http, sse, webserver, streamable]
timestamp: 2026-09-11T08:40:00+08:00
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
| StreamableHttpClientTransport | Streamable HTTP 客户端传输（WinHTTP / 自研 HttpClient 双实现，POST + GET SSE 接收流） | 同上 |

## 关键行为

- accept 线程 + 每连接一线程（上限 256，超出 503）；stateless 并发上限 8（超出 503 `"server busy"`），同步等待超时 30s 返回 **504**（非 500）
- 错误体均为 JSON-RPC 格式：413（超限 body）`-32700`、400（解析失败）`-32700 Parse error`、400（头不匹配）`-32020 HeaderMismatch`、503 `-32000 server closed`、504 `-32000`
- SSE 广播带 `id:` 行；GET 支持 `Last-Event-ID` 断线回放（stateless 不回放）
- `Mcp-Method` 头：客户端从 JSON-RPC body 的 method 字段动态生成（[StreamableHttpClientTransport.cpp:524](../../src/http/StreamableHttpClientTransport.cpp)，POSIX 分支 `:1059`）；服务端在响应中**回显** `mcp-method`/`mcp-name`/`mcp-protocol-version`（SEP-2243，[StreamableHttpServerTransport.cpp:227](../../src/http/StreamableHttpServerTransport.cpp)）；`mcp-param-*` 头往返镜像
- Streamable HTTP 客户端 **GET SSE 接收流**：`enable_listen_stream`（默认 true），发送 `notifications/initialized` 后自动开 GET 长流接收服务端通知，消息经 MessageChannel 并入会话引擎；405 视为服务器不支持（静默放弃）；断线退避重连（1s 起倍增封顶 30s、最多 5 次）携带 `Last-Event-ID`；详见 [/transports/streamable-http.md](../transports/streamable-http.md)
- **发送侧分流**（解开 server→client 请求互等死锁）：Request 保持 `send_thread_` 串行队列；**Notification/Response 经 `LaunchImmediatePost` 独立即时 POST**（短命 detached 线程 `mcp-post`，`shared_from_this` 保活），不再排在在途 Request 之后——否则 elicitation 完成通知会被挂起的 tools/call POST 阻塞，server→client 请求互等死锁
- **接收侧边读边分发**：POST 响应 SSE 流在 `DoPost` 分块回调内即时按 `\n\n` 分块分发（`DispatchSseBlock`），server→client 请求在流打开期间即可达（原"读完整个流再处理"会饿死并发请求）；Win32 `WinHttpReadData` 填满缓冲语义改为 `WinHttpQueryDataAvailable` 增量读（DoPost SSE 与 DoListenGet 同修）
- **404 类型化**（会话过期自愈入口）：4xx body 无法解析为 JSON-RPC error 且状态码 404 时，构造 `SessionExpired(-32009)` 错误响应交付 channel（`MakeSessionExpiredError`），客户端据此重协商并重放（见 [/modules/client.md](client.md)）
- **`MCP-Protocol-Version` 头自学习**：initialize 请求不带该头，从 initialize 响应 `result.protocolVersion` 学习，后续请求与 GET 流按协商版本携带（无学习值兜底 `2026-07-28`）；**initialize 请求同样不带 `Mcp-Session-Id` 头**（`SessionIdHeaderFor` 豁免）
- EventStore：每会话上限 1024 事件，超出从头部裁剪
- `Stop()` 的关闭序列（关 listen/连接 fd 解除阻塞 → join accept 与全部连接线程 → 释放 impl）移入独立 `std::thread` + `detail::JoinThreadSafely`（self-join 防护，[HttpServer.cpp:75](../../src/http/HttpServer.cpp)）
- `running_` 为 `std::atomic<bool>`：`Start` 用 `exchange(true)`、`Stop` 用 `exchange(false)`、`SetHandler` 用 `load()` 检查
- `HttpServerOptions::bind_host`：可配置监听地址（IPv4/IPv6 字面量，空 = `INADDR_ANY`），`HttpServerImpl::Start` 自动选族（`inet_pton` 先 `AF_INET6` 后 `AF_INET`）；详见 [/classes/http-server.md](classes/http-server.md)
- `on_disconnect` 三条移除路径（SSE `onclose` / `RemoveSseClient` / `BroadcastSse` 写失败）统一"恰好一次"：`removed` 标志保证回调只在真正移除时触发一次，且回调在锁外执行

## 相关页面

- [/classes/http-server.md](../classes/http-server.md)
- [/transports/streamable-http.md](../transports/streamable-http.md)
- [/transports/sse.md](../transports/sse.md) — SSE 客户端
- [/modules/transport.md](transport.md) — 依赖的下层
