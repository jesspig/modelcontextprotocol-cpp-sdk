# Transports

| Transport       | Client | Server | Description                                      |
|-----------------|--------|--------|--------------------------------------------------|
| Stdio           | Yes    | Yes    | stdin/stdout pipes, subprocess communication     |
| Streamable HTTP | Yes    | Yes    | HTTP POST with JSON/session-mode responses       |
| SSE             | Yes    | Yes¹   | Server-Sent Events for push notifications        |
| WebSocket       | Yes    | No     | TCP-based bidirectional (self-hosted WebSocketClient)  |
| InMemory        | Yes    | Yes    | In-process for testing                           |

## Streamable HTTP

The Streamable HTTP transport implements the MCP Streamable HTTP specification. Each session uses an internal `StreamableHttpSessionTransport` (wrapping `ITransport`) with its own message channel and background thread.

**Request flow**: Client sends a JSON-RPC message via HTTP POST. In **stateless** mode, the server processes the request synchronously and responds with a JSON response (correlated via `std::promise` with a 30-second timeout). In **session** mode, the server returns `202 Accepted` and delivers results asynchronously through SSE (via a separate GET endpoint). The server validates `Mcp-Method` and `Mcp-Name` headers against the body (SEP-2243), echoes them back on the response, and extracts `Mcp-Param-*` headers into `_meta.x-mcp-headers`.

**SSE read loop**: On the client side, if the POST response includes SSE events (e.g., session-mode results), a background thread reads chunks, splits on `\n\n` delimiters, parses `data:` lines, and enqueues `JsonRpcMessage` into the `MessageChannel`.

**Headers** (`StreamableHttpClientTransport`):
- `MCP-Protocol-Version: 2026-07-28` — always sent
- `Mcp-Method` — dynamic, derived from JSON-RPC body method field
- `Accept: application/json, text/event-stream` — allows server to pick response mode

## Transport Interfaces

```
ITransport (session connection)
  └── TransportBase (3-state: Initial → Connected → Disconnected)
      ├── StdioServerTransport
      ├── StdioClientSessionTransport (internal)
      ├── InMemoryTransportImpl
      ├── SseClientSessionTransport (internal)
      ├── WebSocketSessionTransport (wraps self-hosted WebSocketClient)
      ├── StreamableHttpServerTransport
      └── StreamableHttpSessionTransport (internal)

IClientTransport (connection factory)
  ├── StdioClientTransport (PlatformIO)
  ├── SseClientTransport (self-hosted HttpClient)
  ├── StreamableHttpClientTransport (self-hosted HttpClient / WinHTTP)
  └── WebSocketClientTransport (self-hosted WebSocketClient)
```

## Key Types

### `HttpTransportMode`
Controls how the HTTP client transport connects:

| Value          | Description                                              |
|----------------|----------------------------------------------------------|
| `AutoDetect`   | Try Streamable HTTP (`server/discover`) first; fall back to SSE |
| `StreamableHttp` | Use Streamable HTTP directly (requires 2026-07-28+)    |
| `Sse`          | Use legacy SSE POST only                                 |

### `StdioClientTransportOptions`
| Field                        | Type                          | Default  | Description                          |
|------------------------------|-------------------------------|----------|--------------------------------------|
| `command`                    | `std::string`                 | required | Subprocess command                   |
| `arguments`                  | `std::vector<std::string>`    | `{}`     | Command-line arguments               |
| `name`                       | `std::string`                 | `""`    | Transport name (empty falls back to `"stdio"`) |
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
| `auth_challenge_handler` | `std::function<std::string(std::string_view www_authenticate)>` | `nullptr` | Called with `WWW-Authenticate` on 401/403 (RFC 9728); a non-empty `Authorization` header retries exactly once |

### `StreamableHttpServerOptions`
| Field                  | Type                               | Default      | Description                          |
|------------------------|------------------------------------|--------------|--------------------------------------|
| `port`                 | `uint16_t`                         | `3001`       | HTTP server port                     |
| `endpoint`             | `std::string`                      | `"/mcp"`     | HTTP endpoint path                   |
| `host`                 | `std::string`                      | `""`         | Listen bind address; empty = all interfaces, accepts IPv4/IPv6 literals |
| `stateless`            | `bool`                             | `true`       | 2026-07-28 stateless mode            |
| `enable_legacy_sse`    | `bool`                             | `true`       | Serve SSE stream on GET              |
| `sse_keep_alive_ms`    | `int`                              | `15000`      | SSE keepalive comment-frame interval in ms; `0` disables |
| `event_store`          | `std::shared_ptr<EventStore>`      | `nullptr`    | Event store for resumption; defaults to the built-in in-memory store when unset — plug in `FileEventStore` for a custom implementation |
| `session_store`        | `std::shared_ptr<SessionStore>`    | `nullptr`    | External session store (session mode only); instances sharing the same store can adopt each other's sessions |
| `bearer_auth`          | `std::optional<BearerAuthConfig>`  | `std::nullopt` | Bearer auth config (RFC 6750/9728); see below |
| `server_name`          | `std::string`                      | `"mcp-server"` | Server name for discovery         |
| `server_version`       | `std::string`                      | `kSdkVersion` | Server version for discovery         |

### `BearerAuthConfig`
Once `bearer_auth` is set, `StreamableHttpServerTransport` runs the bearer auth gate at the top of POST/GET handling, before any session handling:

| Field                      | Type                                       | Description                                             |
|----------------------------|--------------------------------------------|---------------------------------------------------------|
| `verify`                   | `function<AuthResult(const std::string&)>` | Required; returns `AuthResult{ok, scopes}` — `ok=false` rejects with `401 error="invalid_token"` |
| `resource_metadata_url`    | `std::string`                              | Full URL of the protected-resource metadata document; also referenced by the `WWW-Authenticate` challenge |
| `scopes_supported`         | `vector<string>`                           | Scopes advertised in the metadata document               |
| `required_scopes`          | `vector<string>`                           | Token scopes must cover all of them; empty disables scope checks (missing scopes → 403) |
| `authorization_servers`    | `vector<string>`                           | Authorization servers written into the metadata document |
| `serve_metadata_endpoint`  | `bool`                                     | Register `GET /.well-known/oauth-protected-resource` (no auth); default `true` |

Outcomes on both the POST and GET (SSE) paths: a missing or malformed `Authorization: Bearer` header → `401` challenge; `verify` returning `ok=false` → `401` with `error="invalid_token"`; a token missing any `required_scopes` entry → `403` with `error="insufficient_scope"`. When `bearer_auth` is unconfigured, behavior is exactly as before — zero regression. For the client-side counterpart see [OAuth Support](/en/client/oauth).

### `InMemoryTransport::Pair`
```cpp
struct Pair {
    std::shared_ptr<ITransport> client;
    std::shared_ptr<ITransport> server;
};
```

> ¹ SSE server-side is provided through `StreamableHttpServerTransport` when `enable_legacy_sse` is `true` (default), which serves an SSE stream on the same endpoint via HTTP GET.

## Stateless Mode

`StreamableHttpServerTransport` supports stateless mode controlled by `StreamableHttpServerOptions::stateless` (default `true`). When `true`, `IsStateless()` returns `true` and:

- **No sessions**: Each request is independent; the response is correlated synchronously via `std::promise` with a 30-second timeout.
- **No SSE**: No SSE broadcast or `EventStore` append; only responses correlated to pending requests are delivered via JSON.
