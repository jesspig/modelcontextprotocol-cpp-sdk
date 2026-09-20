---
type: Concept
title: _meta 元数据与过滤器管线
description: 2026 时代每请求 _meta 携带协议信息（线上位于 params._meta），入站/出站 FilterPipeline 用于认证审计限流。
tags: [协议, meta, filter, 认证]
timestamp: 2026-09-20T21:46:56+08:00
resource: include/mcp/Meta.hpp
---

# _meta 元数据与过滤器管线

## _meta（2026 时代）

- **线上位置**：请求/通知的 `_meta` 位于 **`params._meta`**（对齐官方规范与 TS 服务器），不是 JSON 顶层
- 内存结构（`JsonRpcRequest.meta` / `JsonRpcNotification.meta`）语义不变：序列化层（[JsonRpc.cpp](../../src/core/JsonRpc.cpp)）的 `MergeMetaIntoParams` 把 `meta` 合并进 `params._meta`，反序列化的 `ExtractMetaFromParams` 取出后**从 params 移除 `_meta` 键**——业务参数不受污染
- `RequestMeta` 9 字段（[Meta.hpp](../../include/mcp/Meta.hpp)）：`progress_token`、`protocol_version`（默认 `kLatestProtocolVersion`）、`client_info`、`client_capabilities`、`log_level`、`extensions`、`traceparent`、`tracestate`、`baggage`
- 序列化到 `_meta` 的协议键为 `io.modelcontextprotocol/...` 命名空间（常量在 `src/detail/JsonFields.hpp` 的 `kMeta*Key` 系列）；未知键保留进 extensions bag 保证往返对称
- `IncomingRequestMeta`（[/classes/mcp-session-handler.md](../classes/mcp-session-handler.md)）额外读 `_meta.subscriptionId`（非字符串记 Warning 忽略）
- `CacheHint{ttl_ms?, cache_scope?}` 序列化键 `ttlMs`/`cacheScope`
- **2026 时代缓存字段平铺**：`WireCodec::EncodeResult` 将结果中嵌套的 `cacheHint` 对象转为**顶层** `ttlMs`/`cacheScope` 后删除 `cacheHint`（[WireCodec.cpp:218](../../src/protocol/WireCodec.cpp)）；2025 时代保留嵌套原样
- 每请求日志级别：`RequestContext` 从 meta 的 `io.modelcontextprotocol/logLevel` 读取
- **入站校验与出站 stamp 均按 params 路径**：Rev2026 `ValidateRequest` 要求除 discover 外含 `params._meta`，`StampOutgoingRequest` 写 `params._meta`（[/classes/wire-codec.md](../classes/wire-codec.md)）
- **服务端字段类型校验**（`McpSessionHandler` 派发前）：按 `InvalidMetaField` 检查本 SDK 读取的子字段——`protocolVersion` 字符串、`progressToken` 字符串或整数、`clientInfo`/`clientCapabilities` 对象、`logLevel`/`traceparent`/`tracestate`/`baggage` 字符串；不符回 `InvalidParams`，消息 `invalid _meta field: <键名>`；未识别键放行
- **版本受支持性**：非 `initialize` 请求的 `_meta.protocolVersion` 不在 `kProtocolVersions` 内回 `UnsupportedProtocolVersion`（-32022，带 `requested`/`supported`），详见 [/concepts/version-negotiation.md](version-negotiation.md)
- **双 era progressToken**：legacy era 写 `params._meta.progressToken`；modern era 经 `req.meta` 信封由序列化层落到同一线上位置

## 过滤器管线

- `FilterPipeline`（[MessageFilter.hpp](../../include/mcp/protocol/MessageFilter.hpp)）：`AddFilter(shared_ptr<MessageFilter>)` + `Execute(message, final_handler)`，无 filter 时直通；链式递归
- **约束：`next` 必须在 filter 函数返回前同步调用**，延迟调用不安全
- 挂接点：`ServerOptions::incoming_filters / outgoing_filters`（[/classes/mcp-server.md](../classes/mcp-server.md)）；`McpSessionHandler` 构造参数。入站在消息循环分发前执行，出站在 `SendMessage` 中执行（`closed_` 时不再发送）
- 用途：认证/审计/限流（挂接点见 `ServerOptions::incoming_filters` / `outgoing_filters`）

## 相关页面

- [/modules/core.md](../modules/core.md) — RequestMeta 序列化
- [/classes/mcp-session-handler.md](../classes/mcp-session-handler.md) — 过滤挂接点
- [/classes/mcp-server.md](../classes/mcp-server.md) — ServerOptions 配置
