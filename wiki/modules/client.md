---
type: Module
title: mcp-client 客户端库
description: McpClient 门面：连接模式协商、请求/响应、progress 回调、任务化工具调用、MRTR elicit 回填、404 会话自愈、OAuth 与令牌缓存。
tags: [client, oauth, 缓存, 协商, progress, tasks]
timestamp: 2026-09-16T19:10:10Z
resource: src/client/McpClient.cpp
---

# mcp-client 客户端库

依赖 `mcp-protocol`。开启 Unity 但设 `UNITY_BUILD_UNIQUE_ID ON`（OAuth 匿名符号需要）。

## 组成

| 组件 | 职责 | 页 |
|------|------|----|
| McpClient | 客户端门面：创建、协商、请求、MRTR、自动翻页 | [/classes/mcp-client.md](../classes/mcp-client.md) |
| VersionNegotiation | Auto/Legacy/Pin 三种协商策略 | [/concepts/version-negotiation.md](../concepts/version-negotiation.md) |
| OAuthClientProvider | OAuth 2.0 授权码流 + PKCE + RFC 9207 iss 校验 | [/concepts/oauth.md](../concepts/oauth.md) |
| FileTokenCache / ITokenCache | 令牌持久化（Windows DPAPI） | [/classes/file-token-cache.md](../classes/file-token-cache.md) |

## 客户端行为要点

- **创建即阻塞**：`McpClient::Create` 构造后立即同步 `NegotiateProtocol()`，返回前协商完成
- `WireClientHandlers()` 注册 6 个通知处理器：三个 listChanged（清空响应缓存）、`resources/updated`（按 uri 单键失效）、`notifications/progress`（重置对应请求超时 + 分发 `on_progress` 回调）、`subscriptions/acknowledged`（匹配 `SubscribeAsync` 待确认订阅并转发用户处理器）；另接线 `elicitation/create` 请求处理器（form 走 `elicitation_handler`，url 模式走 `url_elicitation_handler_` 并自动回 `notifications/elicitation/complete`）
- 懒注册：`SetSamplingHandler`/`SetRootsHandler` 未设置时收到请求抛 `MethodNotFound`（两 API 已因 SEP-2577 废弃，新代码改用 `SetElicitationHandler`）；`SetLoggingHandler` 未设置时静默丢弃
- 自动翻页：无 cursor 的列表请求自动翻页，上限 `kMaxListPages = 64` 页，**不收敛抛 `McpError(ProtocolViolation)`**（修复原静默截断缺陷）
- 聚合 API：`ListToolsAll/ListResourcesAll/ListResourceTemplatesAll/ListPromptsAll` 自带 cursor 循环聚合成单结果（同样 64 页上限防不收敛，超限抛 `ProtocolViolation`，返回时 `next_cursor` 为空）
- 任务客户端流：`CallToolAsTask` 发起任务化 tools/call（对端返回 `resultType=="task"` 句柄时立即返回），与 `GetTask`/`PollTaskToCompletion`（500ms 间隔 / 300s 超时）/`CancelTask` 串成完整流
- MRTR elicit/confirm 回填写入 `ElicitResult::content`（成员已由 `values` 改名对齐 wire 键，见 [/concepts/mrtr.md](../concepts/mrtr.md)）
- 404 会话自愈：HTTP 传输把 404 类型化为 `SessionExpired(-32009)` 错误；`ClientOptions::reinit_on_expired_session`（默认 true）时 `SendRequestWithMrtr` 捕获后 `RecoverExpiredSession()` 重协商并以原请求**恰一次重放**（`session_generation_` 原子世代计数 + `reinit_mutex_` 串行化，并发等待者见世代已变则跳过重初始化）
- 总量超时封顶：`ClientOptions::max_total_timeout`（默认 0 禁用）构造时经 `SetMaxTotalTimeout` 接线至会话引擎——每请求记录绝对截止，progress 续命只顺延 idle deadline 不可越过总量（见 [/classes/mcp-session-handler.md](../classes/mcp-session-handler.md)）
- 超时：任务类请求 `kTaskRequestTimeout = 600s`、Ping `kPingTimeout = 10s`

### Auto 协商回退（对齐官方 TS SDK，[McpClient.cpp:296](../../src/client/McpClient.cpp)）

- **stdio 类传输**（RTTI 判定 typeid name 含 `InMemoryTransportImpl`/`StdioClientSessionTransport`）：discover 探测超时/网络失败→回退 initialize
- **HTTP 类传输**：超时→抛 `McpError(RequestTimeout)`；网络异常→`McpError(ConnectionClosed)`，不回退
- **-32022 三分支**：`data.supported` 含 `kLatestProtocolVersion`（2026-07-28）→ corrective 重发一次，再失败抛 `UnsupportedProtocolVersion`；无任何现代版本→回退 initialize；含现代版本但不含 2026-07-28→抛 `UnsupportedProtocolVersion`（supported 缺失/格式不符同样回退 initialize）
- **-32001/-32020/-32021/-32601 及其他错误码**→回退 initialize
- **discover 成功路径版本交集**：`DeclaresSharedClientVersion` 判定响应 `supportedVersions` 与客户端支持表是否相交（字段缺失视为未声明、接受探测版本）；`SelectSharedVersion` 从交集中取客户端支持的最新版本（按 `kProtocolVersions` 从新到旧）；空/无交集回退 legacy（见 [/concepts/version-negotiation.md](../concepts/version-negotiation.md)）

### 缓存读取兼容

`ExtractCacheHint` 顶层 `ttlMs/cacheScope` 优先、回退嵌套 `cacheHint`（兼容 2026 扁平化与 2025 嵌套两形态）；`CacheIfHinted` 顺序**相反**——嵌套 `cacheHint` 优先、顶层兜底（[McpClient.cpp:964](../../src/client/McpClient.cpp)）。`ResponseCache` 键 = `CacheKey(method, context)`（列表带 cursor、read 带 uri），TTL 钳制 24h，按 cacheScope 分 public/private 双分区（`GetAny` 双查、`Close` 清 private），`resources/updated` 按 uri 单键失效（[ResponseCache.hpp](../../src/detail/ResponseCache.hpp)）。`DoSendRequest/ListPages` 的键均用 `detail` 常量。

### progress 接收（客户端方向）

- `RequestOptions::on_progress`（`std::function<void(const ProgressNotificationParams&)>`）设置后，`CallTool`/`GetPrompt` 经 `AttachProgressCallback` 注册回调并生成 progressToken——`options.meta.progressToken`（string/int）显式提供时优先，否则自动生成自 1 起的原子计数；请求结束经 RAII（`ScopedProgressCleanup`）清理回调
- `notifications/progress` 处理器先 `ResetTimeoutByProgressToken` 顺延该请求 deadline，再按 token 分发回调；**回调在会话消息循环线程同步执行，必须快速返回**
- 双 era 落点：legacy era progressToken 写 `params._meta.progressToken`；modern era 经 `_meta` 信封（序列化层落 `params._meta`）

### 客户端 → 服务端通知

`SendRootsListChanged()` 发送 `notifications/roots/list_changed`，协商版本 **>= 2025-06-18** 才允许，否则抛 `McpError(ProtocolViolation)`（[McpClient.cpp:1453](../../src/client/McpClient.cpp)）。

## 相关页面

- [/classes/mcp-client.md](../classes/mcp-client.md)
- [/concepts/version-negotiation.md](../concepts/version-negotiation.md)
- [/concepts/oauth.md](../concepts/oauth.md)
- [/classes/file-token-cache.md](../classes/file-token-cache.md)
