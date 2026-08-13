---
type: Class
title: HttpServer
description: 基于 libhv HttpService 的 PIMPL HTTP 服务端：SSE 广播、并发限制、Host/Origin 校验。
tags: [http, libhv, sse, pimpl]
timestamp: 2026-08-13T12:36:14+08:00
resource: include/mcp/http/HttpServer.hpp
---

# HttpServer

PIMPL 结构（[HttpServer.cpp](../../src/http/HttpServer.cpp)）：`HttpServerImpl` 持有 `shared_ptr<HttpService>` + `unique_ptr<HttpServer>` + 一份 `HttpServerOptions` 拷贝。`impl_` 用 `shared_ptr + atomic_load/store` 管理，SSE onclose 回调捕获 impl 的 shared_ptr 而非 `this`，保证服务 Stop 后回调仍有效。

## 关键行为

- worker 线程 `kHttpWorkerThreads = 8`
- `running_` 为 `std::atomic<bool>`：`Start()` 以 `exchange(true)` 防重入，`Stop()` 以 `exchange(false)`，`SetHandler` 用 `load()` 检查
- `Start()`：AllowCORS → setPort → 注册全部 handler 为 async_handler 路由 → 非阻塞 start；hv 请求头全部转小写
- `Stop()`：`server->stop()`（join 全部事件循环线程，SSE onclose 回调同步在其中执行）+ `service.reset()` 移入**独立 `std::thread`** 后 `JoinThreadSafely`——从 libhv worker 回调内调用 `Stop()`（如 handler 中触发销毁）不会自 join 死锁或 UAF（self-join 防护）
- **`SetHandler` 必须在 `Start()` 之前**：运行期抛 `std::logic_error`
- `on_request` 钩子在处理器之前调用，抛异常仅记 Warning 不中断
- Host 校验（403）：无 Host 直接拒；`allowed_hosts` 非空精确匹配，否则仅允许 localhost/127.0.0.1/::1；Origin 仅在 `allowed_origins` 非空时强制匹配
- handler 非 McpError 异常 → 500
- SSE 写路径：先写 `Content-Type: text/event-stream` 等头，`EndHeaders()` 后显式写 body，**不调用 End()** 保持连接

## SSE 客户端管理

- `on_connect`/`on_disconnect` 已接线：SSE 客户端注册/移除时触发（拷贝语义，勿 move 取走）
- **`on_disconnect` 三条移除路径统一"恰好一次"**：onclose 回调、`RemoveSseClient`、`BroadcastSse` 写失败分支均以 `erase` 返回的 `removed` 标志决定是否**锁外**调用回调（`sse_mutex` 内只移除与拷贝回调，锁外执行）
- `BroadcastSse` 对已断开连接写失败会**自动移除**该客户端（锁外拷贝 entry 列表逐个发送，每个 entry 有独立 `write_mutex`，异常按 entry 自清理）
- 注册前有 `writer->isClosed()` 预检；`next_sse_id` 从 1 递增

## 相关页面

- [/modules/http.md](../modules/http.md) — 所属库
- [/transports/streamable-http.md](../transports/streamable-http.md) — 路由消费方
- [/transports/sse.md](../transports/sse.md) — SSE 客户端
