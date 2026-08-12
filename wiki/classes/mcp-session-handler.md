---
type: Class
title: McpSessionHandler
description: JSON-RPC 引擎：消息分发、请求/响应关联、超时检查、取消、过滤器管线。
tags: [protocol, jsonrpc, 超时, 并发]
timestamp: 2026-08-13T03:25:00+08:00
resource: include/mcp/protocol/McpSessionHandler.hpp
---

# McpSessionHandler

内部 JSON-RPC 引擎（[McpSessionHandler.hpp](../../include/mcp/protocol/McpSessionHandler.hpp)）。构造参数：`shared_ptr<ITransport>`、`unique_ptr<WireCodec>`、可选入站/出站 `FilterPipeline`。本层无 Initialize→Ready 状态机——只有 `running_` 与 `closed_` 两个标志；"已初始化"守卫在 McpServer 层。

## 生命周期

- `Start()` CAS 防重入，启动消息循环线程 + 超时检查线程（每 100ms `CheckTimeouts()`）
- `Close()`：置标志 → 关通道唤醒循环 → `JoinThreadSafely`（self-join 时 detach）→ 所有 pending 以 `ConnectionClosed` 回调（锁外）→ 清空各表 → 关 transport
- 请求 ID：原子计数器从 1 起

## 超时机制

- 默认 `kDefaultRequestTimeout = 30000ms`（[McpSession.hpp](../../include/mcp/protocol/McpSession.hpp)），`SendRequest` 可覆盖
- 超时回调 `ErrorData{RequestTimeout, "request timed out"}`，回调在锁外执行
- `ResetTimeoutByProgressToken`：经 `progress_token_map_ → request_id` 定位 pending，仅当剩余时间 < 30s 时把 deadline 顺延 30s

## 请求处理语义

- handler 抛 `McpError` → 回 `e.Code()`（错误码保留）；其他异常 → `InternalError "handler error: ..."`
- 校验失败：方法不在时代（非 initialize）→ `MethodNotFound`；`Invalid` → `InvalidRequest`；未注册 handler → `MethodNotFound`
- `SendResponseAsync` 用 `std::async` + 50ms 轮询 promise，`closed_` 时放弃等待（保证 Close 不阻塞）
- `RequiredClientCapability`：`createMessage→sampling`、`listRoots→roots`；缺失回 `MissingRequiredClientCapability`（data 含 `requiredCapabilities`）

## 其他

- `SetNegotiatedProtocolVersion` 线程安全（`codec_mutex_` 下重建 codec），消息循环运行中可调用
- `SetRequestStateVerifier`（HMAC/AEAD）须在 `Start()` 前调用
- 订阅：`AddSubscription/RemoveSubscription/NotifySubscribers`，按 `SubscriptionFilter` 过滤，通知带 `subscriptionId` meta
- 事件回调全部经 `InvokeSafely` 包异常（记 Error 日志）
- 过滤器挂接：入站在消息循环分发前，出站在 `SendMessage` 中（`closed_` 时不再发送）

## 相关页面

- [/modules/protocol.md](../modules/protocol.md) — 所属库
- [/classes/wire-codec.md](wire-codec.md) — 编解码协作
- [/classes/message-channel.md](message-channel.md) — 消息载体
- [/concepts/concurrency.md](../concepts/concurrency.md) — 线程与 self-join
- [/concepts/meta-and-filters.md](../concepts/meta-and-filters.md) — 过滤器管线
