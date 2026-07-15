# MCP C++ SDK

> **English version**: [README.md](README.md)

[Model Context Protocol (MCP)](https://modelcontextprotocol.io) 的 C++17 实现，提供客户端和服务器库，用于构建基于 MCP 的 AI 工具集成。

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platform](https://img.shields.io/badge/platform-windows%20%7C%20linux%20%7C%20macos-lightgrey.svg)

<details>
<summary>目录</summary>

- [什么是 MCP？](#什么是-mcp)
- [环境要求](#环境要求)
- [安装](#安装)
- [快速开始](#快速开始)
- [架构](#架构)
- [传输层](#传输层)
- [使用方式](#使用方式)
- [OAuth 支持](#oauth-支持)
- [协议版本](#协议版本)
- [一致性测试](#一致性测试)
- [示例](#示例)
- [相关项目](#相关项目)
- [许可证](#许可证)

</details>

## 什么是 MCP？

[Model Context Protocol](https://modelcontextprotocol.io) 允许你以安全、标准化的方式构建服务器，向 LLM 应用暴露数据和功能。使用此 SDK 你可以：

- **构建 MCP 服务器**，向任何 MCP 主机暴露工具、资源和提示词
- **构建 MCP 客户端**，连接到任何 MCP 服务器
- **支持所有标准传输层**：stdio、Streamable HTTP、SSE、WebSocket

## 环境要求

| 依赖          | 最低版本 | 说明                          |
|---------------|---------|-------------------------------|
| CMake         | 3.28    | 生成器：推荐 Ninja            |
| C++ 编译器    | C++17   | MSVC、Clang、GCC             |
| asio          | 1.30.2  | 自动下载                      |
| nlohmann-json | 3.11.3  | 自动下载                      |
| OpenSSL       | (可选)  | OAuth PKCE 必需               |

支持平台：**Windows** (MSVC、clang-cl)、**Linux** (GCC、Clang)、**macOS** (Clang)。

## 安装

在你的 CMake 项目中通过 `FetchContent` 使用：

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

可用库目标：`mcp-core`（头文件-only）、`mcp-transport`、`mcp-protocol`、`mcp-server`、`mcp-client`、`mcp-http`。

## 快速开始

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

配置预设：`debug`、`release`。需要 Ninja 生成器。

## 架构

```
┌─────────────────────────────────────┐
│  mcp-server        mcp-client       │  服务器 & 客户端 API
├─────────────────────────────────────┤
│  mcp-protocol                       │  WireCodec、版本协商
├─────────────────────────────────────┤
│  mcp-transport                      │  Stdio、SSE、WebSocket、Streamable HTTP
├─────────────────────────────────────┤
│  mcp-core            mcp-http       │  类型、JSON-RPC、HTTP 服务
└─────────────────────────────────────┘
```

库依赖链：`mcp-core` (INTERFACE) → `mcp-transport` → `mcp-protocol` → `mcp-server | mcp-client`。`mcp-http` 依赖 `mcp-transport`。

- **mcp-core** — 头文件-only。所有 MCP 协议类型（`Tool`、`Resource`、`Prompt`、`ElicitResult` 等）、JSON-RPC 消息结构、错误码、能力声明、传输层接口。
- **mcp-transport** — 传输层实现：stdio（客户端/服务器）、SSE 客户端、WebSocket（简化版）、进程内通信（用于测试）。
- **mcp-protocol** — `McpSessionHandler`（JSON-RPC 引擎）、双时代 `WireCodec`（2025-11-25 / 2026-07-28）、请求/响应关联、`MessageFilter` 管道。
- **mcp-server** — `McpServer`，含工具/资源/提示词注册、`Extension` 框架、`IMcpTaskStore`（含 `FileTaskStore`）、MRTR（`InputRequiredResult`）、服务器到客户端的 elicit。
- **mcp-client** — `McpClient`，含服务器发现、版本协商、OAuth（PKCE/DCR）、MRTR 驱动、工具缓存、`FileTokenCache`。
- **mcp-http** — 用于 Streamable HTTP 模式和 SSE 端点服务的 HTTP 服务器。

### 所有传输层

| 传输层          | 客户端 | 服务器 | 说明                                         |
|----------------|--------|--------|----------------------------------------------|
| Stdio          | 是     | 是     | stdin/stdout 管道                            |
| Streamable HTTP| 是     | 是     | HTTP POST 带流式响应                         |
| SSE            | 是     | 否     | 服务器推送事件（Server-Sent Events）          |
| WebSocket      | 是     | 否     | TCP 双向通信（简化版 RFC 6455）              |
| InMemory       | 是     | 是     | 进程内传输，用于测试                          |

## 使用方式

### 服务器

```cpp
#include <mcp/server/McpServer.hpp>
#include <mcp/transport/StdioServerTransport.hpp>
#include <asio/io_context.hpp>

using namespace mcp;

int main() {
    asio::io_context io_ctx;
    auto transport = std::make_unique<StdioServerTransport>(io_ctx);

    ServerOptions opts;
    opts.server_info = Implementation{"MyServer", "1.0.0"};

    auto server = McpServer::Create(std::move(transport), opts, &io_ctx);

    server->RegisterTool("echo",
        ToolOptions{}.Description("Echo input text back"),
        [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
            auto text = ctx.Params().arguments
                ? ctx.Params().arguments->value("text", "")
                : "";
            CallToolResult result;
            result.content.push_back(TextContent{"text", text});
            return result;
        });

    server->Run();
    return 0;
}
```

### 客户端

```cpp
#include <mcp/client/McpClient.hpp>
#include <mcp/transport/StdioClientTransport.hpp>

using namespace mcp;

auto transport = std::make_unique<StdioClientTransport>("path/to/server");
ClientOptions opts;
opts.client_info = Implementation{"MyClient", "1.0.0"};

auto client = McpClient::Create(std::move(transport), opts);

auto tools = client->ListTools();
for (const auto& tool : tools.tools) {
    std::cout << tool.name << "\n";
}

auto result = client->CallTool("echo", nlohmann::json{{"text", "Hello, MCP!"}});
```

## OAuth 支持

客户端支持 MCP OAuth 授权流程：

- **授权码 + PKCE** 流程，使用 S256 代码挑战
- 动态客户端注册（DCR）
- Token 刷新和撤销
- 可插拔 Token 缓存（`ITokenCache`），内置持久化 `FileTokenCache`

```cpp
auto oauth = std::make_shared<OAuthClientProvider>(
    "https://auth.server.com/.well-known/oauth-authorization-server",
    "client-id");
client->SetOAuthProvider(oauth);
```

## 协议版本

| 版本        | 状态    | 主要特性                                    |
|------------|---------|---------------------------------------------|
| 2025-11-25 | 遗留版本 | `initialize` 握手、独立 sampling/roots/list |
| 2026-07-28 | 当前版本 | `server/discover`、每请求 `_meta`、MRTR（`InputRequiredResult`）、`subscriptions/listen` |

`WireCodec` 工厂通过简单的字符串比较（`version >= "2026-07-28"`）自动选择正确的编解码器。

## 一致性测试

**122 个一致性测试**，覆盖两个时代的协议类型序列化：

- JSON-RPC 消息往返（请求、通知、响应、错误）
- WireCodec 时代门控（2025 与 2026 方法/通知集）
- 工具、资源、提示词序列化（含注解和图标）
- 内容变体分发（文本、图片、音频、嵌入资源）
- Elicitation 和 ElicitResultTyped\<T\>
- MRTR（InputRequiredResult、InputRequests、工厂辅助函数）
- 结构化元数据（RequestMetaObject、NotificationMetaObject）
- 扩展能力、ResultType 枚举、SubscriptionFilter
- 任务（获取/更新/取消）、日志（8 级别）
- 分页、缓存、协议版本辅助函数

## 示例

可运行示例在 [`examples/`](examples/) 目录中：

| 示例                                | 说明                              |
|------------------------------------|-----------------------------------|
| [EchoServer](examples/EchoServer/)   | 最小服务器，含工具、资源和提示词    |
| [WeatherServer](examples/WeatherServer/) | 集成外部 API 的服务器        |
| [SimpleClient](examples/SimpleClient/) | 连接服务器的进程内客户端      |

构建和运行：

```bash
cmake --preset debug -DMCP_BUILD_EXAMPLES=ON
cmake --build --preset debug
# 运行 echo 服务器：
build/debug/examples/EchoServer/EchoServer
```

## 参考实现

本 SDK 参考了官方 MCP 协议规范和参考实现：

| 语言       | 仓库                                                            |
|-----------|----------------------------------------------------------------|
| Python    | [modelcontextprotocol/python-sdk](https://github.com/modelcontextprotocol/python-sdk) |
| TypeScript| [modelcontextprotocol/typescript-sdk](https://github.com/modelcontextprotocol/typescript-sdk) |
| Go        | [modelcontextprotocol/go-sdk](https://github.com/modelcontextprotocol/go-sdk) |
| C#        | [modelcontextprotocol/csharp-sdk](https://github.com/modelcontextprotocol/csharp-sdk) |
| Java      | [modelcontextprotocol/java-sdk](https://github.com/modelcontextprotocol/java-sdk) |
| Rust      | [modelcontextprotocol/rust-sdk](https://github.com/modelcontextprotocol/rust-sdk) |

## 许可证

MIT — 参见 [LICENSE](LICENSE)。
