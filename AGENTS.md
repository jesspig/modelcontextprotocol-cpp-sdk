# Repository Guidelines

## Build & Test

```bash
cmake --preset debug                          # Configure (Ninja, Debug)
cmake --build --preset debug                  # Build
ctest --preset debug --output-on-failure      # Run all tests
ctest -R WireCodec                            # Filter single suite
ctest -R Conformance -V                       # Filter group with verbose
cmake --build --preset debug --target mcp-server-tests   # Single target
```

Presets: `debug`, `release`. Ninja generator only.

### Non-obvious build facts

- **Unity (jumbo) builds ON** by default. Override `-DMCP_UNITY_BUILD=OFF`. `mcp-client` uses `UNITY_BUILD_UNIQUE_ID ON` to avoid OAuth symbol clashes.
- **Ninja job pools** + **Unity batch size**: auto-computed from CPU/memory. Override via `MCP_COMPILE_JOBS`, `MCP_LINK_JOBS`, `MCP_UNITY_BATCH_SIZE`, `MCP_MAX_COMPILE_MEM_MB`, `MCP_MAX_LINK_MEM_MB`, `MCP_UNITY_MEM_MB`.
- **Compiler auto-detection**: `clang-cl` (Win, VS or standalone LLVM) > `clang++-N` + matching `clang-N` (Linux) > system default. Version fallback chain: `clang++-19` → `-18` → `-17` → `-16` → `clang++`. Detected before `project()`, skips if `CMAKE_CXX_COMPILER` already set.
- **LTO**: auto-enabled in Release builds. Clang: ThinLTO (`-flto=thin`). MSVC: LTCG (`/GL` + `/LTCG`). GCC: IPO (if supported).
- **Compiler cache**: sccache > ccache > none. Auto-detected. sccache supports MSVC/clang-cl; ccache skips MSVC.
- **lld-link**: auto-detected on Windows MSVC cl.exe + Ninja; adds `/lldlink`. clang-cl already bundles lld.
- **Dependencies cached in `build/<preset>/_deps/`**. Deleting `build/` is expensive.
- **`mcp-core` is INTERFACE (header-only)**. Changing type serialization recompiles everything.
- **Werror**: only with `-DMCP_WERROR=ON` (CI does this). Off by default for local builds.
- **CI** (`ci.yml`) runs on `develop` branch only. Matrix: windows-2022, ubuntu-24.04, macos-15 × debug/release. All with `-DMCP_WERROR=ON`.

### Cross-platform traps

