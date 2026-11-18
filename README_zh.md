# MCP C++ SDK

> **English version**: [README.md](README.md)

[Model Context Protocol (MCP)](https://modelcontextprotocol.io) 的 C++17 实现，提供客户端和服务器库，用于构建基于 MCP 的 AI 工具集成。所有库均为静态库。

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platform](https://img.shields.io/badge/platform-windows%20%7C%20linux%20%7C%20macos-lightgrey.svg)

**当前状态**：0.3.11（`mcp::kSdkVersion`），五个协议版本、两个时代，124 个一致性测试。支持平台：Windows（MSVC、clang-cl）、Linux（GCC、Clang）、macOS（Clang）。

## 环境要求

| 依赖 | 最低版本 | 说明 |
|---|---|---|
| CMake | 3.28 | 需要 Ninja 生成器 |
| C++ 编译器 | C++17 | MSVC、Clang、GCC |
| OpenSSL | 可选 | `MCP_WITH_OPENSSL=AUTO`/`ON`/`OFF`（默认 `AUTO`）。TLS（WebSocket、SSE HTTPS）必需；OAuth PKCE 无需 OpenSSL（内置 SHA-256 回退）。支持 3.x/4.x |

## 快速开始

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

配置预设：`debug`、`release`。示例需 `-DMCP_BUILD_EXAMPLES=ON`；官方 conformance fixture 需 `-DMCP_BUILD_CONFORMANCE=ON`。

## 安装

```cmake
include(FetchContent)
FetchContent_Declare(
    mcp-cpp-sdk
    GIT_REPOSITORY https://github.com/jesspig/modelcontextprotocol-cpp-sdk.git
    GIT_TAG        0.3.11
)
FetchContent_MakeAvailable(mcp-cpp-sdk)

target_link_libraries(your_target PRIVATE mcp-client mcp-server)
```

库目标：`mcp-core`、`mcp-transport`、`mcp-protocol`、`mcp-server`、`mcp-client`、`mcp-http`、`mcp-ext`，全部为静态库。PUBLIC 依赖会自动传递，链接门面目标即可。`find_package`、安装布局与全部构建开关见[安装与集成](docs/zh/start/installation.md)。

## 文档

在线手册发布在 <https://jesspig.github.io/modelcontextprotocol-cpp-sdk/>，源文件在 `docs/zh`（英文镜像在 `docs/en`），分三层——概念、任务式手册、API 参考：

- [快速上手](docs/zh/start/quickstart.md)——五分钟跑通服务端与客户端，附实测输出。
- [安装与集成](docs/zh/start/installation.md)——依赖、构建选项、CMake 集成与可选 OpenSSL。
- [架构](docs/zh/learn/architecture.md)——库分层、线程模型、能力由注册推导。
- [传输层](docs/zh/learn/transports.md)——stdio、Streamable HTTP、SSE、WebSocket、InMemory，有状态与无状态。
- [协议版本](docs/zh/learn/protocol-versions.md)——双时代可用性矩阵、连接模式与 2026-07-28 破坏性变更。
- [注册一个工具](docs/zh/howto/register-a-tool.md)——描述、输入 schema、行为注解与结构化输出。
- [连接服务端](docs/zh/howto/connect-to-a-server.md)——传输、连接模式与关闭时机。
- [运行 Streamable HTTP 服务端](docs/zh/howto/run-a-streamable-http-server.md)与[放到公网](docs/zh/howto/protect-an-http-server.md)。
- [为客户端接入 OAuth](docs/zh/howto/add-oauth-to-a-client.md)——PKCE、CIMD/DCR、令牌缓存与 DPoP 装配。
- [处理错误](docs/zh/howto/handle-errors.md)——三条失败通道与错误码重映射；[调试无响应服务端](docs/zh/howto/debug-a-stuck-server.md)。
- [API 参考](docs/zh/reference/index.md)——只放表格：签名、默认值、抛出与对应源文件。
- [扩展](docs/zh/learn/extensions.md)——`enable_*` 开关族与真正上线的能力。
- [SDK 对照](docs/zh/learn/what-is-mcp.md)——SDK 分级与 Ruby SDK 对照注记。
- [弃用登记表](docs/zh/learn/compatibility.md)，以及 [0.3.9 迁移指南](docs/zh/migration/0.3.9.md)、[0.3.10](docs/zh/migration/0.3.10.md) 与 [0.3.11](docs/zh/migration/0.3.11.md) 发布说明。

全部 C++ 示例来自 [`docs/snippets`](docs/snippets)，由 CI 的 `mcp-docs-snippets` 目标编译，API 改名或改签名会让文档构建直接失败，而不是让手册悄悄腐烂。

## 示例

可运行示例在 [`examples/`](examples/) 目录中：

| 示例 | 说明 |
|---|---|
| [EchoServer](examples/EchoServer/) | 最小服务器，含工具、资源和提示词 |
| [WeatherServer](examples/WeatherServer/) | 集成外部 API 的服务器 |
| [SimpleClient](examples/SimpleClient/) | 连接服务器的进程内客户端 |

```bash
cmake --preset debug -DMCP_BUILD_EXAMPLES=ON
cmake --build --preset debug
build/debug/examples/EchoServer/EchoServer
```

## 验证

- `ctest --preset debug --output-on-failure` 跑全量单元与集成测试。
- **124 个一致性测试**覆盖 JSON-RPC 消息往返、WireCodec 时代门控、工具/资源/提示词序列化、内容变体分发、elicitation 与 MRTR、结构化元数据、扩展、任务、日志、分页与缓存。
- `CONFORMANCE_SPEC_VERSION=2026-07-28 ./scripts/run-conformance.sh server` 用官方裁判打 fixture（configure 时加 `-DMCP_BUILD_CONFORMANCE=ON`）。

## 非标扩展

ETag 条件请求、渐进式发现（`SearchTools`）与服务器卡片是本 SDK 的扩展，不属于 2026 核心规范；均为可选开启，对端未实现时回退到基线路径。开关、默认值与线路标识见[扩展](docs/zh/learn/extensions.md)。

## 不支持声明

应用界面渲染、完整 sampling、EMA、DPoP 客户端逐请求证明、`jwt-bearer` 授权与可续传文件上传均明确不在范围内；对端使用这些能力时，本 SDK 优雅降级而不中断会话。详见[本 SDK 不做什么](docs/zh/learn/what-is-mcp.md)。

## 许可证

MIT — 参见 [LICENSE](LICENSE)。
