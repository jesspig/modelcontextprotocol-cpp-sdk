# Repository Guidelines

## Build & Test

```bash
cmake --preset debug                          # Configure (Ninja, Debug)
cmake --build --preset debug                  # Build
ctest --preset debug --output-on-failure      # All tests
ctest -R WireCodec                            # Single suite
cmake --build --preset debug --target mcp-server-tests   # Single target
```

Presets: `debug`, `release`. Ninja generator only. CI: push/PR to `develop` (ci.yml, 3 OS × 2 build types).

Examples are OFF by default: add `-DMCP_BUILD_EXAMPLES=ON` to configure to build examples.

No formatter/linter config — `-Wextra -Wpedantic` for Clang/GCC, `/W4` for MSVC/clang-cl.

### Non‑obvious build facts

- **Unity (jumbo) builds ON** by default. Override `-DMCP_UNITY_BUILD=OFF`.
- `mcp-transport` and `mcp-protocol` have Unity explicitly disabled (anonymous namespace symbols clash). `mcp-client` uses `UNITY_BUILD_UNIQUE_ID ON` (OAuth symbols).
- **Compiler auto-detection** runs before `project()`: clang-cl (Win) > clang++-N + matching clang-N (Linux) > system default. Skips if `CMAKE_CXX_COMPILER` already set.
- **LTO**: auto-enabled in Release (clang-cl: LTCG, Clang: ThinLTO, GCC: IPO).
- **Compiler cache**: sccache > ccache > none. sccache supports MSVC/clang-cl; ccache skips MSVC.
- **Dependencies cached in `build/<preset>/_deps/`**. Deleting `build/` is expensive.
- **Werror**: only with `-DMCP_WERROR=ON` (CI). CI adds this automatically.
- **`mcp-core` is STATIC** (JsonValue.cpp, JsonRpc.cpp, etc.) — changing serialization recompiles many dependents.

### Cross‑platform traps

- macOS: `environ` in anonymous namespaces creates mangled `mcp::detail::environ`. Use `_NSGetEnviron()`.
- macOS: `pthread_setname_np` is single-arg — guard with `#ifdef __APPLE__`.
- GCC: `(void)`-cast does **not suppress** `warn_unused_result` (Clang only). Affects `chdir()`, `close()`, `dup2()`, `pipe()`.
- Apple Clang enables `-Wunused-private-field` by default — with `-Werror` any unused private member is a hard error.
- clang-cl silently accepts both MSVC (`/W4`) and GCC (`-Wall`) flags — typos pass through.
- `CMake CMP0169`: guarded with `if(POLICY CMP0169)` — not available before CMake 3.30.

### Unity build traps

- **Header self-containment is mandatory**: Unity merges `.cpp` files; headers relying on prior `#include` order break.
- **Debugging**: error line numbers point to the generated Unity batch file, not the original source. Disable with `-DMCP_UNITY_BUILD=OFF`.
- GCC may trigger `-Wunused-function` in Unity files — guard anonymous namespace functions with `[[maybe_unused]]`.

### libhv FetchContent patch

`cmake/FetchDependencies.cmake` patches libhv's CMakeLists.txt: the `install(FILES ... DESTINATION include/hv)` is replaced with `file(COPY ...)` so the `include/hv/` directory exists at configure time (matching `hv_static`'s `BUILD_INTERFACE`). If an agent adds a new preset or modifies dependency fetching, this patch must be preserved.

## Architecture

```
include/mcp/       — Public headers
src/client/        — McpClient, OAuth, FileTokenCache
src/server/        — McpServer, FileTaskStore
src/protocol/      — McpSessionHandler (JSON-RPC engine), WireCodec (dual-era)
src/transport/     — Stdio, SSE, InMemory, WebSocket, StreamableHttp impls
src/http/          — HttpServer, EventStore, StreamableHttp*
tests/             — unit/ (gtest), integration/, conformance/
examples/          — EchoServer, WeatherServer, SimpleClient
```

