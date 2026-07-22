# Architecture

## Library Layers

```
┌─────────────────────────────────────┐
│  mcp-server        mcp-client       │  Server & Client API
├─────────────────────────────────────┤
│  mcp-protocol                       │  WireCodec, McpSessionHandler
├─────────────────────────────────────┤
│  mcp-transport       mcp-http       │  Transports, Streamable HTTP, SSE
├─────────────────────────────────────┤
│  mcp-core                           │  Types, JSON-RPC, interfaces
└─────────────────────────────────────┘
```

### mcp-core (STATIC)

Core protocol types and JSON infrastructure: `JsonValue` (recursive `std::variant`-based JSON), `JsonRpcRequest`, `JsonRpcResponse`, `JsonRpcNotification`, `JsonRpcErrorResponse`, error codes, capability declarations (`ClientCapabilities`, `ServerCapabilities`), content types (`TextContent`, `ImageContent`, `EmbeddedResource`, `ResourceLink`), transport interfaces (`ITransport`, `IClientTransport`).

### mcp-transport (STATIC)

Transport implementations: `StdioServerTransport`, `StdioClientTransport`, `SseClientTransport`, `InMemoryTransport`, `WebSocketClientTransport`, `StreamableHttpServerTransport`, `StreamableHttpClientTransport`. Platform-specific I/O in `detail/` (posix, win32).

### mcp-protocol (STATIC)

JSON-RPC engine (`McpSessionHandler`): async message loop over `MessageChannel`, request/response correlation with timeout, handler dispatch via `unordered_map`, dual-era `WireCodec` (2025-11-25 initialize handshake / 2026-07-28 per-request `_meta`), inbound/outbound `MessageFilter` pipeline.

### mcp-server (STATIC)

`McpServer`: tool/resource/prompt registration, `Extension` framework (SEP-2133), `IMcpTaskStore` (with `FileTaskStore` for persistence), server-to-client `Elicit`, MRTR (`InputRequiredResult`), subscription management, stateless mode for Streamable HTTP.

### mcp-client (STATIC)

`McpClient`: server discovery (`server/discover` → initialize fallback), version negotiation, tool cache, MRTR driver (`MrtrDriver`), OAuth (PKCE, DCR, token refresh/revocation, `FileTokenCache`).

### mcp-http (STATIC)

HTTP server for Streamable HTTP mode: `HttpServer`, `EventStore` (SSE event persistence and replay), `StreamableHttpServerTransport`, `StreamableHttpClientTransport`.

## Dependency Graph

```
mcp-core (STATIC)
  ├── simdjson  (JSON parsing, internal)
  └── libhv     (HTTP/WebSocket, via mcp-transport)

mcp-transport (STATIC)
  └── mcp-core + libhv

mcp-protocol (STATIC)
  └── mcp-transport

mcp-http (STATIC)
  └── mcp-transport + libhv

mcp-server (STATIC)
  └── mcp-protocol

mcp-client (STATIC)
  └── mcp-protocol + OpenSSL (optional)
```
