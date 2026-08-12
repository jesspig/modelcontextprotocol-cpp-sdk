---
type: Module
title: mcp-core 核心库
description: 基础静态库：JSON 值模型、JSON-RPC 消息结构、协议数据类型、错误码与方法常量。
tags: [core, json, jsonrpc, 数据类型]
timestamp: 2026-08-13T03:25:00+08:00
resource: src/core/JsonValue.cpp
---

# mcp-core 核心库

依赖链最底层（`mcp-core` → `mcp-transport` → `mcp-protocol` → `mcp-server | mcp-client`），全部 STATIC。修改本库会重编译大量依赖方。

## JSON 值模型

- `JsonValue` 基于 `std::variant<nullptr_t, bool, int64_t, double, string, Array, Object>`；无 uint64/float 类型（[JsonValue.hpp](../../include/mcp/JsonValue.hpp)）
- `Dump()` 手写序列化：NaN/Inf 输出 `null`，double 用 `max_digits10` 精度（[JsonValue.cpp](../../src/core/JsonValue.cpp)）
- 解析用 simdjson DOM；`Parse` 失败抛 `McpError(ParseError)`
- 非 const `operator[]` 缺失键时插入 null；const 版本抛 `DeserializeFailed`
- 详见 [/classes/json-value.md](../classes/json-value.md)

## JSON-RPC 消息

`JsonRpcRequest / JsonRpcNotification / JsonRpcResponse / JsonRpcErrorResponse` 四种结构 + `JsonRpcMessage` 变体（[JsonRpc.hpp](../../include/mcp/JsonRpc.hpp)）。错误响应 `id` 是 `optional<RequestId>`（JSON-RPC 2.0 §5.1）。反序列化按字段存在性分派：method+id→Request、仅 method→Notification、result→Response、error→ErrorResponse，result+error 同现或全无→`InvalidRequest`。

## 协议数据类型（McpTypes）

| 分组 | 数量 | 代表类型 |
|------|------|----------|
| 基础类型 | 13 | Tool、Resource、ResourceTemplate、Prompt、Pagination、Result 等 11 struct + ToolExecutionMode、ResultType 2 枚举 |
| Params | 23 | 16 struct（Paginated/Resource/CallTool/GetPrompt/Complete/Discover/Initialize/SubscriptionsListen/SubscriptionsAcknowledged/Elicit/CreateMessage/ListRoots/SetLevel + tasks 3）+ 7 alias（ListTools/ListResources/ListResourceTemplates/ListPrompts/ReadResource/Subscribe/Unsubscribe） |
| Results | 19 | 16 struct（EmptyResult、CallToolResult、DiscoverResult、ElicitResultTyped\<T\> 模板、MRTR 三件套等）+ 3 alias（Ping/UpdateTask/CancelTask） |
| Notifications | 4 | SubscriptionFilter、Progress/Cancelled/LoggingMessage 参数 |
| Options | 5 | RequestOptions、CacheableRequestOptions、ToolOptions、ResourceOptions、PromptOptions |

所有类型都有成对 `SerializeXxx/DeserializeXxx` 自由函数（约 70 对）。Result 序列化统一带 `resultType` 键。

## 常量集

- **错误码 18 个**（[ErrorCodes.hpp](../../include/mcp/ErrorCodes.hpp)）：标准 JSON-RPC 5 个（-32700~-32603）+ MCP 专用 7 个（HeaderMismatch、MissingRequiredClientCapability、UnsupportedProtocolVersion、UrlElicitationRequired、ConnectionClosed、RequestTimeout、RequestCancelled）+ 细粒度子类 6 个（DeserializeFailed、ConnectionRefused、TlsHandshakeFailed、ProtocolViolation、TaskNotFound、HandlerError）；`default_error_condition` 将传输类错误映射到 `errc::connection_aborted` 等
- **方法常量 42 个**（[Methods.hpp](../../include/mcp/Methods.hpp)）：`methods` 命名空间 25 个（含前缀常量 `ext/`），`notifications` 命名空间 17 个
- **协议版本**（[ProtocolVersion.hpp](../../include/mcp/ProtocolVersion.hpp)）：`kLatestProtocolVersion = "2026-07-28"`、`kLegacyProtocolVersion = "2025-11-25"`，支持版本数组共 5 个；`IsModernProtocolVersion` = 字典序 `>= kLatest`
- **日志 6 级**（[Log.hpp](../../include/mcp/Log.hpp)）：Off/Error/Warning/Info/Debug/Trace，级别经 `MCP_LOG_LEVEL` 环境变量，输出到 stderr

## 内部头（src/detail）

- `JsonFields.hpp`：全部 JSON 字段名常量（约 70 个 `kXxx[]`），协议键禁止硬编码
- `JsonSerializer.hpp`：`DeserializeOptional` 未特化时 `static_assert` 编译失败（非静默）
- `JsonSchemaValidator.hpp`：最小 JSON Schema 子集校验器（SEP-2106，draft-07 风格）
- `ResponseCache.hpp`：客户端响应缓存（SEP-2549），惰性过期清除

## 相关页面

- [/classes/json-value.md](../classes/json-value.md) — JSON 值实现
- [/concepts/version-negotiation.md](../concepts/version-negotiation.md) — 版本常量如何被使用
- [/modules/protocol.md](protocol.md) — 上层依赖方
- [/build.md](../build.md) — 构建与编译选项
