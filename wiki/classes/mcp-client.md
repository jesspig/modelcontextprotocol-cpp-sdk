---
type: Class
title: McpClient
description: MCP 客户端门面：创建即协商、请求/通知 API、任务化工具调用、404 会话自愈、progress 回调、MRTR、响应缓存与翻页聚合。
tags: [client, 门面, 协商, mrt, progress, tasks]
timestamp: 2026-09-15T15:49:10+08:00
resource: include/mcp/client/McpClient.hpp
---

# McpClient

客户端门面（[McpClient.hpp](../../include/mcp/client/McpClient.hpp)）。**创建即阻塞**：`Create(transport, options)` 构造后立即同步 `NegotiateProtocol()`，返回前协商完成（[McpClient.cpp](../../src/client/McpClient.cpp)）。

## 协商（详见 [/concepts/version-negotiation.md](../concepts/version-negotiation.md)）

| 模式 | 行为 |
|------|------|
| Auto（默认） | 探测 `server/discover`；失败按传输类型分派（见下） |
| Legacy | 强制 initialize 握手 + `notifications/initialized` |
| Pin | 不发送任何探测，版本取 `pin_protocol_version` |

Auto 回退分派（对齐官方 TS SDK，[McpClient.cpp:296](../../src/client/McpClient.cpp)）：

- **stdio 类传输**（RTTI：typeid name 含 `InMemoryTransportImpl`/`StdioClientSessionTransport`）超时/网络失败 → 回退 initialize
- **HTTP 类传输**超时 → `McpError(RequestTimeout)`；网络异常 → `McpError(ConnectionClosed)`
- **-32022 三分支**：`data.supported` 含 `2026-07-28` → corrective 重发一次、再失败抛 `UnsupportedProtocolVersion`；无任何现代版本 → 回退 initialize；含现代版本但不含 2026-07-28 → 抛 `UnsupportedProtocolVersion`（supported 缺失/格式不符同样回退 initialize）
- **-32001/-32020/-32021/-32601 及其他错误码** → 回退 initialize

四个分支协商完成后都调用 `SetNegotiatedProtocolVersion`。

## 行为要点

