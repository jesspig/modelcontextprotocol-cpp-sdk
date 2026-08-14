---
type: Transport
title: Streamable HTTP 传输
description: 2026 时代 HTTP 传输：双端实现、stateless 模式、Mcp-Method 头、SSE 回放与 504 语义。
tags: [transport, http, streamable, stateless, winhttp]
timestamp: 2026-08-13T16:30:00+08:00
resource: src/http/StreamableHttpServerTransport.cpp
---

# Streamable HTTP 传输

服务端（[StreamableHttpServerTransport.cpp](../../src/http/StreamableHttpServerTransport.cpp)）与客户端（[StreamableHttpClientTransport.cpp](../../src/http/StreamableHttpClientTransport.cpp)）。

## 服务端

- 选项：`port`（默认 3001）、`endpoint`（默认 `/mcp`）、`stateless`（默认 false）、`enable_legacy_sse`（默认 true）、可注入 `event_store`、`server_name/server_version`；`session_id_ = "srv-" + 时钟计数`
- 路由：POST 与 DELETE 总是注册，GET 仅 `enable_legacy_sse` 时
- **POST**：body 超限（8MB）→ 413 `-32700`；解析失败 → 400；Mcp-Method/Mcp-Name 头与 body 不符 → 400 `HeaderMismatch`；回显 `mcp-protocol-version/mcp-method/mcp-name` 响应头；`mcp-param-*` 请求头存入 `req.meta["x-mcp-headers"]`
  - stateless：inflight 达 **8** → 503 `"server busy"`；30s（`kStatelessTimeout`）无响应 → **504**；成功 → 200 + 响应体
  - 非 stateless：`TrySend` 失败 → 503；成功 → **202 Accepted** + `{"result":{"resultType":"complete"}}`
  - 通知：fire-and-forget，202 + `{}`
- **GET（SSE 流）**：`event: endpoint` 事件；非 stateless 时支持 `Last-Event-ID` 回放（追加 `id:` 行）；stateless 不回放
- **DELETE**：stateless → 405 `-32601`；否则关 channel、`SetDisconnected()`、200 `{}`
- `SendMessageAsync`：stateless 先查 `pending_responses_` 完成 promise 关联；正常路径 `BuildSseEvent(std::move(message))`（内部 `SerializeMessage(std::move)`）生成 `event: message`，非 stateless 时 Append 到 EventStore 并带 `id:` 前缀，最后 `BroadcastSse`；stateless 响应体亦为 `SerializeMessage(std::move(response))`
- `Close()`：停 HttpServer、非 stateless 清 EventStore、关通道、SetDisconnected

## 客户端

- 选项：`endpoint / transport_mode（默认 AutoDetect）/ name / known_session_id / additional_headers / auth_challenge_handler`（RFC 9728：401/403 收到 WWW-Authenticate 时回调，返回非空 Authorization 头则**恰好重试一次**）
- `HttpTransportMode`：`AutoDetect`（先试 StreamableHttp，失败回落 SSE）/ `StreamableHttp` / `Sse`
- **平台双实现**：Win32 用 WinHTTP（`#pragma comment(lib, "winhttp.lib")`），POSIX 用自研 `detail::net::HttpClient`；发送路径共用 `send_thread_ + send_queue_ + condition_variable`；Win32 会话 `Start()` 补 `SetConnected()`（与 POSIX 对齐，状态机不再恒为 Initial）
- **Mcp-Method 头动态生成**：解析 body 的 method 字段（SEP-2243）；另生成 `Mcp-Param-*`（string/int/bool/double）与 `Mcp-Name`（params.name 回退 uri）；解析失败回退（Win32 → `tools/call`，POSIX → `unknown`）
- 响应：4xx/5xx 视为非 JSON-RPC 负载；`Content-Type` 含 `text/event-stream` 走 SSE 读循环（`\n\n` 分块，仅取 `data:` 行，多行拼接；单块超限（8MB）→ `NotifyError`；响应体超限 → 错误）
- 超时：Win32 30s / POSIX 30s
- `Name()` 空时返回 `"streamable-http"`

## 相关页面

- [/modules/http.md](../modules/http.md) — 所属库
- [/classes/http-server.md](../classes/http-server.md) — 底层 HTTP 服务
- [/transports/sse.md](sse.md) — SSE 回退模式
- [/concepts/version-negotiation.md](../concepts/version-negotiation.md) — 2026 时代无状态语义
