# Repository Guidelines

## Build & Test

```bash
cmake --preset debug                          # Configure (Ninja, Debug)
cmake --build --preset debug                  # Build
ctest --preset debug --output-on-failure      # Run ~200+ tests
ctest -R WireCodec                            # Filter single suite
ctest -R Conformance -V                       # Filter group with verbose
cmake --build --preset debug --target mcp-server-tests   # Single target
```

Presets: `debug`, `release`. Ninja generator only.

### Non-obvious build facts

- **Unity (jumbo) builds ON** by default. Override `-DMCP_UNITY_BUILD=OFF`. `mcp-client` uses `UNITY_BUILD_UNIQUE_ID ON` to avoid OAuth symbol clashes.
- **Ninja job pools**: compile `min(mem/1500MB, cores-2)`, link `min(mem/4000MB, 2)`. Override via `MCP_COMPILE_JOBS` / `MCP_LINK_JOBS`.
- **Compiler auto-detection**: clang-cl (if found) → system default. sccache → ccache → none.
- **lld-link**: auto-detected on Windows MSVC + Ninja; adds `/lldlink`.
- **CI** (`ci.yml`) runs on `develop` branch only with `-DMCP_WERROR=ON`. Three OS: windows-2022, ubuntu-24.04, macos-15.
- **Dependencies cached in `build/<preset>/_deps/`**. Do NOT delete `build/` — re-downloading is expensive.
- **`mcp-core` is INTERFACE (header-only)**. Changing type serialization recompiles everything.
- **OAuth requires OpenSSL**: `mcp-client` gets `MCP_HAVE_OPENSSL` when OpenSSL is found. Without it, `OAuthClientProvider.cpp` hits `static_assert` (intentional — PKCE SHA-256 is mandatory). You cannot build `mcp-client` without OpenSSL.
- **`McpServer::Create` / `McpClient::Create` take `shared_ptr<ITransport>`**: When using `StdioServerTransport`, both must share the same `asio::io_context`. Pass `McpServer::Create(transport, opts, &io_ctx)`. Omitting creates an internal io_context — causes silent data loss.
- **`InMemoryTransport::CreatePair()`** returns `shared_ptr<ITransport>`, not the concrete type. Use `dynamic_cast<TransportBase*>` to access state machine.
- **`StreamableHttpClientTransport.cpp` now compiles on all platforms**: Win32 uses WinHTTP, POSIX uses asio native sockets.

## Architecture

```
include/mcp/       — Public headers
src/client/        — McpClient, OAuth, FileTokenCache
src/server/        — McpServer, FileTaskStore
src/protocol/      — McpSessionHandler (engine), WireCodec
src/transport/     — Stdio, SSE, InMemory, WebSocket transport impls
src/http/          — HttpServer, EventStore, StreamableHttp*
tests/             — unit/ (gtest), integration/, conformance/
examples/          — EchoServer, WeatherServer, SimpleClient
```

Library dependency chain: `mcp-core` (INTERFACE) → `mcp-transport` → `mcp-protocol` → `mcp-server | mcp-client`. `mcp-http` depends on `mcp-transport`.

### Transport hierarchy

```
ITransport —— TransportBase (3-state: Initial→Connected→Disconnected)
  ├── StdioServerTransport
  ├── InMemoryTransportImpl
  ├── StreamableHttpServerTransport
  ├── WebSocketTransport
  └── StreamableHttpSessionTransport (internal, via StreamableHttpClientTransport)

IClientTransport (connection factory)
  ├── StdioClientTransport (merged Win32+POSIX via PlatformIO)
  ├── SseClientTransport (WinHTTP / asio+ssl POSIX path)
  ├── StreamableHttpClientTransport (WinHTTP / asio POSIX path)
  └── WebSocketClientTransport (wss:// guarded by MCP_HAVE_OPENSSL)
```

### Protocol engine

`McpSessionHandler` (in `mcp-protocol`) is the JSON-RPC engine:

- Async message loop over `MessageChannel` (wraps `asio::experimental::channel`)
- Request/response correlation with timeout; `next_request_id_` is `std::atomic<int64_t>`
- Handler dispatch via `unordered_map`
- Dual-era `WireCodec` (2025-11-25 initialize handshake / 2026-07-28 per-request `_meta`)

## Key protocol patterns (2026-07-28 era)

- **Stateless**: `initialize`/`initialized` handshake replaced by `server/discover`. Per-request `_meta` carries `protocolVersion`, `clientInfo`, `clientCapabilities` on every C→S request.
- **MRTR**: Server-initiated interactions (elicitation) embedded as `InputRequiredResult` — not sent as separate requests on SSE. Client retries with `inputResponses` + `requestState`.
- **Subscriptions**: `subscriptions/listen` replaces `resources/subscribe`. Opt-in to change notification types.
- **Extensions**: Negotiated via `map<string, json>` on `ClientCapabilities`/`ServerCapabilities`.
- **Caching**: `CacheHint` with `ttlMs`/`cacheScope` on list/discover/read results.
- **Mcp-Method header**: Dynamic (not hardcoded), derived from JSON-RPC body method field.