- **macOS linker**: `environ` in C++ anonymous namespace creates mangled symbol `mcp::detail::environ`. Use `_NSGetEnviron()` (Apple's API via `<crt_externs.h>`) instead of `extern char** environ`.
- **macOS `pthread_setname_np`**: single-arg (`pthread_setname_np(name)`) on Apple, two-arg (`pthread_setname_np(pthread_self(), name)`) elsewhere. Guard with `#ifdef __APPLE__`.
- **GCC `warn_unused_result`**: `chdir()` return value must be checked or assigned, not `(void)`-cast. `(void)` cast only works on Clang.
- **CMake version**: `CMP0169` guarded with `if(POLICY CMP0169)` — not available before CMake 3.30.
- **WSL**: build directory cache is **not cross-platform**. Windows clang-cl cache (`build/debug/`) is incompatible with WSL Linux builds. Use separate build dirs or delete `build/` when switching.
- **`McpServer::Create`/`McpClient::Create`** take `shared_ptr<ITransport>`. When using `StdioServerTransport`, both must share the same `asio::io_context`. Pass `McpServer::Create(transport, opts, &io_ctx)`. Omitting creates an internal io_context — silent data loss.
- **`InMemoryTransport::CreatePair()`** returns `shared_ptr<ITransport>`, not concrete type. Use `dynamic_cast<TransportBase*>` to access state machine.

### Platform-specific traps

**Apple Clang vs LLVM Clang (macOS):**
- Apple Clang is **not** the same as upstream LLVM Clang. Version numbers differ (e.g., Apple Clang 15 is based on LLVM 16). No `clang++-18` versioned binary exists on macOS — only plain `clang++`. The auto-detector skips versioned name search on macOS.
- `_NSGetEnviron()` from `<crt_externs.h>` is Apple's documented way to access the environment in Mach-O binaries. Plain `extern char** environ` creates a C++ mangled symbol in anonymous namespaces that the linker can't resolve.
- `pthread_setname_np` is single-argument on Apple (`pthread_setname_np(name)`) vs two-argument on Linux (`pthread_setname_np(pthread_self(), name)`).
- Apple Clang enables `-Wunused-private-field` by default, which combined with `-Werror` (CI) turns any unused private member into a hard error. GCC does not warn on unused private fields.

**GCC (Ubuntu):**
- GCC's `warn_unused_result` is stricter than Clang's. A `(void)` cast **does not suppress it** — you must assign the return value or check it in a condition. This affects `chdir()`, `close()`, `dup2()`, and `pipe()`.
- Unity batch files may trigger `-Wunused-function` on GCC when anonymous namespace functions are included but not used in a particular batch — guard with `[[maybe_unused]]`.
- GCC does not support ThinLTO (`-flto=thin`). Falls back to IPO via `check_ipo_supported()`.

**Windows (clang-cl / MSVC):**
- `_WIN32_WINNT=0x0A00` is required because asio's Windows backend (`win32_basic_overlapped_dns`) references `GetAddrInfoW` which needs Windows 10+ API level.
- clang-cl silently accepts both MSVC-style flags (`/W4`, `/EHsc`) and GCC-style flags (`-Wall`). This means a typo like `-Wsomthing` won't error — it just passes through.
- **sccache** supports MSVC and clang-cl; **ccache** does NOT support MSVC. On Windows, only sccache works for compiler caching.
- WinHTTP vs asio sockets: HTTP client code uses WinHTTP on Windows and asio native sockets on POSIX. Both are in the same `.cpp` via `#ifdef WIN32`.
- `clang-cl` on Windows uses `/link` to pass linker flags, not `-Wl,`. The cmake build handles this automatically.

**Compiler cache:**
- sccache supports all three toolchains (Clang, MSVC, GCC). ccache only supports GCC/Clang. The build system prefers sccache > ccache.
- sccache with ThinLTO: generates LLVM bitcode instead of native object files. Works but may hit cache size limits on large projects.
- On CI, sccache is configured with `~/.cache/sccache` (Linux/macOS) or `C:\Users\runneradmin\AppData\Local\Mozilla\sccache` (Windows).

### Unity (jumbo) build traps

- **Header self-containment is mandatory**: Unity files include multiple `.cpp` files in a single translation unit. If a header doesn't include its own dependencies (relying on a prior `#include` in the `.cpp` file), the Unity build breaks with "not declared in this scope" errors. Every `.hpp` must be compilable in isolation.
- **`mcp-client` requires `UNITY_BUILD_UNIQUE_ID ON`**: The OAuth client provider and other components may define identically-named static/anonymous-namespace symbols across TUs. Without unique IDs, linking fails with duplicate symbol errors.
- **Debugging is harder**: Error line numbers point to the generated Unity batch file, not the original source. Add `#line` directives or disable Unity with `-DMCP_UNITY_BUILD=OFF` when debugging.
- **Batch size formula**: `min(mem / 500MB, cores/2)`. Minimum batch size is 2. Single-core machines disable Unity entirely.
- **Memory impact**: Each Unity batch file consumes `MCP_UNITY_MEM_MB` (default 500 MB) of memory during compilation. Override if hitting OOM with `-DMCP_UNITY_MEM_MB=300`.
- **Werror in Unity**: A warning in any included `.cpp` file fails the entire Unity batch. The specific offending source is not obvious from the batch file name — check the batch file content at `build/<preset>/CMakeFiles/<target>/Unity/unity_<N>_cxx.cxx`.

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

Library dep chain: `mcp-core` (INTERFACE) → `mcp-transport` → `mcp-protocol` → `mcp-server | mcp-client`. `mcp-http` depends on `mcp-transport`.

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

`StreamableHttpClientTransport.cpp` compiles on all platforms: Win32 uses WinHTTP, POSIX uses asio native sockets.

### Protocol engine

`McpSessionHandler` (in `mcp-protocol`) is the JSON-RPC engine:
- Async message loop over `MessageChannel` (wraps `asio::experimental::channel`)
- Request/response correlation with timeout; `next_request_id_` is `std::atomic<int64_t>`
- Handler dispatch via `unordered_map`
- Dual-era `WireCodec` (2025-11-25 initialize handshake / 2026-07-28 per-request `_meta`)

## Key protocol patterns (2026-07-28 era)

- **Stateless**: `initialize`/`initialized` handshake replaced by `server/discover`. Per-request `_meta` carries `protocolVersion`, `clientInfo`, `clientCapabilities` on every C→S request.
- **MRTR**: Server-initiated interactions (elicitation) embedded as `InputRequiredResult`. Client retries with `inputResponses` + `requestState`.
- **Subscriptions**: `subscriptions/listen` replaces `resources/subscribe`. Opt-in to change notification types.
- **Extensions**: Negotiated via `map<string, json>` on `ClientCapabilities`/`ServerCapabilities`.
- **Caching**: `CacheHint` with `ttlMs`/`cacheScope` on list/discover/read results.
- **Mcp-Method header**: Dynamic, derived from JSON-RPC body method field.

## Coding Style

- **C++17**, `#pragma once`, 4-space indent. No auto-formatter.
- **Types/Functions**: PascalCase. Constants: `k` + PascalCase.
- **Members**: snake_case + underscore (`io_ctx_`).
- **Namespace**: flat `mcp`. Sub-namespaces: `mcp::methods`, `mcp::notifications`.
- **Includes**: `<mcp/McpCore.hpp>` (umbrella) or per-module headers like `<mcp/server/McpServer.hpp>`.
- **All protocol types in `McpTypes.hpp`**: Do not create new type headers.
- **Content annotations**: typed `Annotations` struct (`audience`, `priority`, `lastModified`), not raw JSON.
- **`Prompt` has no `annotations` field** — per spec. Do not add.
- **`ExtensionsCapability` removed**: replaced by `map<string, json>` on both `ClientCapabilities`/`ServerCapabilities`.
- **Server guards requests with `initialized_` flag**: All handlers reject with `InvalidRequest` until `notifications/initialized` received.
- **`McpClient` sends `notifications/initialized` after `HandshakeInitialize`**: Required by 2025-era protocol.
- **`Icon::mime_type` is `optional<string>`** (not required per spec).
- **`ContentVariant` includes `ResourceLink`**: handle `type == "resource_link"` in dispatch.
- **`ErrorCodes.hpp`**: fine-grained codes like `DeserializeFailed`, `ConnectionRefused`, `TlsHandshakeFailed`, `ProtocolViolation`, `TaskNotFound`, `HandlerError` in addition to JSON-RPC standard codes. Integrates with `std::error_code`.
- **`JsonRpcErrorResponse::id` is `optional<RequestId>`**: Null-id errors serialize correctly per JSON-RPC 2.0 §5.1.
- **`jsonrpc: "2.0"` validated** in all `from_json`; invalid version throws `std::runtime_error`.
- **Log levels via `MCP_LOG_LEVEL` env var**: 0=Off (default), 1=Error, 2=Warning, 3=Info, 4=Debug, 5=Trace. Macros: `MCP_LOG(Error, msg)`, `MCP_BUG(msg)`, `MCP_LOG_CTX(LEVEL, ctx, msg)`.
- **OAuth HTTP/1.1 uses `Connection: close`**: Each token exchange opens a new TCP connection. No keep-alive.

## Testing (Google Test, auto-fetched)

| Suite | Target | Notes |
|-------|--------|-------|
| `JsonRpcTest` | `mcp-core-tests` | Message serialization + variant dispatch |
| `McpTypesTest` | `mcp-core-tests` | Type round-trips with annotations, icons, content variants |
| `WireCodecTest` | `mcp-wire-codec-tests` | Era-gating codec |
| `McpServerTest` | `mcp-server-tests` | Registration, capabilities |
| `McpClientTest` | `mcp-client-tests` | Client creation, tool cache |
| `OAuthTest` | `mcp-oauth-tests` | PKCE, token cache; SHA-256 uses built-in fallback |
| `TransportTest` | `mcp-transport-tests` | State machine via `dynamic_cast<TransportBase*>` |
| `Conformance` | `mcp-conformance-tests` | MCP spec compliance tests (122+) |
| `Integration` | `mcp-integration-tests` | Client-server round-trip via InMemoryTransport |

- `InMemoryTransport` is synchronous — messages delivered when `io_context` runs, not on `SendMessageAsync`.
- All tests compile and pass without OpenSSL. TLS features (WebSocket, SSE HTTPS, OAuth) require OpenSSL at build time.

## Dependencies (auto-fetched via FetchContent)

| Dep | Version | Notes |
|-----|---------|-------|
| asio | 1.30.2 | Header-only; manual INTERFACE target (no upstream CMakeLists.txt) |
| nlohmann-json | 3.11.3 | SYSTEM, shallow fetch |
| GoogleTest | 1.15.2 | Only when `MCP_BUILD_TESTS=ON` |
| OpenSSL | system | Optional; TLS (WebSocket, SSE HTTPS, OAuth). PKCE SHA-256 falls back to built-in. Install: `vcpkg install openssl` / `apt install libssl-dev` / `brew install openssl` |

## Commits

Conventional Commits with scope: `feat(transport):`, `fix(server):`, etc. Scopes: `client`, `server`, `protocol`, `transport`, `http`, `core`, `build`, `test`, `examples`.
