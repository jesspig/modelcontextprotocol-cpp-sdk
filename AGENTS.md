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

- **Unity (jumbo) builds ON** by default. Override `-DMCP_UNITY_BUILD=OFF`. `mcp-client` uses `UNITY_BUILD_UNIQUE_ID ON` to avoid OAuth symbol clashes. Exceptions: `mcp-transport` and `mcp-protocol` have Unity explicitly disabled (anonymous namespace symbols clash).
- **Ninja job pools + Unity batch size**: auto-computed from CPU/memory. Override via `MCP_COMPILE_JOBS`, `MCP_LINK_JOBS`, `MCP_UNITY_BATCH_SIZE`.
- **Compiler auto-detection**: `clang-cl` (Win) > `clang++-N` + matching `clang-N` (Linux) > system default. Detected before `project()`, skips if `CMAKE_CXX_COMPILER` already set.
- **LTO**: auto-enabled in Release. clang-cl: `CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON` (maps to LTCG-style linking). Clang: ThinLTO. GCC: IPO.
- **Compiler cache**: sccache > ccache > none. Auto-detected. sccache supports MSVC/clang-cl; ccache skips MSVC.
- **Dependencies cached in `build/<preset>/_deps/`**. Deleting `build/` is expensive.
- **`mcp-core` is STATIC** (has JsonValue.cpp, JsonRpc.cpp, etc.) — changing serialization recompiles many dependents.
- **Werror**: only with `-DMCP_WERROR=ON` (CI). Off by default.
- **CI** (`ci.yml`): push to `develop` + PRs targeting `develop`. Matrix: windows-2022, ubuntu-24.04, macos-15 × debug/release. All with `-DMCP_WERROR=ON`.

### Cross-platform traps

- **macOS linker**: `environ` in C++ anonymous namespaces creates mangled symbol `mcp::detail::environ`. Use `_NSGetEnviron()` (`<crt_externs.h>`) instead.
- **macOS `pthread_setname_np`**: single-arg on Apple, two-arg elsewhere. Guard with `#ifdef __APPLE__`.
- **GCC `warn_unused_result`**: `(void)`-cast does **not suppress** it — only works on Clang. Affects `chdir()`, `close()`, `dup2()`, `pipe()`.
- **CMake CMP0169**: guarded with `if(POLICY CMP0169)` — not available before CMake 3.30.
- **WSL**: build cache is not cross-platform. Windows clang-cl and WSL Linux builds need separate build dirs.
- **`InMemoryTransport::CreatePair()`** returns `shared_ptr<ITransport>`, not concrete type. Use `dynamic_cast<TransportBase*>` to access state machine.

### Platform-specific traps

**Apple Clang vs LLVM Clang (macOS):**

- Version numbers differ (Apple Clang 15 is LLVM 16). No `clang++-18` binary on macOS — only plain `clang++`.
- Apple Clang enables `-Wunused-private-field` by default — with `-Werror` (CI) any unused private member is a hard error.

**GCC (Ubuntu):**

- GCC does not support ThinLTO. Falls back to IPO via `check_ipo_supported()`.
- Unity batch files may trigger `-Wunused-function` — guard anonymous namespace functions with `[[maybe_unused]]`.

**Windows (clang-cl / MSVC):**

- `_WIN32_WINNT=0x0A00` required for Windows 10+ API level.
- clang-cl silently accepts both MSVC-style (`/W4`) and GCC-style (`-Wall`) flags — typos pass through.
- sccache supports MSVC/clang-cl; ccache does NOT support MSVC.
- clang-cl uses `/link` for linker flags, not `-Wl,`.

### Unity (jumbo) build traps

- **Header self-containment is mandatory**: Unity merges `.cpp` files; any header relying on prior `#include` order breaks.
- **`mcp-client` requires `UNITY_BUILD_UNIQUE_ID ON`** to avoid OAuth static/anonymous-namespace symbol clashes.
- **Debugging**: error line numbers point to the generated Unity batch file, not the original source. Disable Unity with `-DMCP_UNITY_BUILD=OFF`.
- **Batch size formula**: `min(mem / 500MB, (cores + 1) / 2)`. Minimum batch size 2. Single-core machines disable Unity entirely.

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

Library dep chain: `mcp-core (STATIC)` → `mcp-transport` → `mcp-protocol` → `mcp-server | mcp-client`. `mcp-http` depends on `mcp-transport`.

All libraries are STATIC. No `INTERFACE` / header-only libraries.

### Transport hierarchy

