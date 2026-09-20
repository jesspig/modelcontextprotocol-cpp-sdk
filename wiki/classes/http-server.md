---
type: Class
title: HttpServer
description: 自研 HTTP/1.1 PIMPL 服务端：SSE 广播、单响应流关闭、并发限制、Host/Origin 校验。
tags: [http, sse, pimpl]
timestamp: 2026-09-20T18:14:09+08:00
resource: include/mcp/http/HttpServer.hpp
---

# HttpServer

PIMPL 结构（[HttpServer.cpp](../../src/http/HttpServer.cpp) + [HttpServerImpl.hpp](../../src/http/HttpServerImpl.hpp)）：`HttpServerImpl` 即自研实现 `mcp::detail::http_server_impl::Impl`，持有 `HttpServerOptions` 拷贝、监听 fd、accept 线程、连接线程表（`conn_threads_`/`conn_fds_`，后者存 `shared_ptr<TcpSocket>`）与 SSE 客户端表。`impl_` 用 `shared_ptr + atomic_load/store` 管理，`HttpServer::Stop` 的 stopper 线程持有保证存活；SSE 写回调捕获连接对象（`shared_ptr<TcpSocket>`，[HttpServerImpl.cpp:481](../../src/http/HttpServerImpl.cpp)），连接线程读循环结束后直接 `RemoveSseClient(id)` 移除。

## 关键行为

- accept 线程 + 每连接独立线程，连接数上限 `kMaxConnections = 256`（超出直接 503）
- `running_` 为 `std::atomic<bool>`：`Start()` 以 `exchange(true)` 防重入，`Stop()` 以 `exchange(false)`，`SetHandler` 用 `load()` 检查
- `Start()`：建监听 socket（SO_REUSEADDR、backlog 16）→ 起 accept 线程；请求行/头在连接线程内解析，**请求头名统一转小写**
- `Stop()`：关 listen fd，对每条连接调用 `conn->Close()`（连接对象化——accept 线程与连接线程共享同一 `shared_ptr<TcpSocket>`，**不再裸 `close` 原生 fd**，消除连接线程自清理与 Stop 并发 close 的 double-close 竞态）解除读写阻塞 → join **keepalive 线程**（在连接关闭之后，避免阻塞写卡住）→ join accept 线程与全部连接线程 → 释放 impl，移入**独立 `std::thread`** 后 `JoinThreadSafely`——从连接线程回调内调用 `Stop()`（如 handler 中触发销毁）不会自 join 死锁或 UAF（self-join 防护）
- **`SetHandler` 必须在 `Start()` 之前**：运行期抛 `std::logic_error`
- `on_request` 钩子在处理器之前调用，抛异常仅记 Warning 不中断
- Host 校验（403）：无 Host 直接拒；`allowed_hosts` 非空精确匹配，否则仅允许 localhost/127.0.0.1/::1；Origin 仅在 `allowed_origins` 非空时强制匹配
- `bind_host` 可配置监听地址（`HttpServerOptions::bind_host`）：空 = 仅 IPv4 `INADDR_ANY`（向后兼容）；非空时 `Start()` 按字面量选族——先 `inet_pton(AF_INET6)` 再 `inet_pton(AF_INET)`，支持 IPv4/IPv6 字面量（含可选 `[]` 包围），均失败抛 `std::runtime_error("HttpServer: invalid bind_host: ...")`；bind 前打印 `binding <addr>:<port>`（Info），bind 失败消息补充地址
- handler 抛出的**任何 `std::exception`（含 McpError）→ 500**（[HttpServerImpl.cpp:453](../../src/http/HttpServerImpl.cpp)）；传输层（`StreamableHttpServerTransport`）在内部消化错误并映射 400/404，`McpError` 不会逃逸到本层
- **SSE 写路径**：`WriteSseHeaders`（`Connection: keep-alive/close` 由 `close_after_write` 决定）先写响应头与首包体，随后连接线程进入读循环（`IsEof()` 判停）保持连接；`Content-Type: text/event-stream` 与 `Cache-Control: no-cache` 为默认值，调用方 headers 中已带同名头（大小写不敏感）时不重复写入
- **`sse_close_after_write`（HttpResponse 新字段）**：`is_sse` 且该标志为真时——写完首包体后 `conn->Close()` 直接返回，**跳过 `AddSseClient`/EOF 读循环/`RemoveSseClient`**，也不触发 `on_connect`/`on_disconnect`。用于 POST 单响应 SSE 流（Streamable HTTP 请求响应，写完即关闭，避免 EOF 循环永久阻塞）；GET 长连接流不受影响

## SSE 客户端管理

- `on_connect`/`on_disconnect` 已接线：SSE 客户端注册/移除时触发（拷贝语义，勿 move 取走）
- **`on_disconnect` 移除路径统一"恰好一次"**：连接读循环结束（`RemoveSseClient(id)`）、公共 `RemoveSseClient`、`BroadcastSse`/keepalive 写失败分支均以移除结果 `removed` 决定是否**锁外**调用回调（`sse_mutex` 内只移除与拷贝回调，锁外执行）
- `BroadcastSse` 对已断开连接写失败会**自动移除**该客户端（锁外拷贝 entry 列表逐个发送，每个 entry 有独立 `write_mutex`，异常按 entry 自清理）
- `SseClientCount()`（`HttpServer` 公开方法 + `Impl` 实现）：持 `sse_mutex_`（为支持 const 方法已改为 `mutable`）返回 `sse_clients_.size()`，`HttpServer` 层经 `atomic_load(impl_)`、空 impl 返回 0；唯一调用方为 `StreamableHttpServerTransport::SendMessageAsync` 的 server-initiated 请求有界等待（零监听者时 50ms 步长轮询至多 2s，`Disconnected` 可中断，超时 Warning 后仍广播；Notification 不走等待）
- **SSE keepalive 线程**（`options_.sse_keep_alive_ms > 0` 时在 `Start()` 启动）：`wait_for` 谓词仅 `!running_`——每轮睡满 `sse_keep_alive_ms` 间隔后，有客户端则向 `sse_clients_` 广播注释帧 `: ping\r\n\r\n`（`kSsePingFrame`）、无客户端空转一轮；仅 `Stop()`（`running_` 置 false + `notify_all`）唤醒退出（有 SSE 客户端不再触发忙循环）；写失败按 entry 自清理；`Stop()` 在关闭连接 fd 后 join
- `next_sse_id` 从 1 递增

## 相关页面

- [/modules/http.md](../modules/http.md) — 所属库
- [/transports/streamable-http.md](../transports/streamable-http.md) — 路由消费方
- [/transports/sse.md](../transports/sse.md) — SSE 客户端
