# Architecture

## Library Layers

```
┌─────────────────────────────────────┐
│  mcp-server        mcp-client       │  Server & Client API
├─────────────────────────────────────┤
│  mcp-protocol                       │  WireCodec, McpSessionHandler
├─────────────────────────────────────┤
│  mcp-transport                      │  Stdio, SSE, WebSocket, InMemory
├─────────────────────────────────────┤
│  mcp-core            mcp-http       │  Types, JSON-RPC, HTTP serving
└─────────────────────────────────────┘
```

### mcp-core (header-only INTERFACE)

All protocol types: `Tool`, `Resource`, `Prompt`, `ElicitResult`, `CallToolResult`, JSON-RPC message structures (`JsonRpcRequest`, `JsonRpcResponse`, `JsonRpcNotification`, `JsonRpcErrorResponse`), error codes, capability declarations, transport interfaces (`ITransport`, `IClientTransport`).

### mcp-transport (STATIC)

Transport implementations: `StdioServerTransport`, `StdioClientTransport`, `SseClientTransport`, `InMemoryTransport`, `WebSocketClientTransport`. Platform-specific I/O in `detail/` (posix, win32).

### mcp-protocol (STATIC)

JSON-RPC engine (`McpSessionHandler`): async message loop over `MessageChannel`, request/response correlation with timeout, handler dispatch via `unordered_map`, dual-era `WireCodec` (2025-11-25 initialize handshake / 2026-07-28 per-request `_meta`), inbound/outbound `MessageFilter` pipeline.

### mcp-server (STATIC)

`McpServer`: tool/resource/prompt registration, `Extension` framework (SEP-2133), `IMcpTaskStore` (with `FileTaskStore` for persistence), server-to-client `Elicit`, MRTR (`InputRequiredResult`), subscription management, stateless mode for Streamable HTTP.

### mcp-client (STATIC)

`McpClient`: server discovery (`server/discover` → initialize fallback), version negotiation, tool cache, MRTR driver (`MrtrDriver`), OAuth (PKCE, DCR, token refresh/revocation, `FileTokenCache`).

### mcp-http (STATIC)

HTTP server for Streamable HTTP mode: `HttpServer`, `EventStore` (SSE event persistence and replay), `StreamableHttpServerTransport`.

## Dependency Graph

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
  └── mcp-protocol + OpenSSL (optional)
```
