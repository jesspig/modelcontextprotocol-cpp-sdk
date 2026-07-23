# 架构

## 库分层

```
┌─────────────────────────────────────┐
│  mcp-server        mcp-client       │  服务器和客户端 API
├─────────────────────────────────────┤
│  mcp-protocol                       │  WireCodec、McpSessionHandler
├─────────────────────────────────────┤
│  mcp-transport       mcp-http       │  传输层实现、Streamable HTTP、SSE
├─────────────────────────────────────┤
│  mcp-core                           │  类型、JSON-RPC、接口
└─────────────────────────────────────┘
```

### mcp-core（STATIC）

核心协议类型和 JSON 基础设施：`JsonValue`（基于 `std::variant` 的递归 JSON）、`JsonRpcRequest`、`JsonRpcResponse`、`JsonRpcNotification`、`JsonRpcErrorResponse`、错误码、能力声明（`ClientCapabilities`、`ServerCapabilities`）、内容类型（`TextContent`、`ImageContent`、`EmbeddedResource`、`ResourceLink`）、传输层接口（`ITransport`、`IClientTransport`）。

### mcp-transport（STATIC）

传输层实现：`StdioServerTransport`、`StdioClientTransport`、`SseClientTransport`、`InMemoryTransport`、`WebSocketClientTransport`、`StreamableHttpServerTransport`、`StreamableHttpClientTransport`。平台相关 I/O 位于 `detail/` 目录下（posix、win32）。

### mcp-protocol（STATIC）

JSON-RPC 引擎（`McpSessionHandler`）：基于 `MessageChannel` 的异步消息循环、请求/响应关联与超时、通过 `unordered_map` 进行处理器分发、双时代 `WireCodec`（2025-11-25 initialize 握手 / 2026-07-28 每请求 `_meta`）、入站/出站 `MessageFilter` 管道。

### mcp-server（STATIC）

`McpServer`：工具/资源/提示注册、`Extension` 框架（SEP-2133）、`IMcpTaskStore`（含用于持久化的 `FileTaskStore`）、服务器到客户端的 `Elicit`、MRTR（`InputRequiredResult`）、订阅管理、Streamable HTTP 的无状态模式。

### mcp-client（STATIC）

`McpClient`：服务器发现（`server/discover` → initialize 回退）、版本协商、工具缓存、MRTR 驱动（`MrtrDriver`）、OAuth（PKCE、DCR、令牌刷新/撤销、`FileTokenCache`）。

### mcp-http（STATIC）

用于 Streamable HTTP 模式的 HTTP 服务器：`HttpServer`、`EventStore`（SSE 事件持久化和重放）、`StreamableHttpServerTransport`、`StreamableHttpClientTransport`。

## 依赖图

```
mcp-core (STATIC)
  ├── simdjson  (JSON 解析，内部使用)
  └── libhv     (HTTP/WebSocket，通过 mcp-transport)

mcp-transport (STATIC)
  └── mcp-core + libhv

mcp-protocol (STATIC)
  └── mcp-transport

mcp-http (STATIC)
  └── mcp-transport + libhv

mcp-server (STATIC)
  └── mcp-protocol

mcp-client (STATIC)
  └── mcp-protocol + OpenSSL（可选）
```
