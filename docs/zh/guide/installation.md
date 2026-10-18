# 安装

## 环境要求

| 依赖项          | 最低版本     | 说明                          |
|----------------|------------|--------------------------------|
| CMake          | 3.28       | 需要 Ninja 生成器               |
| C++ 编译器      | C++17      | MSVC、Clang、GCC               |
| OpenSSL        | 可选（开发头文件） | 由 `MCP_WITH_OPENSSL`（取值 `AUTO`/`ON`/`OFF`，默认 `AUTO`）与 `find_package(OpenSSL)` 结果共同决定启用与否：启用时定义 `MCP_HAVE_OPENSSL` 并链接，覆盖 `mcp-transport`/`mcp-client`/`mcp-ext` 三个目标（统一由单一 INTERFACE 目标 `mcp-openssl` 施加）。未启用时 TLS 被禁用、PKCE 回退内置 SHA-256。用于 TLS 加密（WebSocket、SSE HTTPS 等） |

configure 阶段无需联网：唯一的外部依赖 OpenSSL 通过 `find_package` 探测本机安装，不拉取任何第三方源码。

`MCP_WITH_OPENSSL` 三态语义：`AUTO`（默认，`find_package(OpenSSL)` 找到即启用）、`ON`（强制启用，未找到时 configure 直接失败）、`OFF`（强制禁用，即使本机装有 OpenSSL 也不定义 `MCP_HAVE_OPENSSL`、不链接）；构建系统不再通过 `__has_include` 嗅探头文件推断 OpenSSL 可用性。

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

## 一致性测试 fixture

官方一致性测试服务端 fixture（`examples/conformance/server`）需要显式构建开关（默认 OFF）：

```bash
cmake --preset debug -DMCP_BUILD_CONFORMANCE=ON -DMCP_BUILD_EXAMPLES=ON
cmake --build --preset debug
```

一致性测试客户端（`examples/conformance/client`）随 `MCP_BUILD_EXAMPLES=ON` 构建。本地复现 CI 一致性工作流：`scripts/run-conformance.sh server|client`。
