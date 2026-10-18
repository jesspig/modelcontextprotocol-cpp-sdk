# Installation

## Requirements

| Dependency     | Minimum    | Notes                          |
|----------------|------------|--------------------------------|
| CMake          | 3.28       | Ninja generator required       |
| C++ Compiler   | C++17      | MSVC, Clang, GCC               |
| OpenSSL        | Optional (dev headers) | Governed by `MCP_WITH_OPENSSL` (`AUTO`/`ON`/`OFF`, default `AUTO`) together with the `find_package(OpenSSL)` result: when enabled, `MCP_HAVE_OPENSSL` is defined and OpenSSL is linked for `mcp-transport`/`mcp-client`/`mcp-ext` (all applied through the single INTERFACE target `mcp-openssl`). Used for TLS in the self-hosted network stack (WebSocket, SSE HTTPS, etc.) and `RAND_bytes` for PKCE verifier generation. When disabled, TLS support is compiled out and PKCE falls back to `BCryptGenRandom` (Windows) / `std::random_device` (POSIX) with the built-in SHA-256 |

Configure requires no network access: the only external dependency, OpenSSL, is located via `find_package` on the local machine and no third-party source is fetched.

`MCP_WITH_OPENSSL` semantics: `AUTO` (default — enabled when `find_package(OpenSSL)` finds it), `ON` (force-enabled; configure fails when OpenSSL is not found), `OFF` (force-disabled — `MCP_HAVE_OPENSSL` is not defined and OpenSSL is not linked even when installed); the build system no longer sniffs headers via `__has_include` to infer OpenSSL availability.

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
