# Repository Guidelines

## Build & Test

```bash
cmake --preset debug                          # Configure (Ninja, Debug)
cmake --build --preset debug                  # Build
ctest --preset debug --output-on-failure      # Run all ~219 tests
ctest -R WireCodec                            # Filter single suite
ctest -R Conformance -V                       # Filter group with verbose
cmake --build --preset debug --target mcp-core-tests   # Single target
```

Presets: `debug`, `release`. Ninja generator only.

### Non-obvious build facts

- **Unity (jumbo) builds ON** by default. Override `-DMCP_UNITY_BUILD=OFF`. `mcp-client` uses `UNITY_BUILD_UNIQUE_ID ON` to avoid OAuth symbol clashes.
- **Ninja job pools**: compile `min(mem/1500MB, cores-2)`, link `min(mem/4000MB, 2)`. Override via `MCP_COMPILE_JOBS` / `MCP_LINK_JOBS`.
- **Compiler auto-detection**: clang-cl (if found) → system default. sccache → ccache → none.
- **lld-link**: auto-detected on Windows MSVC + Ninja; adds `/lldlink`.
- **CI** (`ci.yml`) runs on `develop` branch only with `-DMCP_WERROR=ON` — treat warnings as errors. Three OS matrix: windows-2022, ubuntu-24.04, macos-15.
- **Dependencies cached in `build/<preset>/_deps/`**. Do NOT delete `build/` — re-downloading is expensive.
- `mcp-core` is INTERFACE (header-only). Changing type serialization recompiles everything.
- **OAuth requires OpenSSL**: `mcp-client` and `mcp-transport` get `MCP_HAVE_OPENSSL` when OpenSSL is found. Building `mcp-client` without OpenSSL fails with `static_assert` — intentional (PKCE SHA-256 is mandatory).
- **`StreamableHttpClientTransport.cpp` is Windows-only**: guarded via `if(WIN32)`.
- **`McpServer::Create` / `McpClient::Create` take `shared_ptr<ITransport>`**: When using `StdioServerTransport`, both must share the same `asio::io_context`. Pass `McpServer::Create(transport, opts, &io_ctx)`. Omitting creates an internal io_context — causes silent data loss.
- **`InMemoryTransport::CreatePair()`** returns `shared_ptr<ITransport>`, not the concrete type. Use `dynamic_cast<TransportBase*>` to access state machine.

## Architecture

```
include/mcp/       — Public headers
src/client/        — McpClient, OAuth, MRTR helper
src/server/        — McpServer, Extension framework, FileTaskStore
src/protocol/      — McpSessionHandler (engine), WireCodec
src/transport/     — Stdio, SSE, InMemory, WebSocket transport impls
src/http/          — HttpServer, EventStore, StreamableHttp*
tests/             — unit/ (gtest), integration/, conformance/ (122 tests)
examples/          — EchoServer, WeatherServer, SimpleClient
```

Library dependency chain: `mcp-core` (INTERFACE) → `mcp-transport` → `mcp-protocol` → `mcp-server | mcp-client`. `mcp-http` depends on `mcp-transport`.

### Transport hierarchy

```
ITransport —— TransportBase (3-state: Initial→Connected→Disconnected)
  ├── StdioServerTransport
  ├── InMemoryTransportImpl
  ├── StreamableHttpServerTransport (+ IStatelessTransport)
  ├── WebSocketTransport
  └── StreamableHttpSessionTransport (internal, via StreamableHttpClientTransport)

IClientTransport (connection factory)
  ├── StdioClientTransport (Win32 + POSIX via PlatformIO)
  ├── SseClientTransport (dual WinHTTP/asio POSIX path)
  ├── StreamableHttpClientTransport (Windows-only)
  └── WebSocketClientTransport (wss:// guarded by MCP_HAVE_OPENSSL)
```

### Protocol engine

`McpSessionHandler` (in `mcp-protocol`) is the JSON-RPC engine:

- Async message loop over `MessageChannel` (wraps `asio::experimental::channel`)
- Request/response correlation with timeout; `next_request_id_` is `std::atomic<int64_t>`
- Handler dispatch via `unordered_map`, inbound/outbound `MessageFilter` pipeline (0 by default)
- Dual-era `WireCodec` (2025-11-25 initialize handshake / 2026-07-28 per-request `_meta`)

