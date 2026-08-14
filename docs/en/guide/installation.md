# Installation

## Requirements

| Dependency     | Minimum    | Notes                          |
|----------------|------------|--------------------------------|
| CMake          | 3.28       | Ninja generator required       |
| C++ Compiler   | C++17      | MSVC, Clang, GCC               |
| OpenSSL        | Optional (dev headers) | Auto-detected by CMake via `find_package(OpenSSL QUIET)`; when found, `MCP_HAVE_OPENSSL` is defined for `mcp-client`/`mcp-transport`. Used for TLS in the self-hosted network stack (WebSocket, SSE HTTPS, etc.) and `RAND_bytes` for PKCE verifier generation. When absent, TLS support is compiled out and PKCE falls back to `BCryptGenRandom` (Windows) / `std::random_device` (POSIX) with the built-in SHA-256 |

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
