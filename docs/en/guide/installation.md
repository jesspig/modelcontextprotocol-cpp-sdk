# Installation

## Requirements

| Dependency     | Minimum    | Notes                          |
|----------------|------------|--------------------------------|
| CMake          | 3.28       | Ninja generator required       |
| C++ Compiler   | C++17      | MSVC, Clang, GCC               |
| OpenSSL        | Optional (dev headers; 3.x/4.x) | Governed by `MCP_WITH_OPENSSL` (`AUTO`/`ON`/`OFF`, default `AUTO`) together with the `find_package(OpenSSL)` result: when enabled, `MCP_HAVE_OPENSSL` is defined and OpenSSL is linked for `mcp-transport`/`mcp-client`/`mcp-ext` (all applied through the single INTERFACE target `mcp-openssl`). Used for TLS in the self-hosted network stack (WebSocket, SSE HTTPS, etc.) and `RAND_bytes` for PKCE verifier generation. When disabled, TLS support is compiled out and PKCE falls back to `BCryptGenRandom` (Windows) / `std::random_device` (POSIX) with the built-in SHA-256 |

Configure requires no network access: the only external dependency, OpenSSL, is located via `find_package` on the local machine and no third-party source is fetched.

`MCP_WITH_OPENSSL` semantics: `AUTO` (default — enabled when `find_package(OpenSSL)` finds it), `ON` (force-enabled; configure fails when OpenSSL is not found), `OFF` (force-disabled — `MCP_HAVE_OPENSSL` is not defined and OpenSSL is not linked even when installed); the build system no longer sniffs headers via `__has_include` to infer OpenSSL availability.

### OpenSSL support matrix

| Version | Status | Notes |
| --- | --- | --- |
| 3.0.x | Supported | Ubuntu 24.04 `libssl-dev`; security patches maintained by the distribution |
| 3.5.x LTS | Recommended | Upstream support until 2030-04-08 |
| 3.6.x | Supported | Non-LTS |
| 4.0.x | Supported | Non-LTS, EOL 2027-05-14 |
| 4.1.x | Supported | Non-LTS, EOL 2027-11-23; current Homebrew `openssl@4` |
| 4.2.x LTS | Planned | Planned for 2027-04 |

When OpenSSL is found but its major version is outside 3/4, configure fails with the supported range instead of silently disabling TLS. Patch updates within a major version come from the system package manager; major upgrades are scheduled work that starts with the weekly canary, then updates code and documentation. The project does not vendor OpenSSL: only system development headers are required, with no Perl/NASM prerequisite and no network access at configure time. CI uses `openssl@3` and `openssl@4` on macOS to cover 3.x/4.x, and asserts 3.x on Linux/Windows.

## Consume via FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(
    mcp-cpp-sdk
    GIT_REPOSITORY https://github.com/modelcontextprotocol/cpp-sdk
    GIT_TAG        main
)
FetchContent_MakeAvailable(mcp-cpp-sdk)

target_link_libraries(your_target PRIVATE mcp-client mcp-server)
```

Available library targets:

| Target          | Type       | Description                          |
|-----------------|------------|--------------------------------------|
| mcp-core        | STATIC     | Core protocol types                  |
| mcp-transport   | STATIC     | Transport implementations            |
| mcp-protocol    | STATIC     | JSON-RPC engine, WireCodec           |
| mcp-http        | STATIC     | HTTP/SSE server transport            |
| mcp-server      | STATIC     | McpServer, tools/resources/prompts   |
| mcp-client      | STATIC     | McpClient, OAuth, MRTR               |

## Conformance Fixture

The official conformance server fixture (`examples/conformance/server`) requires an explicit build switch (default OFF):

```bash
cmake --preset debug -DMCP_BUILD_CONFORMANCE=ON -DMCP_BUILD_EXAMPLES=ON
cmake --build --preset debug
```

The conformance client (`examples/conformance/client`) builds with `MCP_BUILD_EXAMPLES=ON`. Local reproduction of the CI conformance workflow: `scripts/run-conformance.sh server|client`.
