# 安装

## 环境要求

| 依赖项          | 最低版本     | 说明                          |
|----------------|------------|--------------------------------|
| CMake          | 3.28       | 需要 Ninja 生成器               |
| C++ 编译器      | C++17      | MSVC、Clang、GCC               |
| OpenSSL        | 可选（开发头文件） | 仅 `mcp-client`/`mcp-transport` 的可选依赖：`find_package(OpenSSL QUIET)` 找到才定义 `MCP_HAVE_OPENSSL` 并链接。未找到时 TLS 被禁用、PKCE 回退内置 SHA-256。用于 TLS 加密（WebSocket、SSE HTTPS 等） |

configure 阶段无需联网：唯一的外部依赖 OpenSSL 通过 `find_package` 探测本机安装，不拉取任何第三方源码。

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