Library dep chain: `mcp-core` → `mcp-transport` → `mcp-protocol` → `mcp-server | mcp-client`. `mcp-http` depends on `mcp-transport`. All STATIC. No INTERFACE / header-only libraries.

### Transport hierarchy

```
ITransport — TransportBase (3-state: Initial→Connected→Disconnected)
  ├── StdioServerTransport
  ├── InMemoryTransportImpl
  ├── StreamableHttpServerTransport
  └── *SessionTransport (internal, anonymous namespace or enable_shared_from_this)

IClientTransport (connection factory)
  ├── StdioClientTransport (PlatformIO, merged Win32+POSIX)
  ├── SseClientTransport (libhv HttpClient)
  ├── StreamableHttpClientTransport (libhv requests / WinHTTP)
  └── WebSocketClientTransport (libhv WebSocketClient)
```

No asio dependency — raw threads + stdlib primitives (mutex, condition_variable) or libhv event loop.

`MessageChannel` (`include/mcp/protocol/MessageChannel.hpp`) is a bounded async queue (`std::queue + mutex + condition_variable`) replacing `asio::experimental::channel`.

`InMemoryTransport::CreatePair()` returns `shared_ptr<ITransport>`. Use `dynamic_cast<TransportBase*>` to access state machine.

### HttpServer (`mcp-http`)

PIMPL pattern over libhv `HttpService`. Key detail: `HttpServerOptions::on_connect` and `on_disconnect` are **now wired** — they fire on SSE client add/remove respectively.

### Version negotiation (critical)

- **`HandleInitialize` must echo the client's legacy version**: Return the version the client sent (e.g., `"2025-11-25"`). Never return `kLatestProtocolVersion` (`"2026-07-28"`) — TS SDK v2 validates `result.protocolVersion` against its legacy list.
- **Modern versions (2026-07-28+) are NEVER negotiated via `initialize`**: Only via `server/discover`.
- `McpClient` connect modes: `Auto` (default, probes `server/discover`, falls back to `initialize`), `Legacy` (initialize only), `Pin` (pinned version).
- `SetNegotiatedProtocolVersion(version)` stores the version and recreates the `WireCodec`.

## Key protocol patterns

- **2026-07-28+ (modern)**: Stateless. `server/discover` replaces `initialize`/`initialized`. Per-request `_meta` carries `protocolVersion`, `clientInfo`, `clientCapabilities`, `logLevel`.
- **MRTR**: Server-initiated elicitation embedded as `InputRequiredResult`.
- **Subscriptions**: `subscriptions/listen` replaces `resources/subscribe`.
- **Caching**: `CacheHint` with `ttlMs`/`cacheScope`.
- **Mcp-Method header**: Dynamic, derived from JSON-RPC body method field (for Streamable HTTP + SSE).

## Notification handlers

All 17 notification types are registered in `WireCodec.cpp` codec collections. Server‑side handlers are wired in `McpServer::WireHandlers()`; client‑side handlers in `McpClient::WireClientHandlers()`. Notifications are dispatched via `McpSessionHandler::OnNotification()` — unregistered notifications are logged (catch‑all added in OnNotification).

**Gotchas**:
- `notifications/cancelled` is hard‑coded in `OnNotification()` before the handler map lookup (not a `SetNotificationHandler` registration).
- `logging/setLevel` is a **request** (not notification) — if adding a server, it must be registered in `WireHandlers()`.
- Progress notification handler calls `ResetTimeoutByProgressToken()` which extends the deadline by 30s via `progress_token_map_` → `pending_` lookup. Defined in `McpSessionHandler`, wired in `McpServer::WireHandlers()`.

## Server options and event hooks

`ServerOptions` exposes four callback layers: shorthand (`on_method_called`, `on_protocol_error`), full message (`on_request`, `on_response`, `on_error`, `on_notification`), lifecycle (`on_client_connected`, `on_initialized`), transport (`on_transport_close`, `on_transport_error`). For auth/audit/rate‑limiting, inject `FilterPipeline` via `incoming_filters`/`outgoing_filters`.