```
ITransport —— TransportBase (3-state: Initial→Connected→Disconnected)
  ├── StdioServerTransport
  ├── StdioClientSessionTransport (internal, anonymous namespace)
  ├── InMemoryTransportImpl
  ├── SseClientSessionTransport (internal)
  ├── WebSocketSessionTransport (internal, wraps hv::WebSocketClient, enable_shared_from_this)
  ├── StreamableHttpServerTransport
  └── StreamableHttpSessionTransport (internal, via StreamableHttpClientTransport; Win32/WinHTTP + POSIX/libhv)

IClientTransport (connection factory)
  ├── StdioClientTransport (PlatformIO, merged Win32+POSIX)
  ├── SseClientTransport (libhv HttpClient + requests::post)
  ├── StreamableHttpClientTransport (libhv requests::post / WinHTTP)
  └── WebSocketClientTransport (libhv WebSocketClient)
```

All transports have zero dependency on `asio::io_context` — they use raw threads + stdlib primitives (mutex, condition_variable) or libhv's event loop.

### MessageChannel (custom, not asio)

`MessageChannel` (`include/mcp/protocol/MessageChannel.hpp`) is a bounded async queue built on `std::queue + mutex + condition_variable`:

- `AsyncReceive(callback)` — blocks until a message arrives or channel is closed
- `Send(msg)` — blocks if buffer full (backpressure)
- `TrySend(msg)` — non-blocking
- `Close()` — wakes all waiters
- Replaces the old `asio::experimental::channel`

### Protocol engine

`McpSessionHandler` (in `mcp-protocol`) is the JSON-RPC engine:

- Async message loop over `MessageChannel`
- Request/response correlation with timeout; `next_request_id_` is `std::atomic<int64_t>`
- Handler dispatch via `unordered_map` (two maps: one for requests, one for notifications)
- Dual-era `WireCodec` (2025-11-25 initialize handshake / 2026-07-28 per-request `_meta`)

### Version negotiation gotchas

- **`HandleInitialize` must echo the client's legacy version**: Return the version the client sent (e.g., `"2025-11-25"`). Never return `kLatestProtocolVersion` (`"2026-07-28"`) — TypeScript SDK v2 validates `result.protocolVersion` against its legacy list and throws if it doesn't match.
- **Modern versions (2026-07-28+) are NEVER negotiated via `initialize`**: Only via `server/discover`. The initialize handshake is strictly for legacy versions.
- `SetNegotiatedProtocolVersion(version)` both stores the version AND recreates the `WireCodec`.

### HttpServer

`HttpServer` (in `mcp-http`) uses libhv `HttpService` internally (PIMPL pattern):

- `SetHandler(method, path, handler)` registers handlers (GET/POST/etc.)
- SSE streaming: handler sets `resp.is_sse = true` → libhv async handler with `HttpResponseWriter`
- Connected SSE clients stored by ID; `BroadcastSse(event)` pushes to all
- `HttpServerOptions` provides optional `on_request` callback. (`on_connect` and `on_disconnect` are declared but not wired in the current implementation.)

## Key protocol patterns (2026-07-28 era)

- **Stateless**: `initialize`/`initialized` handshake replaced by `server/discover`. Per-request `_meta` carries `protocolVersion`, `clientInfo`, `clientCapabilities`, `logLevel` on every C→S request; additionally `progressToken` and `subscriptionId` extracted from incoming `_meta`.
- **MRTR**: Server-initiated interactions (elicitation) embedded as `InputRequiredResult`.
- **Subscriptions**: `subscriptions/listen` replaces `resources/subscribe`.
- **Extensions**: Negotiated via `map<string, json>` on `ClientCapabilities`/`ServerCapabilities`.
- **Caching**: `CacheHint` with `ttlMs`/`cacheScope`.
- **Mcp-Method header**: Dynamic, derived from JSON-RPC body method field (for Streamable HTTP + SSE).

## ServerOptions event hooks

`ServerOptions` exposes four layers of event callbacks, all optional:

| Layer | Callbacks | Purpose |
|-------|-----------|---------|
| **Shorthand** | `on_method_called`, `on_protocol_error` | Quick logging — method name / error message strings only |
| **Full message** | `on_request`, `on_response`, `on_error`, `on_notification` | Full JSON-RPC message bodies |
| **Server lifecycle** | `on_client_connected`, `on_initialized` | High‑level server events |
| **Transport** | `on_transport_close`, `on_transport_error` | Connection‑level events |

For fine‑grained interception (auth, audit, rate‑limiting), inject `FilterPipeline` via `incoming_filters` / `outgoing_filters`. See `MessageFilter.hpp`.

## Coding Style

