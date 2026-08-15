---
type: Class
title: McpSessionHandler
description: JSON-RPC 引擎：消息分发、请求/响应关联、超时检查、取消、过滤器管线。
tags: [protocol, jsonrpc, 超时, 并发]
timestamp: 2026-08-15T02:40:07+08:00
resource: include/mcp/protocol/McpSessionHandler.hpp
---

# McpSessionHandler

内部 JSON-RPC 引擎（[McpSessionHandler.hpp](../../include/mcp/protocol/McpSessionHandler.hpp)）。构造参数：`shared_ptr<ITransport>`、`unique_ptr<WireCodec>`、可选入站/出站 `FilterPipeline`。本层无 Initialize→Ready 状态机——只有 `running_` 与 `closed_` 两个标志；"已初始化"守卫在 McpServer 层。

## 生命周期

- `Start()` CAS 防重入；`closed_` 已置时调用抛 `std::logic_error`（[McpSessionHandler.cpp:75](../../src/protocol/McpSessionHandler.cpp)）。启动 3 个线程：消息循环、超时检查（每 100ms `CheckTimeouts()`）、响应 worker
- `Close()`：置标志 → 关通道唤醒循环 → `JoinThreadSafely`（self-join 时 detach）→ notify + join 响应 worker（任务内 `closed_` 中止，join 不阻塞）→ 锁内收集并清空 pending 表（含 progress_token_map_）→ 锁外以 `ConnectionClosed` 回调所有 pending → 关 transport
- 请求 ID：原子计数器从 1 起

## 超时机制

- 默认 `kDefaultRequestTimeout = 60000ms`（[McpSession.hpp](../../include/mcp/protocol/McpSession.hpp)），`SendRequest` 可覆盖
- 超时回调 `ErrorData{RequestTimeout, "request timed out"}`，回调在锁外执行
- `ResetTimeoutByProgressToken`：经 `progress_token_map_ → request_id` 定位 pending，仅当剩余时间 < 30s 时把 deadline 顺延 30s

## 请求处理语义

- handler 抛 `McpError` → 回 `e.Code()`（错误码保留）；其他异常 → `InternalError "handler error: ..."`
- 校验失败：方法不在时代（非 initialize）→ `MethodNotFound`（-32601）；`Invalid` → `InvalidRequest`；未注册 handler → `MethodNotFound`
- 入站校验用轻量视图：只构造 `{method, _meta 占位, initialize 的 params}` 三个键交给 codec，不完整序列化请求（[McpSessionHandler.cpp:222](../../src/protocol/McpSessionHandler.cpp)）
- 响应回发：`EnqueueResponse` 入队（锁内检查 `closed_`，已关闭直接丢弃），单一 `response_worker_` 线程经 `response_queue_`（`deque<std::function>`）+ `response_cv_` 消费；任务先 `wait_for(0)` 快检（同步 handler 零延迟），未就绪则 10ms 间隔轮询 promise（`kResponsePollInterval`），`closed_` 时中止（保证 Close 不阻塞）。`SendResponseAsync`/`ReapCompletedResponses` 已删除
- `SendRequest` 注册 pending 后复查 `closed_`：已关闭则以 `ConnectionClosed` 错误满足 promise（[McpSessionHandler.cpp:505](../../src/protocol/McpSessionHandler.cpp)）
- `RequiredClientCapability`：`createMessage→sampling`、`listRoots→roots`；缺失回 `MissingRequiredClientCapability`（data 含 `requiredCapabilities`）

## 其他

- `negotiated_version_` 为 `shared_ptr<const std::string>`；`NegotiatedProtocolVersion()` 读锁下仅拷贝 shared_ptr、锁外解引用；`SetNegotiatedProtocolVersion` 在 `codec_mutex_`（`shared_mutex`，读并发写独占）下写（替换 codec + 版本），消息循环运行中可调用
- `ExtractIncomingMeta(req)` 为本类成员（[McpSessionHandler.cpp:564](../../src/protocol/McpSessionHandler.cpp)）：解析 `req.meta` 全部 RequestMeta 字段 + `subscriptionId`；解析失败记 Warning 并返回空 meta
- `SetRequestStateVerifier`（HMAC/AEAD）须在 `Start()` 前调用
- 订阅：`AddSubscription/RemoveSubscription/NotifySubscribers`，按 `SubscriptionFilter` 过滤，通知带 `subscriptionId` meta——事件通知的 `subscriptionId` **优先回显条目 `session_id`**（订阅时 `_meta` 携带的客户端 ID，与 ack 帧一致），未设置时回退服务端自增 `id`（[McpSessionHandler.cpp:655](../../src/protocol/McpSessionHandler.cpp)）
- 事件回调全部经 `InvokeSafely` 包异常（记 Error 日志）
- 过滤器挂接：入站在消息循环分发前，出站在 `SendMessage` 中（`closed_` 时不再发送）

## 相关页面

- [/modules/protocol.md](../modules/protocol.md) — 所属库
- [/classes/wire-codec.md](wire-codec.md) — 编解码协作
- [/classes/message-channel.md](message-channel.md) — 消息载体
- [/concepts/concurrency.md](../concepts/concurrency.md) — 线程与 self-join
- [/concepts/meta-and-filters.md](../concepts/meta-and-filters.md) — 过滤器管线
