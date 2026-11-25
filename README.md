# MCP C++ SDK

> 中文文档：[README_zh.md](README_zh.md)

C++17 implementation of the [Model Context Protocol (MCP)](https://modelcontextprotocol.io): static client and server libraries for MCP-based AI tooling integrations.

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platform](https://img.shields.io/badge/platform-windows%20%7C%20linux%20%7C%20macos-lightgrey.svg)

**Status**: 0.3.13 (`mcp::kSdkVersion`), five protocol versions in two eras, 124 conformance tests. Windows (MSVC, clang-cl), Linux (GCC, Clang), macOS (Clang).

## Requirements

- CMake 3.28+ with the Ninja generator; a C++17 compiler (MSVC, Clang, GCC).
- OpenSSL optional: `MCP_WITH_OPENSSL=AUTO|ON|OFF` (default `AUTO`). TLS needs it; OAuth PKCE does not (built-in SHA-256 fallback). Supports 3.x/4.x.

## Quick start

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Presets: `debug`, `release`. Examples need `-DMCP_BUILD_EXAMPLES=ON`; the conformance fixture needs `-DMCP_BUILD_CONFORMANCE=ON`.

## Documentation

Full manual: <https://jesspig.github.io/modelcontextprotocol-cpp-sdk/>. Sources: `docs/zh` (Chinese original), mirrored in English at `docs/en`.

- [Quick start](docs/en/start/quickstart.md) — a working server and client in five minutes.
- [Installation & CMake integration](docs/en/start/installation.md) — dependencies, build options, `find_package`, install layout.
- [Architecture](docs/en/learn/architecture.md) · [Protocol versions](docs/en/learn/protocol-versions.md) · [Extensions](docs/en/learn/extensions.md)
- [Register a tool](docs/en/howto/register-a-tool.md) · [Connect to a server](docs/en/howto/connect-to-a-server.md) · [Handle errors](docs/en/howto/handle-errors.md)
- [API reference](docs/en/reference/index.md) · [Compatibility & deprecation registry](docs/en/learn/compatibility.md) · [Release notes](docs/en/migration/0.3.12.md)
- Chinese manual: [docs/zh/](docs/zh/). Runnable examples: [examples/](examples/).

## License

MIT — [LICENSE](LICENSE).