- **C++17**, `#pragma once`, 4-space indent. No auto-formatter.
- **Types/Functions**: PascalCase. Constants: `k` + PascalCase.
- **Members**: snake_case + underscore (`io_ctx_`).
- **Namespace**: flat `mcp`. Sub-namespaces: `mcp::methods`, `mcp::notifications`.
- **All public types are pure C++17** (`std::variant`‑based `JsonValue`). Zero external JSON deps in public headers.
- **Content annotations**: typed `Annotations` struct (`audience`, `priority`, `last_modified`), not raw JSON.
- **`Prompt` has no `annotations` field** — per spec. Do not add.
- **Server guards requests with `initialized_` flag**: All handlers reject with `InvalidRequest` until `notifications/initialized` received.
- **`McpClient` sends `notifications/initialized` after `HandshakeInitialize`**: Required by 2025-era protocol.
- **`McpClient` connect modes**: `ClientOptions::connect_mode` controls negotiation. `Auto` (default) probes `server/discover`, falls back to `initialize`. `Legacy` forces `initialize` only. `Pin` uses a pinned version.
- **`ToolOptions::InputSchema(JsonValue s)` is required for tools with parameters**: Without it, `McpServerToolImpl` hardcodes an empty schema `{"type":"object","properties":{}}`.
- **`ContentVariant` includes `ResourceLink`**: handle `type == "resource_link"` in dispatch.
- **`JsonRpcErrorResponse::id` is `optional<RequestId>`**: Null-id errors serialize correctly per JSON-RPC 2.0 §5.1.
- **`jsonrpc: "2.0"` validated** in all `from_json`; invalid version throws.
- **Log levels via `MCP_LOG_LEVEL` env var**: 0=Off (default), 1=Error, 2=Warning, 3=Info, 4=Debug, 5=Trace.
- **OAuth HTTP/1.1 uses `Connection: close`**: Each token exchange opens a new TCP connection. No keep-alive.

## JsonValue — public JSON type

`JsonValue` (`include/mcp/JsonValue.hpp`) is a recursive `std::variant<nullptr, bool, int64_t, double, string, Array, Object>`:

- Parsing uses **simdjson DOM** (not on-demand) — internal only, not in public headers
- Serialization is a hand-written recursive `Dump()` — no rapidjson/simdjson in public headers
- Type methods: `IsNull()`, `IsBool()`, `IsInt()`, `IsDouble()`, `IsNumber()`, `IsString()`, `IsArray()`, `IsObject()`
- Access: `GetBool()`, `GetInt()`, `GetDouble()`, `GetString()`, `GetArray()`, `GetObject()` (throws on type mismatch), `Find(key)` (returns nullptr, no throw)
- Container: `Contains(key)`, `Size()`, `Empty()`, `operator[](key)`, `PushBack(val)`
- Serialization helpers in `include/mcp/JsonRpc.hpp` and `src/detail/JsonSerializer.hpp` (internal, not in public headers)

## Testing (Google Test, auto-fetched)

| Suite | Target | Notes |
|-------|--------|-------|
| `JsonRpcTest` | `mcp-core-tests` | Message serialization + variant dispatch |
| `McpTypesTest` | `mcp-core-tests` | Type round-trips with annotations, icons, content variants |
| `WireCodecTest` | `mcp-wire-codec-tests` | Era-gating codec |
| `McpServerTest` | `mcp-server-tests` | Registration, capabilities |
| `McpClientTest` | `mcp-client-tests` | Client creation, tool cache |
| `OAuthTest` | `mcp-oauth-tests` | PKCE, token cache |
| `TransportTest` | `mcp-transport-tests` | InMemory + state machine via `dynamic_cast<TransportBase*>` |
| `HttpServerTest` / `EventStoreTest` / `StreamableHttpTest` | `mcp-http-tests` | HttpServer GET/POST/404, EventStore, StreamableHttp header validation |
| `Conformance` | `mcp-conformance-tests` | MCP spec compliance tests |
| `ClientServerFixture` | `mcp-integration-tests` | Client-server round-trip via InMemoryTransport |

- `InMemoryTransport` is **synchronous** — messages deliver on `Send()` / `AsyncReceive()`, no external event loop needed.
- All tests compile and pass without OpenSSL. TLS features require OpenSSL.

## Dependencies (auto-fetched via FetchContent)

| Dep | Version | Notes |
|-----|---------|-------|
| libhv | 1.3.4 | HTTP client/server, WebSocket client, event loop; `hv_static` target |
| simdjson | 3.12.3 | JSON parsing (internal only, not in public headers) |
| GoogleTest | 1.15.2 | Only when `MCP_BUILD_TESTS=ON` |
| OpenSSL | system | Optional; TLS (WebSocket, SSE HTTPS, OAuth). PKCE SHA-256 falls back to built-in. Install: `vcpkg install openssl` / `apt install libssl-dev` / `brew install openssl` |

**No asio. No nlohmann-json.** Both have been fully removed. All JSON-RPC serialization uses `JsonValue`.

## Commits

Conventional Commits with scope: `feat(transport):`, `fix(server):`, etc. Scopes: `client`, `server`, `protocol`, `transport`, `http`, `core`, `build`, `test`, `examples`.
