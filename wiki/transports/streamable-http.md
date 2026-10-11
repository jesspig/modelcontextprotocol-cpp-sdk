---
type: Transport
title: Streamable HTTP 传输
description: 2026 时代 HTTP 传输：双端实现、stateless 默认、Bearer 鉴权挑战与 audience 校验、外部 SessionStore 会话接管（含容量上限与闲置过期）、任务头校验、版本头主动校验、x-mcp-header 参数头、POST SSE 请求响应（边读边分发）、GET SSE 接收流、404 类型化与 503/504 语义。
tags: [transport, http, streamable, stateless, winhttp, sse, bearer, audience]
timestamp: 2026-10-11T16:35:00+08:00
resource: src/http/StreamableHttpServerTransport.cpp
---

# Streamable HTTP 传输

服务端（[StreamableHttpServerTransport.cpp](../../src/http/StreamableHttpServerTransport.cpp)）与客户端（[StreamableHttpClientTransport.cpp](../../src/http/StreamableHttpClientTransport.cpp)）。

## 服务端

- 选项：`port`（默认 3001）、`endpoint`（默认 `/mcp`）、`host`（监听绑定地址，透传至 `HttpServerOptions::bind_host`，空 = `INADDR_ANY`）、`stateless`（**默认 true**，对齐 python/rust/go/csharp 2026；false 为 sessionful 传统模式）、`enable_legacy_sse`（**默认 false**，此前为 true；false 时不注册 GET，SSE 接收流不可用）、`sse_keep_alive_ms`（SSE 注释帧间隔毫秒，默认 15000，0 禁用）、可注入 `event_store`、`server_name/server_version`；`session_id_ = "srv-" + 时钟计数`
- **Bearer 鉴权**（RFC 6750/9728）：`bearer_auth`（`optional<BearerAuthConfig>`）设置即启用，`verify` 回调必填（缺失构造抛 `McpError(InvalidRequest)`）；`resource_metadata_url`（挑战与元数据文档 URL）、`scopes_supported`、`required_scopes`（token scopes 须**全覆盖**，空禁用 scope 检查）、`authorization_servers`、`serve_metadata_endpoint`（默认 true）、`expected_resource`（期望 audience，空禁用 audience 检查）、`validate_token_resource`（默认 true）。`HandlePost`/`HandleGet` 入口先过 `AuthorizeRequest`：无 Authorization 头或非 `Bearer` 方案 → **401** + `WWW-Authenticate: Bearer resource_metadata="<url>"`；`verify` 不通过 → **401** + 追加 `error="invalid_token"`；audience 门（`validate_token_resource && !expected_resource.empty()`，归一化比较 `AuthResult.resource`，失配）→ **401** + `error="invalid_token"`；required_scopes 未全覆盖 → **403** + `error="insufficient_scope", scope="..."`；错误体均为 JSON-RPC `-32000`。auth gate 位于一切会话处理之前；audience 语义详见 [/concepts/oauth.md](../concepts/oauth.md)
- **受保护资源元数据端点**：`serve_metadata_endpoint` 时注册 `GET <resource_metadata_url 路径>`（默认 `/.well-known/oauth-protected-resource`），**匿名**访问；返回 RFC 9728 字段：`resource`（从 metadata URL 剥掉 well-known 后缀反推）、`authorization_servers`、`scopes_supported`、`bearer_methods_supported: ["header"]`
- **外部 SessionStore（stateful 模式）**：`session_store`（`SessionStore` 抽象，见 [/concepts/storage.md](../concepts/storage.md)）设置后会话可被共享同一 store 的其他实例接管（多实例部署/实例重启）；**构造期不再预存**初始记录——`initialize` 成功时才 `TrySave(ActiveSessionId())`（stateful + store 才执行），store 已满（`kMaxHttpSessions = 10000`）→ **503 + `-32000 "session store is full"`**；请求入口 `EnsureSession`——stateless 或无 store 直接放行，否则按请求头会话 id 查 store（命中且非当前则 `AdoptSession`，后续 `EventStore`/DELETE 均按 `ActiveSessionId`），未知 id → **404 + `-32009 Session expired`**（客户端自愈入口）；`initialize` 响应回 `mcp-session-id` 头；DELETE 时 `store->Remove`。`SessionRecord` 新增 `last_seen_ms`：`InMemorySessionStore::Load` 命中即刷新并在闲置超 `kSessionIdleTimeoutMs`（30 分钟）时按过期删除（返回 nullopt → 调用方 404），`TrySave` 在存前先 `SweepExpired`、满时返回 false（`Save` 转抛 `runtime_error "session store is full"`）；另有 `Size`/`SweepExpired` 供观测与清理
- 路由：POST 与 DELETE 总是注册，GET 仅 `enable_legacy_sse` 时（metadata 端点按 Bearer 配置另行注册）
- **版本头主动校验**：POST/GET 入口解析 `MCP-Protocol-Version`——非空且不在 `kProtocolVersions` 内 → **400 + `-32022 UnsupportedProtocolVersion`**（`RespondUnsupportedVersion`，此前未知版本头静默放行）；响应恒回 `mcp-protocol-version` 头（请求有值则回显，否则回 `kDefaultNegotiatedProtocolVersion` "2025-03-26"）
- **POST**：body 超限（4MiB；实际由 HTTP 服务层按 `Content-Length` 先行返回空体 413，此处为传输层兜底）→ 413 `-32700`；解析失败 → 400；Mcp-Method/Mcp-Name 头与 body 不符 → 400 `HeaderMismatch`；回显 `mcp-protocol-version/mcp-method/mcp-name` 响应头；`mcp-param-*` 请求头存入 `req.meta["x-mcp-headers"]`
- **任务方法头校验**：`RequiresMcpNameHeader` 覆盖 `tools/call`、`resources/read`、`prompts/get` **+ tasks/get、tasks/update、tasks/cancel**；tasks 三方法走专用分支——`params.taskId` 缺失/空 → 400（`tasks request missing valid taskId`），`Mcp-Name` 与 `taskId` 不等 → 400 `HeaderMismatch`（此为 HTTP 层校验，应用层另有 `WireCodec::ValidateTaskHeaders`，见 [/concepts/tasks-extension.md](../concepts/tasks-extension.md)）
  - **请求（stateless 与 stateful 同一路径）**：inflight 上限 8（仅 stateless）→ 503 `"server busy"`；channel 关闭/TrySend 失败 → 503 `-32000`；送入 channel 前登记 `pending_responses_`（`pending_mutex_` 保护），`SendMessageAsync` 匹配到响应时 set promise
    - 成功：**200 + `text/event-stream`**，body 为 SSE 首帧 `event: message\ndata: <serialized response>\n\n`，头含 `cache-control: no-cache`、`x-accel-buffering: no`；`sse_close_after_write = true`（见 HttpServer 页）——**响应不再经 GET 流广播**（2025-era 客户端依赖 GET 收响应属已知协议行为变化）
    - 协议错误（JsonRpcErrorResponse）按规范映射 HTTP 状态码：`-32020/-32021/-32022/-32600/-32602/-32700` → **400**，`-32601` → **404**，body 为 JSON-RPC error JSON（`application/json`）；其余错误码仍 200 + SSE 流
    - 30s（`kStatelessTimeout`）无响应 → **504** + JSON `-32000`
  - 通知：fire-and-forget，**202 + `{}`**（202 仅用于确认客户端发来的通知/响应，不等待也不承载 JSON-RPC 响应）
