# MCP C++ SDK

> **中文版文档**：[README_zh.md](README_zh.md)

C++17 implementation of the [Model Context Protocol (MCP)](https://modelcontextprotocol.io), providing client and server libraries for MCP-based AI tooling integrations. All libraries are static.

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platform](https://img.shields.io/badge/platform-windows%20%7C%20linux%20%7C%20macos-lightgrey.svg)

**Status**: 0.3.11 (`mcp::kSdkVersion`), five protocol versions in two eras, 124 conformance tests. Supported platforms: Windows (MSVC, clang-cl), Linux (GCC, Clang), macOS (Clang).

## Requirements

| Dependency | Minimum | Notes |
|---|---|---|
| CMake | 3.28 | Ninja generator required |
| C++ compiler | C++17 | MSVC, Clang, GCC |
| OpenSSL | optional | `MCP_WITH_OPENSSL=AUTO`/`ON`/`OFF` (default `AUTO`). Required for TLS (WebSocket, SSE HTTPS); OAuth PKCE works without it (built-in SHA-256 fallback). Supports 3.x/4.x |

## Quick Start

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Presets: `debug`, `release`. Examples need `-DMCP_BUILD_EXAMPLES=ON`; the official conformance fixture needs `-DMCP_BUILD_CONFORMANCE=ON`.

## Installation

```cmake
include(FetchContent)
FetchContent_Declare(
    mcp-cpp-sdk
    GIT_REPOSITORY https://github.com/jesspig/modelcontextprotocol-cpp-sdk.git
    GIT_TAG        0.3.11
)
FetchContent_MakeAvailable(mcp-cpp-sdk)

target_link_libraries(your_target PRIVATE mcp-client mcp-server)
```

Library targets: `mcp-core`, `mcp-transport`, `mcp-protocol`, `mcp-server`, `mcp-client`, `mcp-http`, `mcp-ext`; all static. PUBLIC dependencies propagate, so linking a façade is enough. `find_package`, install layout and every build switch: [Installation](docs/en/start/installation.md).

## Documentation

The manual is published at <https://jesspig.github.io/modelcontextprotocol-cpp-sdk/> and lives in `docs/en` (Chinese mirror in `docs/zh`). It is organised in three layers: concepts, task-shaped how-tos, and an API reference.

- [Quick start](docs/en/start/quickstart.md) — a working server and client in five minutes, with expected output.
- [Installation](docs/en/start/installation.md) — dependencies, build options, CMake integration, optional OpenSSL.
- [Architecture](docs/en/learn/architecture.md) — library layers, threading model, capabilities derived from registration.
- [Transports](docs/en/learn/transports.md) — stdio, Streamable HTTP, SSE, WebSocket, InMemory; stateful vs stateless.
- [Protocol versions](docs/en/learn/protocol-versions.md) — the authoritative dual-era matrix, connect modes and the 2026-07-28 breaking changes.
- [Register a tool](docs/en/howto/register-a-tool.md) — descriptions, input schema, annotations, structured output.
- [Connect to a server](docs/en/howto/connect-to-a-server.md) — transports, connect modes, close timing.
- [Run a Streamable HTTP server](docs/en/howto/run-a-streamable-http-server.md) and [protect it](docs/en/howto/protect-an-http-server.md).
- [Add OAuth to a client](docs/en/howto/add-oauth-to-a-client.md) — PKCE, CIMD/DCR, token cache, DPoP wiring.
- [Handle errors](docs/en/howto/handle-errors.md) — the three failure channels and error-code remapping; [debug a stuck server](docs/en/howto/debug-a-stuck-server.md).
- [API reference](docs/en/reference/index.md) — tables only: signatures, defaults, throws, source files.
- [Extensions](docs/en/learn/extensions.md) — the `enable_*` switch family and what reaches the wire.
- [SDK comparison](docs/en/learn/what-is-mcp.md) — SDK tiers and the Ruby SDK cross-check.
- [Deprecation registry](docs/en/learn/compatibility.md), [0.3.9 migration](docs/en/migration/0.3.9.md), [0.3.10](docs/en/migration/0.3.10.md) and [0.3.11](docs/en/migration/0.3.11.md) release notes.

All C++ samples come from [`docs/snippets`](docs/snippets), which is compiled by the `mcp-docs-snippets` target in CI, so a renamed or changed API fails the docs build instead of silently rotting the manual.

## Examples

Runnable examples in [`examples/`](examples/):

| Example | Description |
|---|---|
| [EchoServer](examples/EchoServer/) | Minimal server with tool, resource, and prompt |
| [WeatherServer](examples/WeatherServer/) | Server with external API integration |
| [SimpleClient](examples/SimpleClient/) | Client that connects to a server in-process |

```bash
cmake --preset debug -DMCP_BUILD_EXAMPLES=ON
cmake --build --preset debug
build/debug/examples/EchoServer/EchoServer
```

## Verification

- `ctest --preset debug --output-on-failure` runs the full unit and integration suite.
- **124 conformance tests** cover JSON-RPC round-trips, WireCodec era gating, tool/resource/prompt serialization, content dispatch, elicitation and MRTR, structured meta, extensions, tasks, logging, pagination and caching.
- `CONFORMANCE_SPEC_VERSION=2026-07-28 ./scripts/run-conformance.sh server` runs the official referee against the fixture (configure with `-DMCP_BUILD_CONFORMANCE=ON`).

## Non-standard Extensions

ETag conditional requests, progressive discovery (`SearchTools`) and server cards are SDK extensions, not part of the 2026 core specification. Each is opt-in and degrades to the baseline path when a peer does not implement it. Switches, defaults and wire identifiers: [Extensions](docs/en/learn/extensions.md).

## Unsupported Declarations

Apps UI rendering, full sampling, EMA, client-side per-request DPoP proofs, the `jwt-bearer` grant and resumable file uploads are explicitly out of scope; when a peer uses them, the SDK degrades gracefully instead of failing the session. Details: [What this SDK does not do](docs/en/learn/what-is-mcp.md).

## License

MIT — see [LICENSE](LICENSE).