## Coding Style

- **C++17**, `#pragma once`, 4-space indent. No auto-formatter.
- **Types/Functions**: PascalCase (`WireCodec`, `MakeWireCodec`). Constants: `k` + PascalCase.
- **Members**: snake_case + underscore (`io_ctx_`).
- **Namespace**: flat `mcp`. Sub-namespaces for constants: `mcp::methods`, `mcp::notifications`.
- **Compiler warnings**: `/W4` on MSVC/clang-cl, `-Wall -Wextra -Wpedantic` on Clang/GCC; `-Wno-unused-parameter` allowed. CI adds `-DMCP_WERROR=ON`.
- **Includes**: `<mcp/McpCore.hpp>` (umbrella), `<mcp/server/McpServer.hpp>`, `<mcp/transport/StdioServerTransport.hpp>`.

## Testing (Google Test, auto-fetched)

| Suite | Target | Notes |
|-------|--------|-------|
| `JsonRpcTest` | `mcp-core-tests` | Message serialization + variant dispatch |
| `McpTypesTest` | `mcp-core-tests` | Type round-trips with annotations, icons, content variants |
| `WireCodecTest` | `mcp-wire-codec-tests` | Era-gating codec |
| `McpServerTest` | `mcp-server-tests` | Registration, capabilities |
| `McpClientTest` | `mcp-client-tests` | Requires OpenSSL to build |
| `OAuthTest` | `mcp-oauth-tests` | PKCE, token cache; requires OpenSSL |
| `TransportTest` | `mcp-transport-tests` | State machine via `dynamic_cast<TransportBase*>` |
| `Conformance` | `mcp-conformance-tests` | MCP spec compliance; requires OpenSSL |
| `Integration` | `mcp-integration-tests` | Client-server round-trip; requires OpenSSL |

- `InMemoryTransport` is synchronous — messages delivered when `io_context` runs, not on `SendMessageAsync`.
- Werror is off by default; enable via `-DMCP_WERROR=ON` for CI-matching behavior.
- Tests requiring OpenSSL (`mcp-client` dependents) cannot build without it. All other 65 tests are standalone.

## Traps

- **All protocol types in `McpTypes.hpp`**: Result, Progress, etc. were consolidated here. Do not create new type headers.
- **Content annotations**: typed `Annotations` struct (not raw JSON). Has `audience`, `priority`, `lastModified`.
- **`Prompt` has no `annotations` field** — removed per spec. Do not add it back.
- **`ExtensionsCapability` removed**: replaced by `map<string, json>` on both `ClientCapabilities` and `ServerCapabilities`.
- **Server guards requests with `initialized_` flag**: All handlers reject with `InvalidRequest` until `notifications/initialized` received.
- **`McpClient` sends `notifications/initialized` after `HandshakeInitialize`**: Required by 2025-era protocol.
- **OAuth HTTP/1.1 uses `Connection: close`**: Each token exchange opens a new TCP connection. No keep-alive.
- **`jsonrpc: &quot;2.0&quot;` validated** in all `from_json`; invalid version throws `std::runtime_error`.
- **`JsonRpcErrorResponse::id` is `optional<RequestId>`**: Null-id errors serialize correctly per JSON-RPC 2.0 §5.1.
- **`Icon::mime_type` is `optional<string>`** (not required per spec).
- **`ContentVariant` includes `ResourceLink`**: handle `type == &quot;resource_link&quot;` in dispatch.
- **`ErrorCodes.hpp` has fine-grained codes**: `DeserializeFailed`, `ConnectionRefused`, `TlsHandshakeFailed`, `ProtocolViolation`, `TaskNotFound`, `HandlerError` in addition to JSON-RPC standard codes. Integrates with `std::error_code`.
- **Log levels via `MCP_LOG_LEVEL` env var**: 0=Off (default), 1=Error, 2=Warning, 3=Info, 4=Debug, 5=Trace. Use `MCP_LOG(Error, msg)`, `MCP_BUG(msg)`, or `MCP_LOG_CTX(LEVEL, ctx, msg)` macros.

## Dependencies (auto-fetched via FetchContent)

| Dep | Version | Notes |
|-----|---------|-------|
| asio | 1.30.2 | Header-only; manual INTERFACE target |
| nlohmann-json | 3.11.3 | SYSTEM, shallow fetch |
| GoogleTest | 1.15.2 | Only when `MCP_BUILD_TESTS=ON` |
| OpenSSL | system | Required for OAuth PKCE; `mcp-client` won't build without it |

## Commits

Conventional Commits with scope: `feat(transport):`, `fix(server):`, etc. Scopes: `client`, `server`, `protocol`, `transport`, `http`, `core`, `build`, `test`, `examples`.
