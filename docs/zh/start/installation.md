# 安装与集成

把本 SDK 装进你自己的 C++17 工程，以及从零构建本仓库的命令。全库为静态库，没有运行时 DLL 依赖。

## 环境要求

| 依赖 | 最低版本 | 说明 |
|---|---|---|
| CMake | 3.28 | `cmake_minimum_required(VERSION 3.28...4.2)`，见 `CMakeLists.txt:1` |
| 生成器 | Ninja | 预设里 `generator: "Ninja"`（`CMakePresets.json`）；仓库只按 Ninja 验证 |
| C++ 标准 | C++17 | 每个库都 `target_compile_features(... PUBLIC cxx_std_17)` |
| 编译器 | MSVC / Clang / GCC / clang-cl | Windows 与 Linux 的 clang 会自动探测（`cmake/CompilerOptions.cmake:17`）；只有 MSVC 时需显式 `-DCMAKE_CXX_COMPILER=cl` |
| 平台 | Windows、Linux、macOS | 三平台都在 CI 中构建 |
| OpenSSL | 可选 | 由 `MCP_WITH_OPENSSL` 控制，见下节 |

## 从零构建本仓库

```bash
git clone --recurse-submodules https://github.com/jesspig/modelcontextprotocol-cpp-sdk.git
cd modelcontextprotocol-cpp-sdk
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

- 忘了带 `--recurse-submodules` 时补一句 `git submodule update --init --recursive`：测试栈 googletest 是子模块 `third_party/googletest`，只在 `MCP_BUILD_TESTS=ON` 时需要。
- 预设把产物写到 `build/debug` 与 `build/release`，并固定 `CMAKE_EXPORT_COMPILE_COMMANDS=ON`、`MCP_BUILD_TESTS=ON`、`MCP_BUILD_EXAMPLES=OFF`。
- 单跑一个用例：`ctest --preset debug -R 'McpServerTest.CaseName'`，或直接执行 `build/debug/tests/unit/mcp-core-tests --gtest_filter='McpServerTest.*'`。
- 用例规模自查：`ctest -N` 列出注册数。

## 作为依赖引入自己的工程

### 方式一：FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(
    mcp-cpp-sdk
    GIT_REPOSITORY https://github.com/jesspig/modelcontextprotocol-cpp-sdk.git
    GIT_TAG        0.3.10
)
FetchContent_MakeAvailable(mcp-cpp-sdk)

target_link_libraries(your_target PRIVATE mcp-client mcp-server)
```

`GIT_TAG` 建议锁 tag（`0.3.10`）而非分支，取最新开发线才用 `master`。库之间是 PUBLIC 传递依赖，链 `mcp-client` 或 `mcp-server` 就够了，不必手写整条链。

### 方式二：安装后 find_package

```bash
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix <你的安装前缀>
```

```cmake
find_package(mcp CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE mcp::mcp-client)
```

安装导出的是 `mcpTargets.cmake` 加包版本文件（`COMPATIBILITY SameMajorVersion`，版本号取自 `include/mcp/McpVersion.hpp` 的 `kSdkVersion`），同时生成 pkg-config 描述文件。`find_package` 会替你 `find_dependency(Threads)`，并在本库启用了 OpenSSL 时 `find_dependency(OpenSSL)`（`cmake/mcpConfig.cmake.in`）。

## 静态库清单

| 目标 | 职责 | 依赖 |
|---|---|---|
| `mcp-core` | JSON-RPC、类型、错误码、日志、span | — |
| `mcp-transport` | 各传输、平台 IO、公共进程 API | `mcp-core` |
| `mcp-protocol` | 会话引擎与双时代编解码 | `mcp-transport` |
| `mcp-server` | 服务端门面 | `mcp-protocol` |
| `mcp-client` | 客户端门面 | `mcp-protocol` |
| `mcp-http` | HTTP 服务端底座与会话/事件存储 | `mcp-transport` |
| `mcp-ext` | apps、server card、DPoP、triggers 扩展 | `mcp-core` |

七个目标全部 `STATIC`（`CMakeLists.txt:90-97`、各 `add_library(mcp-* STATIC)`）。

## 构建开关

