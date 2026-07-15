# Repository Guidelines

## Build & Test

```bash
cmake --preset debug                          # Configure (Ninja, Debug)
cmake --build --preset debug                  # Build
ctest --preset debug --output-on-failure      # Run all 219 tests
ctest -R WireCodec                            # Filter single suite
ctest -R "Conformance" -V                     # Filter group with verbose
```

Presets: `debug`, `release`. Ninja generator only.

### Non-obvious build facts

- **Unity (jumbo) builds ON** by default (multi-core). Override `-DMCP_UNITY_BUILD=OFF`. `mcp-client` sets `UNITY_BUILD_UNIQUE_ID ON` to avoid OAuth symbol clashes.
- **Ninja job pools**: compile `min(mem/1500MB, cores-2)`, link `min(mem/4000MB, 2)`. Auto-computed. Override via `MCP_COMPILE_JOBS` / `MCP_LINK_JOBS`.
- **Compiler auto-detection priority**: clang-cl (if found) → system default. sccache → ccache → none.
- **lld-link**: auto-detected on Windows MSVC + Ninja; `/lldlink` added automatically.
- **Out-of-source libs**: `mcp-transport` links `winhttp` on Windows only. `mcp-http` also links `winhttp` when WIN32 (StreamableHttpClientTransport).
- **Dependencies cached per build dir** in `build/<preset>/_deps/`. Do NOT delete `build/` — re-downloading dependencies is expensive.
- `mcp-core` is INTERFACE (header-only); changing type serialization recompiles everything.
- **WireCodec version negotiation**: simple string comparison `version >= "2026-07-28"` — version IDs are ISO date strings.
- **`-DMCP_WERROR=ON`**: applies `/WX` (MSVC) or `-Werror` (GCC/Clang) globally via `mcp-core INTERFACE`. Off by default.
- **`MCP_API`** (`include/mcp/Export.hpp`): no-op in static builds; marks classes for future DLL/so export when switching to shared libraries.

### PlatformIO cross-platform helpers

`include/mcp/transport/detail/PlatformIO.hpp` provides OS-agnostic wrappers:

- `native_handle()` — get `PipeHandle`/`ProcessHandle` for async I/O
- `WaitForData()` — block on pipe readability (poll/PipeHandle)
- `OpenStandardInput/Output/Error()` — cross-platform stdio handles
- `SetThreadName()` — set thread name for debugging

Implementations: `win32_platform.cpp` (WaitForSingleObject, SetThreadDescription), `posix_platform.cpp` (poll, pthread_setname_np).

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
  ├── WebSocketTransport
  └── StreamableHttpSessionTransport (internal, via StreamableHttpClientTransport)

IClientTransport (connection factory)
  ├── StdioClientTransport
  ├── SseClientTransport
  ├── StreamableHttpClientTransport (Windows-only; CMake if(WIN32) + winhttp)
  └── WebSocketClientTransport (supports wss:// with OpenSSL)
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

**219 tests** — Google Test v1.15.2 (auto-fetched).

| Suite | CMake Target | Location | Purpose |
|-------|-------------|----------|---------|
| `JsonRpcTest` | `mcp-core-tests` | `tests/unit/` | Message serialization + variant dispatch |
| `McpTypesTest` | `mcp-core-tests` | `tests/unit/` | Type round-trips |
| `WireCodecTest` | `mcp-wire-codec-tests` | `tests/unit/` | Era-gating codec |
| `McpServerTest` | `mcp-server-tests` | `tests/unit/` | Registration, capabilities |
| `McpClientTest` | `mcp-client-tests` | `tests/unit/` | Client creation, tool cache |
| `OAuthTest` | `mcp-oauth-tests` | `tests/unit/` | PKCE, token cache |
| `TransportTest` | `mcp-transport-tests` | `tests/unit/` | InMemory transport, state machine, error propagation |
| `Conformance` | `mcp-conformance-tests` | `tests/conformance/` | 122 MCP spec compliance tests |
| `Integration` | `mcp-integration-tests` | `tests/integration/` | Client-server round-trip |

Run a single target: `cmake --build --preset debug --target mcp-core-tests`

### Test notes

- `TransportTest` uses `dynamic_cast<TransportBase*>` on `InMemoryTransport::CreatePair()` returned `ITransport` pointers to test state machine and error propagation directly.
- `InMemoryTransport` is synchronous — messages are delivered when io_context runs (not on SendMessageAsync).

## CI

CI (`ci.yml`) runs on `develop` branch only — both push and PR. Docs (`docs.yml`) deploys from `master`. Examples built and tested by default in debug preset.

## Traps

- **`McpServer::Create` / `McpClient::Create` take `shared_ptr<ITransport>`**: When using `StdioServerTransport`, both must share the same `asio::io_context`. Pass `McpServer::Create(transport, opts, &io_ctx)`. Omitting creates an internal io_context — causes silent data loss.
- **`Protocol.hpp` name collision**: `include/mcp/Protocol.hpp` redirects to `include/mcp/protocol/Protocol.hpp`. Prefer `<mcp/protocol/McpSessionHandler.hpp>`.
- **All types in `McpTypes.hpp`**: `Result.hpp`, `Progress.hpp`, etc. were consolidated; everything lives in `McpTypes.hpp`.
- **Don't delete `build/`**: Dependencies cached in `build/<preset>/_deps/`. Deleting forces re-download of asio, nlohmann-json, googletest.
- **`StreamableHttpClientTransport.cpp` is Windows-only**: guarded with `#ifdef _WIN32` and CMake `if(WIN32)`. Requires `winhttp.h`.
- **`InMemoryTransport::CreatePair()`** returns `std::shared_ptr<ITransport>`, not the concrete `InMemoryTransportImpl`. Use `dynamic_cast<TransportBase*>` to access state machine (GetState, SetOnClose, etc.).

## Dependencies

All fetched automatically via `FetchContent`:

| Dep | Version | Notes |
|-----|---------|-------|
| asio | 1.30.2 | Header-only; manual INTERFACE target (no upstream CMakeLists.txt) |
| nlohmann-json | 3.11.3 | SYSTEM, shallow fetch |
| GoogleTest | 1.15.2 | Only when `MCP_BUILD_TESTS=ON` |
| OpenSSL | system | Optional; OAuth PKCE + WebSocket wss:// (`MCP_HAVE_OPENSSL`) |

## Commits

Conventional Commits with scope: `feat(transport):`, `fix(server):`, etc. Scopes: `client`, `server`, `protocol`, `transport`, `http`, `core`, `build`, `test`, `examples`.
