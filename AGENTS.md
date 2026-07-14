# Repository Guidelines

## Build & Test

```bash
cmake --preset debug                          # Configure (Ninja, Debug)
cmake --build --preset debug                  # Build
ctest --preset debug --output-on-failure      # Run all 137 tests
ctest -R WireCodec                            # Filter single suite
ctest -R "Conformance.Tool" -V               # Filter specific test
```

Presets: `debug`, `release` only. Ninja generator, clang-cl auto-detected (ThinLTO on Release). No platform-specific presets needed — everything auto-detected.

### Non-obvious build facts

- **Unity (jumbo) builds ON** by default (multi-core only). Override `-DMCP_UNITY_BUILD=OFF`. `mcp-client` sets `UNITY_BUILD_UNIQUE_ID ON` to avoid OAuth symbol clashes.
- **Ninja job pools**: compile `min(mem/1500MB, cores-2)`, link `min(mem/4000MB, 2)`. Auto-computed from hardware. Override via `MCP_COMPILE_JOBS` / `MCP_LINK_JOBS`.
- **Compiler auto-detection priority**: clang-cl (if found) → system default. sccache → ccache → none. All auto-detected.
- **lld-link**: auto-detected on Windows MSVC when using Ninja generator; `/lldlink` added automatically.
- **Out-of-source libs**: `target_link_libraries(mcp-transport PUBLIC winhttp)` on Windows only.
- **WireCodec version negotiation**: simple string comparison `version >= "2026-07-28"` — version IDs are ISO date strings.
- **Dependencies cached per build dir** in `build/<preset>/_deps/`. No global `.cache/deps/`. Do NOT delete `build/` — re-downloading dependencies is expensive.
- `mcp-core` is INTERFACE (header-only); changing type serialization recompiles everything.

## Architecture

```
include/mcp/       — Public headers (PascalCase: McpClient.hpp)
src/client/        — McpClient, OAuth, MRTR helper
src/server/        — McpServer, Extension framework
src/protocol/      — McpSessionHandler (engine), WireCodec, cache
src/transport/     — StdioServer/Client, SSE Client, InMemory
src/http/          — HttpServer, EventStore, StreamableHttp*
tests/unit/        — Google Test (12 suites)
tests/integration/ — Client-server round-trip tests (enabled)
tests/conformance/ — MCP spec conformance (43 tests)
examples/          — EchoServer, WeatherServer, SimpleClient
cmake/             — Build modules: auto-detect compiler/hardware/cache/LTO
```

Library dependency chain: `mcp-core` (header-only INTERFACE) → `mcp-transport` → `mcp-protocol` → `mcp-server | mcp-client`. `mcp-http` depends on `mcp-transport`.

### Transport hierarchy

```
ITransport (interface)
  ├── TransportBase (3-state machine: Initial→Connected→Disconnected)
  │   ├── StdioServerTransport
  │   ├── InMemoryTransportImpl
  │   ├── StreamableHttpServerTransport
  │   └── ...session transports
  └── Transport (backward-compatible, provides Start()/IoContext())

IClientTransport (connection factory interface)
  ├── StdioClientTransport
  ├── SseClientTransport
  └── StreamableHttpClientTransport
```

New code should use `ITransport`/`TransportBase`/`IClientTransport`. The old `Transport`/`ClientTransport` base classes exist in `Transport.hpp` for backward compat but are deprecated.

### Protocol engine

`McpSessionHandler` (in `mcp-protocol`) is the core JSON-RPC engine. It manages:
- Async message loop over `MessageChannel` (wraps `asio::experimental::channel`)
- Request/response correlation with timeout
- Handler dispatch via registered maps
- `_meta` per-request metadata injection/extraction (2026-era)
- Inbound/outbound `MessageFilter` pipeline

## Traps

