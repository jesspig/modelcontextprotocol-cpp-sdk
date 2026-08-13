# 安装

## 环境要求

| 依赖项          | 最低版本     | 说明                          |
|----------------|------------|--------------------------------|
| CMake          | 3.28       | 需要 Ninja 生成器               |
| C++ 编译器      | C++17      | MSVC、Clang、GCC               |
| OpenSSL        | 必需（开发头文件） | 编译 `mcp-client` 的硬依赖（`<openssl/rand.h>` 被无条件包含）；CMake 自动查找。用于 TLS 加密（WebSocket、SSE HTTPS 等）。PKCE 验证器生成在 `MCP_HAVE_OPENSSL` 下使用 `RAND_bytes`，否则回退 `std::random_device` |

## 通过 FetchContent 引用

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

可用的库目标：

| 目标             | 类型        | 说明                                |
|-----------------|------------|--------------------------------------|
| mcp-core        | STATIC     | 核心协议类型                         |
| mcp-transport   | STATIC     | 传输层实现                           |
| mcp-protocol    | STATIC     | JSON-RPC 引擎、WireCodec             |
| mcp-http        | STATIC     | HTTP/SSE 服务器传输                  |
| mcp-server      | STATIC     | McpServer、工具/资源/提示             |
| mcp-client      | STATIC     | McpClient、OAuth、MRTR               |
