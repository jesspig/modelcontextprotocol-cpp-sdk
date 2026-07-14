# Repository Guidelines

## Build & Test

```bash
cmake --preset debug                          # Configure (Ninja, Debug)
cmake --build --preset debug                  # Build
ctest --preset debug --output-on-failure      # Run all 216 tests
ctest -R WireCodec                            # Filter single suite
ctest -R "Conformance" -V                     # Filter group with verbose
```

Presets: `debug`, `release`. Ninja generator only.

### Non-obvious build facts

- **Unity (jumbo) builds ON** by default (multi-core). Override `-DMCP_UNITY_BUILD=OFF`. `mcp-client` sets `UNITY_BUILD_UNIQUE_ID ON` to avoid OAuth symbol clashes.
- **Ninja job pools**: compile `min(mem/1500MB, cores-2)`, link `min(mem/4000MB, 2)`. Auto-computed. Override via `MCP_COMPILE_JOBS` / `MCP_LINK_JOBS`.
- **Compiler auto-detection priority**: clang-cl (if found) → system default. sccache → ccache → none.
- **lld-link**: auto-detected on Windows MSVC + Ninja; `/lldlink` added automatically.
- **Out-of-source libs**: `mcp-transport` links `winhttp` on Windows only.
- **Dependencies cached per build dir** in `build/<preset>/_deps/`. Do NOT delete `build/` — re-downloading dependencies is expensive.
- `mcp-core` is INTERFACE (header-only); changing type serialization recompiles everything.
- **WireCodec version negotiation**: simple string comparison `version >= "2026-07-28"` — version IDs are ISO date strings.

## Architecture

```
include/mcp/       — Public headers
src/client/        — McpClient, OAuth, MRTR helper
src/server/        — McpServer, Extension framework, FileTaskStore
src/protocol/      — McpSessionHandler (engine), WireCodec, cache
src/transport/     — Stdio, SSE, InMemory, WebSocket transport impls
src/http/          — HttpServer, EventStore, StreamableHttp*
tests/             — unit/ (gtest), integration/, conformance/ (122 tests)
examples/          — EchoServer, WeatherServer, SimpleClient
cmake/             — Build modules: auto-detect compiler/hardware/cache/LTO
```

Library dependency chain: `mcp-core` (INTERFACE) → `mcp-transport` → `mcp-protocol` → `mcp-server | mcp-client`. `mcp-http` depends on `mcp-transport`.

### Transport hierarchy

```
ITransport —— TransportBase (3-state: Initial→Connected→Disconnected)
  ├── StdioServerTransport
  ├── InMemoryTransportImpl
  ├── StreamableHttpServerTransport (+ IStatelessTransport)
  └── WebSocketTransport

IClientTransport (connection factory)
  ├── StdioClientTransport
  ├── SseClientTransport
  ├── StreamableHttpClientTransport
  └── WebSocketClientTransport
```

New code use `ITransport`/`TransportBase`/`IClientTransport`. Old `Transport`/`ClientTransport` in `Transport.hpp` are deprecated.

### Protocol engine

`McpSessionHandler` (in `mcp-protocol`) is the JSON-RPC engine:
- Async message loop over `MessageChannel` (wraps `asio::experimental::channel`)
- Request/response correlation with timeout
- Handler dispatch via `unordered_map` (not virtual dispatch)
- Dual-era `WireCodec` (2025-11-25 initialize handshake / 2026-07-28 per-request `_meta`)
- Inbound/outbound `MessageFilter` pipeline

## Coding Style

- **C++17**, `#pragma once`, 4-space indent
- **Types/Functions**: PascalCase (`WireCodec`, `MakeWireCodec`)
- **Constants**: `k` + PascalCase (`kLatestProtocolVersion`)
- **Members**: snake_case + underscore (`io_ctx_`)
- **Namespace**: flat `mcp`. Sub-namespaces for constants: `mcp::methods`, `mcp::notifications`, `mcp::headers`
- **Compiler warnings**: `/W4` on MSVC/clang-cl, `-Wall -Wextra -Wpedantic` on Clang/GCC; `-Wno-unused-parameter` allowed
- **Includes**: `<mcp/McpCore.hpp>` (umbrella), `<mcp/server/McpServer.hpp>`, `<mcp/transport/StdioServerTransport.hpp>`
- No auto-formatter; follow existing file style.

## Testing

**216 tests** — Google Test v1.15.2 (auto-fetched).

| Suite | Location | Purpose |
|-------|----------|---------|
| `JsonRpcTest` | `tests/unit/` | Message serialization + variant dispatch |
| `McpTypesTest` | `tests/unit/` | Type round-trips |
| `WireCodecTest` | `tests/unit/` | Era-gating codec |
| `McpServerTest` | `tests/unit/` | Registration, capabilities |
| `McpClientTest` | `tests/unit/` | Client creation, tool cache |
| `OAuthTest` | `tests/unit/` | PKCE, token cache |
| `TransportTest` | `tests/unit/` | InMemory transport |
| `Conformance` | `tests/conformance/` | 122 MCP spec compliance tests |
| `Integration` | `tests/integration/` | Client-server round-trip |

## Traps

- **`McpServer::Create` / `McpClient::Create` take `shared_ptr<ITransport>`**: When using `StdioServerTransport`, both must share the same `asio::io_context`. Pass `McpServer::Create(transport, opts, &io_ctx)`. Omitting creates an internal io_context — causes silent data loss.
- **`Protocol.hpp` name collision**: `include/mcp/Protocol.hpp` redirects to `include/mcp/protocol/Protocol.hpp`. Prefer `<mcp/protocol/McpSessionHandler.hpp>`.
- **All types in `McpTypes.hpp`**: `Result.hpp`, `Progress.hpp`, etc. were consolidated; everything lives in `McpTypes.hpp`.
- **Don't delete `build/`**: Dependencies cached in `build/<preset>/_deps/`. Deleting forces re-download of asio, nlohmann-json, googletest.

## Dependencies

All fetched automatically via `FetchContent`:

| Dep | Version | Notes |
|-----|---------|-------|
| asio | 1.30.2 | Header-only; manual INTERFACE target (no upstream CMakeLists.txt) |
| nlohmann-json | 3.11.3 | SYSTEM, shallow fetch |
| GoogleTest | 1.15.2 | Only when `MCP_BUILD_TESTS=ON` |
| OpenSSL | system | Optional; OAuth PKCE (`MCP_HAVE_OPENSSL`) |

## Commits

Conventional Commits with scope: `feat(transport):`, `fix(server):`, etc. Scopes: `client`, `server`, `protocol`, `transport`, `http`, `core`, `build`, `test`, `examples`.
