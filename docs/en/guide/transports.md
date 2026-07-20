# Transports

| Transport       | Client | Server | Description                                   |
|-----------------|--------|--------|-----------------------------------------------|
| Stdio           | Yes    | Yes    | stdin/stdout pipes, subprocess communication  |
| Streamable HTTP | Yes    | Yes    | HTTP POST with streaming server-sent events   |
| SSE             | Yes    | Yes¹   | Server-Sent Events for push notifications     |
| WebSocket       | Yes    | No     | TCP-based bidirectional (simplified RFC 6455) |
| InMemory        | Yes    | Yes    | In-process for testing                        |

## Streamable HTTP

The Streamable HTTP transport implements the MCP Streamable HTTP specification. Each session uses an internal `StreamableHttpSessionTransport` (wrapping `ITransport`) with a dedicated `asio::io_context`.

**Request flow**: Client sends a JSON-RPC message via HTTP POST. The server validates `Mcp-Method` and `Mcp-Name` headers against the body (SEP-2243), echoes them back on the response, and extracts `Mcp-Param-*` headers into `_meta.x-mcp-headers`. If the response is JSON, the server responds synchronously in **stateless** mode (with pending-response correlation via `std::promise`) or returns `202 Accepted` in session mode. If the response is SSE (`text/event-stream`), the server keeps the connection open and streams events.

**SSE read loop**: On the client side, when the POST response has `Content-Type: text/event-stream`, a background thread (`SseReadLoop`) reads chunks, splits on `\n\n` delimiters, parses `data:` lines, and enqueues `JsonRpcMessage` into the `MessageChannel`.

**Headers** (`StreamableHttpClientTransport`):
- `MCP-Protocol-Version: 2026-07-28` — always sent
- `Mcp-Method` — dynamic, derived from JSON-RPC body method field
- `Mcp-Param-*` — primitive params extracted for middleware routing (strings, integers, booleans only)
- `Accept: application/json, text/event-stream` — allows server to pick response mode

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

## Key Types

### `HttpTransportMode`
Controls how the HTTP client transport connects:

| Value          | Description                                              |
|----------------|----------------------------------------------------------|
| `AutoDetect`   | Probe `server/discover` first; fall back to SSE POST     |
| `StreamableHttp` | Use Streamable HTTP directly (requires 2026-07-28+)    |
| `Sse`          | Use legacy SSE POST only                                 |

### `StdioClientTransportOptions`
| Field                        | Type                          | Default  | Description                          |
|------------------------------|-------------------------------|----------|--------------------------------------|
| `command`                    | `std::string`                 | required | Subprocess command                   |
| `arguments`                  | `std::vector<std::string>`    | `{}`     | Command-line arguments               |
| `name`                       | `std::string`                 | `"stdio"`| Transport name                       |
| `working_directory`          | `std::string`                 | `""`     | Subprocess working directory         |
| `inherit_environment_variables` | `bool`                     | `true`   | Inherit parent environment           |
| `environment_variables`      | `std::map<std::string, std::string>` | `{}` | Additional env vars        |

### `HttpClientTransportOptions`
| Field                  | Type                                    | Default         | Description                   |
|------------------------|-----------------------------------------|-----------------|-------------------------------|
| `endpoint`             | `std::string`                           | required        | Server endpoint URL           |
| `transport_mode`       | `HttpTransportMode`                     | `AutoDetect`    | Connection mode               |
| `name`                 | `std::string`                           | `""`            | Transport name                |
| `known_session_id`     | `std::string`                           | `""`            | Session ID for resumption     |
| `additional_headers`   | `std::map<std::string, std::string>`    | `{}`            | Extra HTTP headers            |

### `StreamableHttpServerOptions`
| Field                  | Type                               | Default      | Description                          |
|------------------------|------------------------------------|--------------|--------------------------------------|
| `port`                 | `uint16_t`                         | `3001`       | HTTP server port                     |
| `endpoint`             | `std::string`                      | `"/mcp"`     | HTTP endpoint path                   |
| `stateless`            | `bool`                             | `false`      | Enable 2026-07-28 stateless mode     |
| `enable_legacy_sse`    | `bool`                             | `true`       | Serve SSE stream on GET              |
| `event_store`          | `std::shared_ptr<EventStore>`      | `nullptr`    | Event store for resumption           |
| `server_name`          | `std::string`                      | `"mcp-server"` | Server name for discovery         |
| `server_version`       | `std::string`                      | `"0.1.0"`    | Server version for discovery         |

### `InMemoryTransport::Pair`
```cpp
struct Pair {
    std::shared_ptr<ITransport> client;
    std::shared_ptr<ITransport> server;
};
```

> ¹ SSE server-side is provided through `StreamableHttpServerTransport` when `enable_legacy_sse` is `true` (default), which serves an SSE stream on the same endpoint via HTTP GET.

## Stateless Mode

`StreamableHttpServerTransport` supports stateless mode controlled by `StreamableHttpServerOptions::stateless` (default `false`). When `true`, `IsStateless()` returns `true` and:

- **No sessions**: Each request is independent; the response is correlated synchronously via `std::promise` with a 30-second timeout.
- **No SSE**: Server-initiated notifications are delivered inline via JSON response; `EventStore` append is skipped.
- **MRTR disabled**: The server disables `InputRequiredResult` — requests must carry full context via `_meta` and `requestState` (opaque token for cross-request state recovery).
