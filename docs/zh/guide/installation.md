# 安装

## 环境要求

| 依赖项          | 最低版本     | 说明                          |
|----------------|------------|--------------------------------|
| CMake          | 4.2        | 需要 Ninja 生成器               |
| C++ 编译器      | C++17      | MSVC、Clang、GCC               |
| asio           | 1.30.2     | 自动拉取                        |
| nlohmann-json  | 3.11.3     | 自动拉取                        |
| OpenSSL        | （可选）     | OAuth PKCE 所需                |

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
| mcp-core        | INTERFACE  | 仅头文件的协议类型                     |
| mcp-transport   | STATIC     | 传输层实现                           |
| mcp-protocol    | STATIC     | JSON-RPC 引擎、WireCodec             |
| mcp-http        | STATIC     | HTTP/SSE 服务器传输                  |
| mcp-server      | STATIC     | McpServer、工具/资源/提示             |
| mcp-client      | STATIC     | McpClient、OAuth、MRTR               |
