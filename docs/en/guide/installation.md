# Installation

## Requirements

| Dependency     | Minimum    | Notes                          |
|----------------|------------|--------------------------------|
| CMake          | 4.2        | Ninja generator required       |
| C++ Compiler   | C++17      | MSVC, Clang, GCC               |
| asio           | 1.30.2     | Fetched automatically          |
| nlohmann-json  | 3.11.3     | Fetched automatically          |
| OpenSSL        | (optional) | Required for OAuth PKCE        |

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
| mcp-core        | INTERFACE  | Header-only protocol types           |
| mcp-transport   | STATIC     | Transport implementations            |
| mcp-protocol    | STATIC     | JSON-RPC engine, WireCodec           |
| mcp-http        | STATIC     | HTTP/SSE server transport            |
| mcp-server      | STATIC     | McpServer, tools/resources/prompts   |
| mcp-client      | STATIC     | McpClient, OAuth, MRTR               |
