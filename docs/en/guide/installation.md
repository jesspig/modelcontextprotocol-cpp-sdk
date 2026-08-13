# Installation

## Requirements

| Dependency     | Minimum    | Notes                          |
|----------------|------------|--------------------------------|
| CMake          | 3.28       | Ninja generator required       |
| C++ Compiler   | C++17      | MSVC, Clang, GCC               |
| simdjson       | 3.12.3     | Fetched automatically          |
| libhv          | 1.3.4      | Fetched automatically          |
| OpenSSL        | Required (dev headers) | Hard requirement for building `mcp-client` (`<openssl/rand.h>` included unconditionally); located automatically by CMake. Used for TLS encryption (WebSocket, SSE HTTPS, etc.). PKCE verifier generation uses `RAND_bytes` when `MCP_HAVE_OPENSSL` is defined, else falls back to `std::random_device` |

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
