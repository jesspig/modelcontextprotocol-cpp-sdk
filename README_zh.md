# MCP C++ SDK

> English documentation: [README.md](README.md)

[Model Context Protocol (MCP)](https://modelcontextprotocol.io) 的 C++17 实现，提供客户端与服务端静态库，用于构建基于 MCP 的 AI 工具集成。

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platform](https://img.shields.io/badge/platform-windows%20%7C%20linux%20%7C%20macos-lightgrey.svg)

**当前状态**：0.3.11（`mcp::kSdkVersion`），五个协议版本、两个时代，124 个一致性测试。支持 Windows（MSVC、clang-cl）、Linux（GCC、Clang）、macOS（Clang）。

## 环境要求

- CMake 3.28+ 与 Ninja 生成器；C++17 编译器（MSVC、Clang、GCC）。
- OpenSSL 可选：`MCP_WITH_OPENSSL=AUTO|ON|OFF`（默认 `AUTO`）。TLS 需要它，OAuth PKCE 不需要（内置 SHA-256 回退）。支持 3.x/4.x。

## 快速开始

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

配置预设：`debug`、`release`。示例需 `-DMCP_BUILD_EXAMPLES=ON`；conformance fixture 需 `-DMCP_BUILD_CONFORMANCE=ON`。

## 文档

完整手册：<https://jesspig.github.io/modelcontextprotocol-cpp-sdk/>。源文件在 `docs/zh`（英文镜像 `docs/en`）。

- [快速上手](docs/zh/start/quickstart.md)——五分钟跑通服务端与客户端。
- [安装与集成](docs/zh/start/installation.md)——依赖、构建选项、`find_package` 与安装布局。
- [架构](docs/zh/learn/architecture.md) · [协议版本](docs/zh/learn/protocol-versions.md) · [扩展](docs/zh/learn/extensions.md)
- [注册一个工具](docs/zh/howto/register-a-tool.md) · [连接服务端](docs/zh/howto/connect-to-a-server.md) · [处理错误](docs/zh/howto/handle-errors.md)
- [API 参考](docs/zh/reference/index.md) · [兼容性与弃用登记表](docs/zh/learn/compatibility.md) · [发布说明](docs/zh/migration/0.3.11.md)
- 英文手册：[docs/en/](docs/en/)。可运行示例：[examples/](examples/)。

## 许可证

MIT — 参见 [LICENSE](LICENSE)。