- `WireClientHandlers()` 注册 6 个通知处理器（[McpClient.cpp:522](../../src/client/McpClient.cpp)）：三个 listChanged → `response_cache_->Clear()`；`resources/updated` → 按 uri 键**单键失效**（`Invalidate`，其余缓存保留）；`notifications/progress` → 重置对应请求超时 + 分发 `on_progress` 回调；`subscriptions/acknowledged` → 匹配 `SubscribeAsync` 待确认订阅并转发用户处理器（经 `SetNotificationHandler` 特判存储的 `user_ack_notification_handler_`，不覆盖内部逻辑）；另注册 elicit 请求处理器
- 懒注册：`SetSamplingHandler`/`SetRootsHandler` 未设置 → `MethodNotFound`；`SetLoggingHandler` 未设置 → 静默丢弃
- 响应缓存（SEP-2549）：键 = `CacheKey(method, context)`——列表方法带 cursor 键（`<method>\x1F<cursor>`，无 cursor 为空串），`resources/read` 带 uri 键；`ttlMs > 0` 才缓存，TTL **钳制 24h**（`kMaxTtl`）；按 `cacheScope` 分 **public/private 双分区**（private 连接关闭时 `ClearPrivate` 丢弃，public 保留），读取 `GetAny` 双分区查（public 优先）；`resources/updated` 只失效对应 uri 键；`ExtractCacheHint` 顶层 `ttlMs`/`cacheScope` 优先回退嵌套 `cacheHint`，而 `CacheIfHinted` 顺序**相反**（嵌套优先、顶层兜底，[McpClient.cpp:964](../../src/client/McpClient.cpp)）；`ReadResource` 支持 `cache_mode`（`use`/`bypass`/`refresh`）与 `max_age_ms`
- MRTR：`SendRequestWithMrtr` 包装 `SendRequestWithMrtrOnce`（后者循环处理 `input_required`）——**仅当 `ClientOptions::input_required_config` 显式配置**（`auto_fulfill` 默认开）时启用，未配置 `max_rounds=0` 仅 1 轮；配置时 `max_rounds`（默认 10）超限抛 InternalError、`max_total_timeout` 超限抛 RequestTimeout；`input_requests` 三类型（elicit/confirm → elicitation、sampling、roots）分派对应 handler，elicit/confirm 的 handler 结果取 `ElicitResult::content`（成员已由 `values` 改名对齐 wire 键）填入 responses；无请求项时 state-only 退避（50ms ×2、封顶 250ms，见 [/concepts/mrtr.md](../concepts/mrtr.md)）
- 自动翻页：`ListPages` 上限 `kMaxListPages = 64` 页，不收敛抛 `McpError(ProtocolViolation)`（修复原静默截断）；聚合入口 `ListToolsAll/ListResourcesAll/ListResourceTemplatesAll/ListPromptsAll` 自带 cursor 循环（同样 64 页上限，返回时 `next_cursor` 为空）
- 任务客户端流：`CallToolAsTask(name, arguments?, options?)` 发起任务化 tools/call——对端返回 `resultType=="task"` 时立即得到 `GetTaskResult`（任务句柄），后续 `GetTask`/`PollTaskToCompletion`（500ms 间隔 / 300s 超时，超时抛 `InternalError`）轮询至终态，`CancelTask` 请求取消
- 404 会话自愈：`SessionExpired(-32009)` 且 `reinit_on_expired_session`（默认 true）时，`SendRequestWithMrtr` 捕获后调 `RecoverExpiredSession()`——`session_generation_` 原子世代计数 + `reinit_mutex_` 串行化重协商，持锁前先读世代、并发等待者见世代已变则跳过；随后原请求**恰一次重放**（`SendRequestWithMrtrOnce` 不再捕获，避免二次重放）
- 总量超时：构造时 `SetMaxTotalTimeout(options_.max_total_timeout)` 接线至会话引擎（默认 0 禁用），progress 续命不可越过每请求绝对截止（见 [/classes/mcp-session-handler.md](mcp-session-handler.md)）；MRTR 循环自身的总预算仍取 `input_required_config->max_total_timeout`
- URL elicitation：`SetUrlElicitationHandler` 注册 url 模式处理器——收到 `mode=="url"` 的 `elicitation/create` 时调用（缺 `elicitationId` 直接回 `InvalidParams`），返回后 SDK 自动回 `action="accept"` 并发送 `notifications/elicitation/complete`；处理器抛异常则异常回传服务端
- 超时：任务类请求 600s、Ping 10s（Ping 已标记 deprecated）；`SubscribeAsync` 发送后等待 `subscriptions/acknowledged` 首帧（**5s**，`kSubscriptionAckTimeout`），超时抛 `McpError(InternalError)`；请求携带 `_meta` `subscriptionId`（调用方提供或自动生成 `client-sub-<时钟>-<计数>`）
- `ClientOptions` 默认：`client_info {"mcp-cpp-client","0.3.4"}`、`initialization_timeout 60s`、`discover_probe_timeout 5s`、`max_total_timeout 0`（禁用）、`reinit_on_expired_session true`

## progress 接收

`RequestOptions::on_progress`（`std::function<void(const ProgressNotificationParams&)>`）启用 progress 上报，覆盖 `CallTool`/`GetPrompt`：

- **token 生成**：`options.meta.progressToken`（string/int）显式提供时优先透传，否则 `next_progress_token_` 原子计数自 1 起自动生成（`AttachProgressCallback`，[McpClient.cpp:751](../../src/client/McpClient.cpp)）
- **注册/清理**：回调存入 `progress_callbacks_`（token 键控），请求结束经 RAII `ScopedProgressCleanup` 确保异常路径也清理
- **分发**：`notifications/progress` 处理器按 token 反查回调，先 `ResetTimeoutByProgressToken` 顺延请求 deadline 再触发；**回调在会话消息循环线程同步执行，必须快速返回**（handler 异常吞掉记 Error 日志）
- **双 era 落点**：legacy era progressToken 写 `params._meta.progressToken`（[McpSessionHandler.cpp:514](../../src/protocol/McpSessionHandler.cpp)）；modern era 走 `_meta` 信封、序列化层落 `params._meta`

## 客户端 → 服务端通知

`SendRootsListChanged()` 发送 `notifications/roots/list_changed`（[McpClient.cpp:1453](../../src/client/McpClient.cpp)）。协商版本须 **>= 2025-06-18**（在 `kProtocolVersions` 中比较序位），否则抛 `McpError(ProtocolViolation)`。

## 相关页面

- [/modules/client.md](../modules/client.md) — 所属库
- [/concepts/version-negotiation.md](../concepts/version-negotiation.md)
- [/concepts/mrtr.md](../concepts/mrtr.md) — MRTR 循环
- [/classes/mcp-session-handler.md](mcp-session-handler.md) — 底层引擎