- **`McpServer::Create` / `McpClient::Create` take `shared_ptr<ITransport>`**: When using `StdioServerTransport`, both the transport and server/client must use the same `asio::io_context`. Pass the third argument: `McpServer::Create(transport, opts, &io_ctx)`. Omitting it creates an internal io_context that won't be shared with the transport, causing silent data loss on the channel.
- **`McpServerTool::InvokeAsync` runs synchronously**: It calls the callable and sets the promise on the calling thread. Async dispatch happens in `McpSessionHandler::OnRequest` via `asio::post`.
- **`HttpTransportMode` defined twice**: in `include/mcp/transport/HttpTransportMode.hpp` AND duplicated inside `StreamableHttpClientTransport.hpp`. Both define `enum class HttpTransportMode` with the same values. Changing one without the other causes ODR violations.
- **`Protocol.hpp` name collision**: `include/mcp/Protocol.hpp` now redirects to `include/mcp/protocol/Protocol.hpp`. For new code, prefer `<mcp/protocol/McpSessionHandler.hpp>`.
- **All types in `McpTypes.hpp`**: `Result.hpp`, `Progress.hpp`, `Notifications.hpp` were deleted; everything is in `McpTypes.hpp`.
- **Don't delete `build/`**: Dependencies are cached in `build/<preset>/_deps/`. Deleting build forces re-download of asio, nlohmann-json, googletest — expensive on slow networks.

## Coding Style

- **C++17**, `#pragma once`, 4-space indentation
- **Types/Functions**: PascalCase (`WireCodec`, `MakeWireCodec`)
- **Constants**: `k` + PascalCase (`kLatestProtocolVersion`)
- **Members**: snake_case + underscore (`io_ctx_`)
- **Namespace**: flat `mcp`. Sub-namespaces for constants: `mcp::methods`, `mcp::notifications`, `mcp::headers`
- **Compiler warnings**: `/W4` on MSVC/clang-cl, `-Wall -Wextra -Wpedantic` on Clang/GCC; `-Wno-unused-parameter` allowed
- **Includes**: `<mcp/McpCore.hpp>` (umbrella), `<mcp/server/McpServer.hpp>`, `<mcp/transport/StdioServerTransport.hpp>`
- No auto-formatter; follow existing file style.

## Testing

Tests use **Google Test v1.15.2** (auto-fetched). **137 tests** across 12 suites:

| Suite | File | Count | Notes |
|-------|------|-------|-------|
| `JsonRpcTest` | `JsonRpcTests.cpp` | 12 | Message serialization + variant dispatch |
| `McpTypesTest` | `McpTypesTests.cpp` | 26 | Type round-trips |
| `WireCodecTest` | `WireCodecTests.cpp` | 13 | Era-gating codec |
| `McpServerTest` | `McpServerTests.cpp` | 9 | Registration, capabilities |
| `McpClientTest` | `McpClientTests.cpp` | 10 | Client creation, tool cache |
| `OAuthTest` | `OAuthTests.cpp` | 15 | PKCE, token cache |
| `TransportTest` | `TransportTests.cpp` | 2 | InMemory transport |
| `Conformance` | `ProtocolConformance.cpp` | 43 | Spec compliance |
| `Integration` | `ClientServerRoundTrip.cpp` | 7+ | Client-server round-trip |

- Client tests link **both** mcp-client and mcp-server.
- All tests are synchronous and self-contained (inline `nlohmann::json`, no external fixtures).
- New features need matching `tests/unit/` tests; transport changes should add integration tests.

## Dependencies

All fetched automatically via `FetchContent`, cached per build directory:

| Dep | Version | Notes |
|-----|---------|-------|
| asio | 1.30.2 | Header-only; manual INTERFACE target (no upstream CMakeLists.txt) |
| nlohmann-json | 3.11.3 | SYSTEM, fetched shallow |
| GoogleTest | 1.15.2 | Only when `MCP_BUILD_TESTS=ON` |
| OpenSSL | system | Optional; enables OAuth PKCE (`MCP_HAVE_OPENSSL`) |

## Commits

Conventional Commits with scope: `feat(transport):`, `fix(server):`, etc. Scopes: `client`, `server`, `protocol`, `transport`, `http`, `core`, `build`, `test`, `examples`. PRs target `develop`.
