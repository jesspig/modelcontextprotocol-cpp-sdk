---
type: Transport
title: Streamable HTTP 传输
description: 2026 时代 HTTP 传输：双端实现、stateless 默认、POST SSE 请求响应、Mcp-Method 头、SSE 回放与 504 语义。
tags: [transport, http, streamable, stateless, winhttp]
timestamp: 2026-08-15T02:40:07+08:00
resource: src/http/StreamableHttpServerTransport.cpp
---

# Streamable HTTP 传输

服务端（[StreamableHttpServerTransport.cpp](../../src/http/StreamableHttpServerTransport.cpp)）与客户端（[StreamableHttpClientTransport.cpp](../../src/http/StreamableHttpClientTransport.cpp)）。

## 服务端

- 选项：`port`（默认 3001）、`endpoint`（默认 `/mcp`）、`stateless`（**默认 true**，对齐 python/rust/go/csharp 2026；false 为 sessionful 传统模式）、`enable_legacy_sse`（默认 true）、`sse_keep_alive_ms`（SSE 注释帧间隔毫秒，默认 15000，0 禁用）、可注入 `event_store`、`server_name/server_version`；`session_id_ = "srv-" + 时钟计数`
- 路由：POST 与 DELETE 总是注册，GET 仅 `enable_legacy_sse` 时
- **POST**：body 超限（4MiB）→ 413 `-32700`；解析失败 → 400；Mcp-Method/Mcp-Name 头与 body 不符 → 400 `HeaderMismatch`；回显 `mcp-protocol-version/mcp-method/mcp-name` 响应头；`mcp-param-*` 请求头存入 `req.meta["x-mcp-headers"]`
  - **请求（stateless 与 stateful 同一路径）**：inflight 上限 8（仅 stateless）→ 503 `"server busy"`；channel 关闭/TrySend 失败 → 503 `-32000`；送入 channel 前登记 `pending_responses_`（`pending_mutex_` 保护），`SendMessageAsync` 匹配到响应时 set promise
    - 成功：**200 + `text/event-stream`**，body 为 SSE 首帧 `event: message\ndata: <serialized response>\n\n`，头含 `cache-control: no-cache`、`x-accel-buffering: no`；`sse_close_after_write = true`（见 HttpServer 页）——**响应不再经 GET 流广播**（2025-era 客户端依赖 GET 收响应属已知协议行为变化）
    - 协议错误（JsonRpcErrorResponse）按规范映射 HTTP 状态码：`-32020/-32021/-32022/-32600/-32602/-32700` → **400**，`-32601` → **404**，body 为 JSON-RPC error JSON（`application/json`）；其余错误码仍 200 + SSE 流
    - 30s（`kStatelessTimeout`）无响应 → **504** + JSON `-32000`
  - 通知：fire-and-forget，**202 + `{}`**（202 仅用于服务端发出的通知/响应确认）
- **GET（SSE 流）**：仅承载服务端→客户端**通知**（`event: endpoint` 事件 + 非 stateless 时 `Last-Event-ID` 回放，stateless 不回放）；连接存活期间按 `sse_keep_alive_ms`（默认 15s）周期性广播注释帧 `: ping\r\n\r\n`（对齐 python `_SSE_PING_INTERVAL=15s` / ts `DEFAULT_SSE_KEEP_ALIVE_MS=15000`）
- **DELETE**：stateless → 405 `-32601`；否则关 channel、`SetDisconnected()`、200 `{}`
- `SendMessageAsync`：**无条件**先查 `pending_responses_`（响应/错误响应按 id 匹配则 set promise 并返回，不广播）；未匹配的消息经 `BuildSseEvent(std::move(message))`（`SerializeMessage(std::move)`）生成 `event: message`，非 stateless 时 Append 到 EventStore 并带 `id:` 前缀，最后 `BroadcastSse`
- `Close()`：停 HttpServer、非 stateless 清 EventStore、关通道、SetDisconnected

## 客户端

- 选项：`endpoint / transport_mode（默认 AutoDetect）/ name / known_session_id / additional_headers / auth_challenge_handler`（RFC 9728：401/403 收到 WWW-Authenticate 时回调，返回非空 Authorization 头则**恰好重试一次**）
- `HttpTransportMode`：`AutoDetect`（先试 StreamableHttp，失败回落 SSE）/ `StreamableHttp` / `Sse`
- **平台双实现**：Win32 用 WinHTTP（`#pragma comment(lib, "winhttp.lib")`），POSIX 用自研 `detail::net::HttpClient`；发送路径共用 `send_thread_ + send_queue_ + condition_variable`；Win32 会话 `Start()` 补 `SetConnected()`（与 POSIX 对齐，状态机不再恒为 Initial）
- **Mcp-Method 头动态生成**：解析 body 的 method 字段（SEP-2243）；另生成 `Mcp-Param-*`（string/int/bool/double）与 `Mcp-Name`（params.name 回退 uri）；解析失败回退（Win32 → `tools/call`，POSIX → `unknown`）
- **每请求固定带 `MCP-Protocol-Version: 2026-07-28` 头**（Win32/POSIX 两分支一致；transport 层无协商状态，固定取最新版本）
- **会话头（stateful 兼容）**：`known_session_id` 非空则从首个 POST 起携带 `Mcp-Session-Id` 请求头；任意响应（含 4xx）返回 `Mcp-Session-Id` 头时捕获为当前会话 id（存入会话传输内部状态），后续请求携带——stateless 服务端不发该头则全程不带，行为不变
- **Close 会话终止**：`Close()` 置 `delete_pending_` 唤醒发送线程，发送线程退出循环后**仅当已持有会话 id** 时同步发送 `DELETE`（带 `Mcp-Session-Id` 头；Win32 独立 WinHTTP 请求 / POSIX `HttpClient`），随后 join——无会话 id（stateless）不发 DELETE，默认路径无额外请求
- **响应分流**（两分支一致）：
  - 4xx/5xx：401/403 challenge 重试优先；否则解析 body 为 JSON-RPC error（如 404 + `-32601`）成功则**入 channel**（连接保持），失败才 `NotifyError`
  - **202**：通知确认，**忽略**（body 含 `id` 时记 Warning 视为异常，否则 Info）；不再存在"伪响应"路径
  - `Content-Type` 含 `text/event-stream`：**同步读完整 SSE 流**（服务器写完事件后关闭连接；Win32 原先独立 SSE 读线程已移除，读由 send 线程承担，`sse_request_` 仅作 Close 中断句柄），`\n\n` 分块取 `data:` 行反序列化入 channel；单块超限（8MB）→ `NotifyError`
  - 其余：单 JSON 响应解析入 channel（超限 → 错误）
- 超时：Win32 30s / POSIX 30s
- `Name()` 空时返回 `"streamable-http"`

## 相关页面

- [/modules/http.md](../modules/http.md) — 所属库
- [/classes/http-server.md](../classes/http-server.md) — 底层 HTTP 服务（含 `sse_close_after_write`）
- [/transports/sse.md](sse.md) — SSE 回退模式
- [/concepts/version-negotiation.md](../concepts/version-negotiation.md) — 2026 时代无状态语义