- **GET（SSE 流）**：承载 `SendMessageAsync` 中未匹配 pending 响应的服务端→客户端消息——通知与**请求**（如 `McpServer::Elicit` 的 `elicitation/create`）都经 `BroadcastSse` 走本流，响应则走 POST 流；请求在无监听者时先经有界等待（见下 `SendMessageAsync`），通知保持 fire-and-forget；首帧 `event: endpoint`，非 stateless 时按 `Last-Event-ID` 回放（stateless 不回放）；连接存活期间按 `sse_keep_alive_ms`（默认 15s）周期性广播注释帧 `: ping\r\n\r\n`（对齐 python `_SSE_PING_INTERVAL=15s` / ts `DEFAULT_SSE_KEEP_ALIVE_MS=15000`）
- **DELETE**：stateless → 405 `-32601`；否则关 channel、`SetDisconnected()`、200 `{}`
- `SendMessageAsync`：**无条件**先查 `pending_responses_`（响应/错误响应按 id 匹配则 set promise 并返回，不广播）；未匹配的消息中**仅 `IsRequest`（`JsonRpcRequest`）在快照为空时有界等待监听者**——`http_server_->SseClientCount() == 0` 时以 50ms 步长（`kSseListenerWaitStep`）轮询至多 2s（`kSseListenerWaitBudget`），期间 `TransportState::Disconnected` 可中断退出，超时记 Warning（`server-initiated request '<method>' broadcast with no SSE listener`）后仍走广播路径；**Notification 不等待、保持 fire-and-forget**（此前的"快照为空即经 `BroadcastSse` 静默丢弃"已不适用于请求）；之后经 `BuildSseEvent(std::move(message))`（`SerializeMessage(std::move)`）生成 `event: message`，非 stateless 时 Append 到 EventStore 并带 `id:` 前缀，最后 `BroadcastSse`（零监听者时仍是空操作）
- `Close()`：停 HttpServer、非 stateless 清 EventStore、关通道、SetDisconnected

