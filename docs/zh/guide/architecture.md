# 架构

## 库分层

```
┌─────────────────────────────────────┐
│  mcp-server        mcp-client       │  服务器和客户端 API
├─────────────────────────────────────┤
│  mcp-protocol                       │  WireCodec、McpSessionHandler
├─────────────────────────────────────┤
│  mcp-transport                      │  Stdio、SSE、WebSocket、InMemory
├─────────────────────────────────────┤
│  mcp-core            mcp-http       │  类型、JSON-RPC、HTTP 服务
└─────────────────────────────────────┘
```

### mcp-core（仅头文件的 INTERFACE）

所有协议类型：`Tool`、`Resource`、`Prompt`、`ElicitResult`、`CallToolResult`、JSON-RPC 消息结构（`JsonRpcRequest`、`JsonRpcResponse`、`JsonRpcNotification`、`JsonRpcErrorResponse`）、错误码、能力声明、传输层接口（`ITransport`、`IClientTransport`）。

### mcp-transport（STATIC）

传输层实现：`StdioServerTransport`、`StdioClientTransport`、`SseClientTransport`、`InMemoryTransport`、`WebSocketClientTransport`。平台相关 I/O 位于 `detail/` 目录下（posix、win32）。

### mcp-protocol（STATIC）

JSON-RPC 引擎（`McpSessionHandler`）：基于 `MessageChannel` 的异步消息循环、请求/响应关联与超时、通过 `unordered_map` 进行处理器分发、双时代 `WireCodec`（2025-11-25 initialize 握手 / 2026-07-28 每请求 `_meta`）、入站/出站 `MessageFilter` 管道。

### mcp-server（STATIC）

`McpServer`：工具/资源/提示注册、`Extension` 框架（SEP-2133）、`IMcpTaskStore`（含用于持久化的 `FileTaskStore`）、服务器到客户端的 `Elicit`、MRTR（`InputRequiredResult`）、订阅管理、Streamable HTTP 的无状态模式。

### mcp-client（STATIC）

`McpClient`：服务器发现（`server/discover` → initialize 回退）、版本协商、工具缓存、MRTR 驱动（`MrtrDriver`）、OAuth（PKCE、DCR、令牌刷新/撤销、`FileTokenCache`）。

### mcp-http（STATIC）

用于 Streamable HTTP 模式的 HTTP 服务器：`HttpServer`、`EventStore`（SSE 事件持久化和重放）、`StreamableHttpServerTransport`。

## 依赖图

```
mcp-core (INTERFACE)
  └── nlohmann_json

mcp-transport (STATIC)
  └── mcp-core + asio

mcp-protocol (STATIC)
  └── mcp-transport

mcp-http (STATIC)
  └── mcp-transport

mcp-server (STATIC)
  └── mcp-protocol

mcp-client (STATIC)
  └── mcp-protocol + OpenSSL（可选）
```