## Coding conventions

- **C++17**, `#pragma once`, 4‑space indent. No auto‑formatter.
- Types/functions: PascalCase. Constants: `k` + PascalCase. Members: snake_case + underscore (`io_ctx_`).
- Namespace: flat `mcp`. Sub‑namespaces: `mcp::methods`, `mcp::notifications`.
- **No external JSON in public headers**: `JsonValue` (`std::variant`-based) is the sole JSON type. Parsing uses simdjson DOM (internal). Serialization is hand‑written `Dump()`.
- `Prompt` has no `annotations` field — per spec.
- Server guards all handlers with `initialized_` flag until `notifications/initialized` received.
- `ContentVariant` includes `ResourceLink`, `ToolUseContent`, `ToolResultContent` — dispatch on `type` string.
- `JsonRpcErrorResponse::id` is `optional<RequestId>` (per JSON‑RPC 2.0 §5.1).
- Log levels via `MCP_LOG_LEVEL` env var: 0=Off, 1=Error, 2=Warning, 3=Info, 4=Debug, 5=Trace.
- OAuth HTTP/1.1 uses `Connection: close` — each token exchange opens a new TCP connection.
- `ToolOptions::InputSchema(JsonValue s)` is required for tools with parameters (default is empty schema).
- `StreamableHttpClientTransport::Name()` returns `"streamable-http"` when `options_.name` is empty.

## Testing

`InMemoryTransport` is **synchronous** — messages deliver on `Send()`/`AsyncReceive()`, no external event loop. All tests compile and pass without OpenSSL.

| Suite | Target | Key file |
|-------|--------|----------|
| JsonRpcTest | `mcp-core-tests` | Serialization + variant dispatch |
| McpTypesTest | `mcp-core-tests` | Type round‑trips |
| WireCodecTest | `mcp-wire-codec-tests` | Era‑gating codec |
| McpServerTest | `mcp-server-tests` | Registration, capabilities |
| McpClientTest | `mcp-client-tests` | Client creation, connect modes |
| OAuthTest | `mcp-oauth-tests` | PKCE, token cache |
| TransportTest | `mcp-transport-tests` | InMemory + state machine via `dynamic_cast<TransportBase*>` |
| HttpServer/EventStore/StreamableHttp | `mcp-http-tests` | HTTP server, SSE, headers |
| Conformance | `mcp-conformance-tests` | MCP spec compliance |
| ClientServerFixture | `mcp-integration-tests` | Client‑server round‑trip via InMemoryTransport |
| MessageFilterTest | `mcp-message-filter-tests` | FilterPipeline chain, stop, modify |
| FileTokenCacheTest | `mcp-token-cache-tests` | Persistence, corruption handling |
| FileTaskStoreTest | `mcp-task-store-tests` | Task CRUD, status transitions |
| StreamableHttpTransportTest | `mcp-streamable-http-tests` | Client/server construction, header validation |
| WebSocketTransportTest | `mcp-websocket-tests` | Construction, default name |

## Dependencies (auto‑fetched)

| Dep | Version | Notes |
|-----|---------|-------|
| libhv | 1.3.4 | HTTP client/server, WebSocket, event loop; `hv_static` target |
| simdjson | 3.12.3 | JSON parsing (internal, not in public headers) |
| GoogleTest | 1.15.2 | Only when `MCP_BUILD_TESTS=ON` |
| OpenSSL | system | Optional: TLS, PKCE SHA‑256 (falls back to built‑in) |

No asio. No nlohmann‑json. Both fully removed.

## Documentation

`docs/` is a vitepress site (`pnpm dev` to serve locally, `pnpm build` to build). Bilingual: `en/` and `zh/` directories. The SDK uses this for user-facing docs; generated API docs are in `docs/.vitepress/dist/`.

## Commits

Conventional Commits with scope: `feat(transport):`, `fix(server):`, etc. Scopes: `client`, `server`, `protocol`, `transport`, `http`, `core`, `build`, `test`, `examples`.