| 开关 | 默认 | 作用 |
|---|---|---|
| `MCP_BUILD_TESTS` | `OFF`（预设为 `ON`） | 单元测试、集成测试与 conformance 测试目标 |
| `MCP_BUILD_EXAMPLES` | `OFF` | `examples/` 下的示例程序 |
| `MCP_BUILD_CONFORMANCE` | `OFF` | 官方 conformance 的服务端 fixture（`examples/conformance/server`） |
| `MCP_BUILD_DOCS_SNIPPETS` | `OFF` | 用 `/Zs` 或 `-fsyntax-only` 语法检查 `docs/snippets/*.cpp` |
| `MCP_WERROR` | `OFF` | 警告即错误；CI 一律打开 |
| `MCP_WITH_OPENSSL` | `AUTO` | 见下节 |
| `MCP_UNITY_BUILD` | `ON` | jumbo 编译；批量大小 `MCP_UNITY_BATCH_SIZE`，单批内存上限 `MCP_UNITY_MEM_MB` |

## OpenSSL 是可选依赖

`MCP_WITH_OPENSSL` 三档，语义与 `find_package(OpenSSL)` 的结果共同决定（`cmake/FetchDependencies.cmake:7-27`）：

- `AUTO`：找到就启用，找不到就禁用。
- `ON`：找不到直接 configure 失败，不会静默降级。
- `OFF`：强制禁用，即使系统装了也不启用。

启用时由单一 INTERFACE 目标 `mcp-openssl` 统一定义 `MCP_HAVE_OPENSSL` 并链接到 `mcp-transport`/`mcp-client`/`mcp-ext`。**不要用 `__has_include` 猜 OpenSSL 是否可用**——头文件可见不等于宏已定义，更不等于已链接。支持 3.x 与 4.x；主版本不在范围内时 configure 失败。禁用后的行为：TLS 不可用（WebSocket `wss`、HTTPS SSE 失效），OAuth PKCE 的随机源回退到 `BCryptGenRandom`（Windows）或 `std::random_device`（POSIX），SHA-256 恒为内置实现，配置了 `private_key_pem` 的 `private_key_jwt` 断言与 DPoP 证明会抛 `McpError(InternalError)`。

系统安装方式举例：`vcpkg install openssl`、`apt install libssl-dev`、`brew install openssl@3`。CI 里必须显式锁定主版本，不要用会滚动的 `brew install openssl` 别名。

## 分发陷阱

- **默认带 `-march=native`**：非 CI 且非 Apple 时生效（`cmake/CompilerOptions.cmake:23-24`、`55-56`），产物不可跨机分发。CI 由环境变量 `CI` 判定（`cmake/Platform.cmake:8-11`）关闭该标志。要发布二进制，请显式在 CI 环境构建或自行去掉该标志。
- **Release 自动开 LTO**（Clang ThinLTO、MSVC LTCG、GCC IPO），本地 Release 链接偏慢属正常。
- **unity build 默认 ON**（core/server/client/http），protocol 与 transport 为 OFF。往这些目标里新增 `.cpp` 前先看目标的 `UNITY_BUILD` 设置——含匿名命名空间或同名静态符号会重复定义。

## 验证

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

期望：configure 打印 `[mcp] SDK version: 0.3.10` 与 OpenSSL 档位结论；构建零错误；`ctest` 全量通过（当前规模 18 个测试目标、Windows 本机注册 982 例）。只想验证库能链上，就写一个只 `#include <mcp/client/McpClient.hpp>` 并 `target_link_libraries(... mcp-client)` 的最小可执行文件构建一次。

## 下一步

- 跑通第一个服务端与客户端：[快速上手](/zh/start/quickstart)
- 选择传输方式：[传输层概念](/zh/learn/transports)
- 查阅签名与字段：[服务端参考](/zh/reference/server)

## 下一步

- 装好之后跑通一次端到端：[/zh/start/quickstart](/zh/start/quickstart)
- 服务端第一件要做的事：[/zh/howto/register-a-tool](/zh/howto/register-a-tool)
- 客户端侧的传输与鉴权选择：[/zh/howto/connect-to-a-server](/zh/howto/connect-to-a-server)、[/zh/howto/add-oauth-to-a-client](/zh/howto/add-oauth-to-a-client)

## 本页对应源码

- `CMakeLists.txt`、`CMakePresets.json`、`cmake/CompilerOptions.cmake`、`cmake/FetchDependencies.cmake`、`cmake/BuildOptimization.cmake`、`cmake/Lto.cmake`、`cmake/Platform.cmake`、`cmake/mcpConfig.cmake.in`、`include/mcp/McpVersion.hpp`