## 客户端

- 选项：`endpoint / transport_mode（默认 AutoDetect）/ name / known_session_id / additional_headers / auth_challenge_handler`（RFC 9728：**401 必触发**回调；403 仅 `WWW-Authenticate` 含 `insufficient_scope`（大小写不敏感）时触发，其余 403 直接按错误体处理——返回非空 Authorization 头则**恰好重试一次**）/ `enable_listen_stream`（**默认 true**：发送 `notifications/initialized` 后自动开启 GET SSE 接收流，见下）/ `stateless`（**默认 false**：true 时不发送 `Mcp-Session-Id`/`Last-Event-ID`、不处理会话接管、不发送 DELETE，纯无状态对端专用）
- `HttpTransportMode`：`AutoDetect` / `StreamableHttp` / `Sse`——注意 `Connect()` 始终固定走 Streamable HTTP，`transport_mode` 字段当前**无运行时读取点**（仅声明与测试引用）；SSE 模式由用户直接选用 `SseClientTransport`，"AutoDetect 失败回落 SSE" 未实现
- **平台双实现**：Win32 用 WinHTTP（`#pragma comment(lib, "winhttp.lib")`），POSIX 用自研 `detail::net::HttpClient`；**Request 走 `send_thread_ + send_queue_` 串行队列**，Notification/Response 改为 `LaunchImmediatePost` **独立即时 POST**（短命 detached 线程 `mcp-post`，`shared_from_this` 保活、HTTP 超时兜底线程寿命）——否则 server→client 请求场景（如 elicitation 挂起的 tools/call 等完成通知）互等死锁；Win32 会话 `Start()` 补 `SetConnected()`（与 POSIX 对齐，状态机不再恒为 Initial）
- **IPv6 Host 头**（detail/net/HttpClient.cpp，POSIX 分支）：Host 含 `:` 时自动加方括号 `[v6]` 形式
- **Mcp-Method / Mcp-Name / Mcp-Param-* 头**：解析 body 的 method 字段生成 `Mcp-Method`，`Mcp-Name` 使用 `params.name` 回退 URI——**tasks/get、tasks/update、tasks/cancel 例外取 `params.taskId`**（`IsTaskNameMethod`）；`Mcp-Param-*` 只按 `tools/list` 的 `inputSchema` 中合法 `x-mcp-header` 注解镜像有值参数，未注解参数不发头。服务端可用 `resolve_param_annotations` 校验头与 `tools/call` body 的存在性、编码和值一致性，失败返回 400 `HeaderMismatch (-32020)`；服务端结果 `_meta.x-mcp-header` 会镜像为响应头，详见 [/concepts/mcp-param-headers.md](../concepts/mcp-param-headers.md)
- **`MCP-Protocol-Version` 头自学习**：initialize 请求**不带**该头；从 initialize 响应 `result.protocolVersion` 学习（只认 `initialize` 的 POST 响应——单 JSON 体或 SSE 块，GET 流不参与；`DispatchSseBlock` 仅在 `is_initialize` 时学习），后续请求与 GET 流按协商版本携带，无学习值兜底 `2026-07-28`（`ProtocolVersionHeaderFor`/`NegotiatedVersionFromResponse`，[StreamableHttpClientTransport.cpp:63](../../src/http/StreamableHttpClientTransport.cpp)）；**initialize 请求同样不带 `Mcp-Session-Id` 头**（`SessionIdHeaderFor` 对 initialize 豁免，避免握手期携带过期会话 id）
- **GET SSE 接收流**（`enable_listen_stream` 默认 true）：发送 `notifications/initialized` 后在独立 `mcp-listen` 线程发起 GET 长流（WinHTTP/POSIX 两平台一致），服务端主动推送的消息经 SSE 分块解析、反序列化后并入 MessageChannel，由会话引擎统一分发；单流读超时 600s；**405 视为服务器不支持**（`ListenState::Unsupported`，静默放弃不再重试）；**5xx 视为不可恢复**（`ListenState::GivenUp`，直接放弃不再重连，此前按普通断线退避）；断线退避重连——1s 起 ×1.5 增长封顶 30s、**最多 5 次**（超限 `GivenUp`），经与 SSE 客户端共享语义的 `ReconnectDelayForAttempt` 钳制到 **450–1000ms**（服务端 `retry:` 字段参与：`ParseSseBlock` 解析全数字 `retry:` 为 `retry_ms_`，非法/空值忽略；超大值先按整数比较再构造时长，避免 `milliseconds` 溢出回绕；确定性无抖动），已记录事件 id 时重连携带 `Last-Event-ID` 头（stateless 模式不带），退避睡眠可被 `Close()` 打断（`SleepInterruptibly` 谓词 `running.load()`）；POST 响应的 SSE 块同样解析并存储 `retry:`/`id:`（`DispatchSseBlock`），流内消息超 8MB 丢弃并记 Error 日志（该路径不 `NotifyError`，POST SSE 路径才通知）
- **会话头（stateful 兼容）**：`known_session_id` 非空则从首个非 initialize 的 POST 起携带 `Mcp-Session-Id` 请求头（initialize 豁免，见上）；任意响应（含 4xx）返回 `Mcp-Session-Id` 头时捕获为当前会话 id（存入会话传输内部状态），后续请求携带——stateless 服务端不发该头则全程不带，行为不变
- **Close 顺序**：先停 listen 流（Win32 缩短 `listen_request_` 接收超时、POSIX `listen_client_->Close()` 中断在途 GET，再 join 监听线程，避免 Close 阻塞在读上），再走 POST 通路收尾（置 `delete_pending_` 唤醒发送线程，发送线程退出循环后**仅当已持有会话 id** 时同步发送 `DELETE`（带 `Mcp-Session-Id` 头；Win32 独立 WinHTTP 请求 / POSIX `HttpClient`），随后 join——无会话 id（stateless）不发 DELETE，默认路径无额外请求）
- **响应分流**（两分支一致）：
  - 4xx/5xx：401 必走 challenge 回调、`WWW-Authenticate` 含 `insufficient_scope` 的 403 才走回调（其余 403 直接按错误体处理）；body 可解析为 JSON-RPC error（如 404 + `-32601`）则**入 channel**（连接保持）；**body 不可解析且状态码 404 → 构造 `SessionExpired(-32009)` 错误响应交付 channel**（`MakeSessionExpiredError`，会话过期自愈入口，客户端层据此重协商重放）；仍无法交付才 `NotifyError`
  - **202**：通知确认，**忽略**（body 含 `id` 时记 Warning 视为异常，否则 Info）；不再存在"伪响应"路径
  - `Content-Type` 含 `text/event-stream`：**边读边分块分发**——分块回调内即时按 `\n\n` 切块（`DispatchSseBlock`；空 `data:` 块静默忽略）反序列化入 channel，server→client 请求在流打开期间即可达（原"同步读完整流再处理"会饿死并发请求；Win32 原先独立 SSE 读线程已移除，读由 POST 线程承担，`sse_request_` 仅作 Close 中断句柄）；**响应体累积超过 8MB（`kMaxMessageSize`）→ 丢弃 + `NotifyError`**（Win32/POSIX 一致）
  - 其余：单 JSON 响应解析入 channel（超限 → 错误）
- **Win32 增量读**：`WinHttpReadData` 的"填满缓冲"语义改为 `WinHttpQueryDataAvailable` 先查可用字节数再按量读（DoPost SSE 与 DoListenGet 同修），消除流式响应尾部等待
- 超时：Win32 30s / POSIX 30s
- `Name()` 空时返回 `"streamable-http"`

## 相关页面

- [/modules/http.md](../modules/http.md) — 所属库
- [/classes/http-server.md](../classes/http-server.md) — 底层 HTTP 服务（含 `sse_close_after_write`）
- [/transports/sse.md](sse.md) — SSE 回退模式
- [/concepts/version-negotiation.md](../concepts/version-negotiation.md) — 2026 时代无状态语义
