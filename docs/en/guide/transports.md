# Transports

| Transport       | Client | Server | Description                                   |
|-----------------|--------|--------|-----------------------------------------------|
| Stdio           | Yes    | Yes    | stdin/stdout pipes, subprocess communication  |
| Streamable HTTP | Yes    | Yes    | HTTP POST with streaming server-sent events   |
| SSE             | Yes    | No     | Server-Sent Events for push notifications     |
| WebSocket       | Yes    | No     | TCP-based bidirectional (simplified RFC 6455) |
| InMemory        | Yes    | Yes    | In-process for testing                        |

## Transport Interfaces

```
ITransport (session connection)
  └── TransportBase (3-state: Initial → Connected → Disconnected)
      ├── StdioServerTransport
      ├── InMemoryTransportImpl
      ├── StreamableHttpServerTransport
      └── WebSocketTransport

IClientTransport (connection factory)
  ├── StdioClientTransport
  ├── SseClientTransport
  ├── StreamableHttpClientTransport
  └── WebSocketClientTransport
```

## Stateless Transport

`StreamableHttpServerTransport` has `IsStateless() == true`. When detected, the server disables MRTR (`InputRequiredResult`) — requests must carry full context via `_meta` and `requestState` (opaque token for cross-request state recovery).