## Key protocol patterns (2026-07-28 era)

- **Stateless**: `initialize`/`initialized` handshake replaced by `server/discover`. Per-request `_meta` carries `protocolVersion`, `clientInfo`, `clientCapabilities` on every C→S request.
- **MRTR**: Server-initiated interactions (elicitation, sampling, roots) embedded as `InputRequiredResult` — not sent as separate requests on SSE. Client retries with `inputResponses` + `requestState`.
- **Subscriptions**: `subscriptions/listen` replaces `resources/subscribe`. Opt-in to change notification types.
- **Extensions**: Negotiated via `extensions` map (now `map<string, json>`, not empty struct) on `ClientCapabilities`/`ServerCapabilities`.
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

| Suite | Target | Location | Notes |
|-------|--------|----------|-------|
| `JsonRpcTest` | `mcp-core-tests` | `tests/unit/` | Message serialization + variant dispatch |
| `McpTypesTest` | `mcp-core-tests` | `tests/unit/` | Type round-trips with annotations, icons, content variants |
| `WireCodecTest` | `mcp-wire-codec-tests` | `tests/unit/` | Era-gating codec |
| `McpServerTest` | `mcp-server-tests` | `tests/unit/` | Registration, capabilities |
| `McpClientTest` | `mcp-client-tests` | `tests/unit/` | Client creation, tool cache |
| `OAuthTest` | `mcp-oauth-tests` | `tests/unit/` | PKCE, token cache |
| `TransportTest` | `mcp-transport-tests` | `tests/unit/` | State machine, error propagation via `dynamic_cast<TransportBase*>` |
| `Conformance` | `mcp-conformance-tests` | `tests/conformance/` | 122 MCP spec compliance tests |
| `Integration` | `mcp-integration-tests` | `tests/integration/` | Client-server round-trip |

- `InMemoryTransport` is synchronous — messages delivered when `io_context` runs, not on `SendMessageAsync`.
- Werror is off by default; enable via `-DMCP_WERROR=ON` for CI-matching behavior.

## Traps

- **All protocol types in `McpTypes.hpp`**: Result, Progress, etc. were consolidated here. Do not create new type headers.
- **Content annotations**: typed `Annotations` struct (not raw JSON). Has `audience`, `priority`, `lastModified`.
- **`Prompt` has no `annotations` field** — removed per spec. Do not add it back.
- **`ExtensionsCapability` removed**: replaced by `map<string, json>` on both `ClientCapabilities` and `ServerCapabilities`.
- **Server guards requests with `initialized_` flag**: All handlers reject with `InvalidRequest` until `notifications/initialized` received.
- **`McpClient` sends `notifications/initialized` after `HandshakeInitialize`**: Required by 2025-era protocol.
- **OAuth HTTP/1.1 uses `Connection: close`**: Each token exchange opens a new TCP connection. No keep-alive.
- **No HTTP/1.1 keep-alive**: OAuth client opens separate connections.
- **`jsonrpc: "2.0"` validated** in all `from_json`; invalid version throws `std::runtime_error`.
- **`JsonRpcErrorResponse::id` is `optional<RequestId>`**: Null-id errors (parse errors, invalid requests) serialize/deserialize correctly per JSON-RPC 2.0 §5.1.
- **`Icon::mime_type` is `optional<string>`** (not required per spec).
- **`ContentVariant` includes `ResourceLink`**: handle `type == "resource_link"` in disaptch.
- **X-Mcp-Headers**: `ScanXMcpHeaders()` in `XMcpHeaders.hpp` scans inputSchema for `x-mcp-header` annotations.

## Dependencies (auto-fetched via FetchContent)

| Dep | Version | Notes |
|-----|---------|-------|
| asio | 1.30.2 | Header-only; manual INTERFACE target |
| nlohmann-json | 3.11.3 | SYSTEM, shallow fetch |
| GoogleTest | 1.15.2 | Only when `MCP_BUILD_TESTS=ON` |
| OpenSSL | system | Optional; required for OAuth PKCE |

## Commits

Conventional Commits with scope: `feat(transport):`, `fix(server):`, etc. Scopes: `client`, `server`, `protocol`, `transport`, `http`, `core`, `build`, `test`, `examples`.
