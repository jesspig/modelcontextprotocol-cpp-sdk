---
type: Concept
title: _meta 元数据与过滤器管线
description: 2026 时代每请求 _meta 携带协议信息，入站/出站 FilterPipeline 用于认证审计限流。
tags: [协议, meta, filter, 认证]
timestamp: 2026-08-15T22:30:00+08:00
resource: include/mcp/Meta.hpp
---

# _meta 元数据与过滤器管线

## _meta（2026 时代）

- `RequestMeta` 9 字段（[Meta.hpp](../../include/mcp/Meta.hpp)）：`progress_token`、`protocol_version`（默认 `kLatestProtocolVersion`）、`client_info`、`client_capabilities`、`log_level`、`extensions`、`traceparent`、`tracestate`、`baggage`
- 序列化到 `_meta` 的协议键为 `io.modelcontextprotocol/...` 命名空间（常量在 `src/detail/JsonFields.hpp` 的 `kMeta*Key` 系列）；未知键保留进 extensions bag 保证往返对称
- `IncomingRequestMeta`（[/classes/mcp-session-handler.md](../classes/mcp-session-handler.md)）额外读 `_meta.subscriptionId`（非字符串记 Warning 忽略）
- `CacheHint{ttl_ms?, cache_scope?}` 序列化键 `ttlMs`/`cacheScope`
- **2026 时代缓存字段平铺**：`WireCodec::EncodeResult` 将结果中嵌套的 `cacheHint` 对象转为**顶层** `ttlMs`/`cacheScope` 后删除 `cacheHint`（[WireCodec.cpp](../../src/protocol/WireCodec.cpp:211)）；2025 时代保留嵌套原样
- 每请求日志级别：`RequestContext` 从 meta 的 `io.modelcontextprotocol/logLevel` 读取

## 过滤器管线

- `FilterPipeline`（[MessageFilter.hpp](../../include/mcp/protocol/MessageFilter.hpp)）：`AddFilter(shared_ptr<MessageFilter>)` + `Execute(message, final_handler)`，无 filter 时直通；链式递归
- **约束：`next` 必须在 filter 函数返回前同步调用**，延迟调用不安全
- 挂接点：`ServerOptions::incoming_filters / outgoing_filters`（[/classes/mcp-server.md](../classes/mcp-server.md)）；`McpSessionHandler` 构造参数。入站在消息循环分发前执行，出站在 `SendMessage` 中执行（`closed_` 时不再发送）
- 用途：认证/审计/限流（ServerOptions/MessageFilter 头注释推荐做法）

## 相关页面

- [/modules/core.md](../modules/core.md) — RequestMeta 序列化
- [/classes/mcp-session-handler.md](../classes/mcp-session-handler.md) — 过滤挂接点
- [/classes/mcp-server.md](../classes/mcp-server.md) — ServerOptions 配置
