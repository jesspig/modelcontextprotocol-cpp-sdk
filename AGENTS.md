# AGENTS.md — MCP C++17 SDK

Model Context Protocol 的 C++17 实现。全部为静态库，依赖链：`mcp-core` → `mcp-transport` → `mcp-protocol` → `mcp-server | mcp-client`，`mcp-http` 依赖 `mcp-transport`。架构详见 [docs/zh/guide/architecture.md](docs/zh/guide/architecture.md) 与 [wiki/modules/](wiki/modules/)。

## 构建与测试

```bash
cmake --preset debug          # 或 release；Ninja 生成器，构建目录 build/<preset>
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

- 运行单个用例：`ctest --preset debug -R 'XxxTest.CaseName'`，或直接跑二进制：`build/debug/tests/unit/mcp-core-tests --gtest_filter='XxxTest.*'`
- 构建示例需 `-DMCP_BUILD_EXAMPLES=ON`（预设默认 OFF）
- 首次 configure 必须联网（simdjson/libhv/googletest 由 FetchContent 拉取，且会改写 libhv 的 CMakeLists）；构建目录不可跨机拷贝
- 编译器自动探测：Windows 优先 clang-cl，Linux 优先 clang++-19 起；只有 MSVC 时显式传 `-DCMAKE_CXX_COMPILER=cl`
- 详细说明见 [wiki/build.md](wiki/build.md)、[wiki/tests.md](wiki/tests.md)

## 代码约定

- **命名**：类/接口 PascalCase（接口加 `I` 前缀，如 `ITransport`）；方法 PascalCase；成员 `snake_case_` 尾下划线；常量 `k` 前缀（`kLatestProtocolVersion`）；`enum class` 值 PascalCase
- **错误处理**：抛 `McpError(McpErrorCode, message)`（继承 `std::runtime_error`）；用户 handler 抛出的异常被包装为 `HandlerError`
- **所有权**：`McpClient/McpServer::Create` 返回 `unique_ptr`；`IClientTransport::Connect()` 返回 `shared_ptr`；`JsonValue` 按值传递，可选字段用 `std::optional`
- **头文件**：`#pragma once` + 首行 `// FileName.hpp — 说明`；4 空格缩进；单一 `namespace mcp`，内部实现放 `mcp::detail`
- `MCP_API` 导出宏为 no-op（当前全静态库）
- 不写代码注释，意图靠命名与结构表达
- 提交信息：带 scope 的 Conventional Commits（如 `feat(transport):`、`docs(wiki):`）

## 关键陷阱

1. **协议版本共 6 个**（`ProtocolVersion.hpp` 的 `kProtocolVersions[]`：2024-10-07 至 2026-07-28），现代判定为字典序 `>= "2026-07-28"`（`IsModernProtocolVersion`）。新增协议方法/通知必须同时考虑两个 era（`WireCodec` 按协商版本 era-gating）
2. **Unity build 默认 ON**（core/server/client/http），但 **protocol/transport 为 OFF**。新增 `.cpp` 前先确认目标是否开启 `UNITY_BUILD`——含匿名 namespace 或同名静态符号会重复定义
3. **OpenSSL 头文件是硬依赖**：`find_package(OpenSSL QUIET)` 找到才定义 `MCP_HAVE_OPENSSL`（仅 client/transport），但 `src/client/auth/OAuthClientProvider.cpp` 有无保护的 `#include <openssl/rand.h>`——无 OpenSSL 开发头的机器无法编译 `mcp-client`，不要按"可选"假设
4. **IO 线程回调内调用 `Close()` 会 self-join**：stdio/SSE/HTTP 传输的 IO 线程直接执行用户回调（`on_transport_close`/`on_transport_error`），回调里调 `Close()` 会 join 自身线程。所有 `Close()` 必须用 `detail::JoinThreadSafely`（`include/mcp/detail/ThreadUtils.hpp`）
5. **`PipeHandle::Read` 返回 0 不一定是 EOF**：PosixPipe 用 poll 轮询，无数据时返回 0；读循环必须用 `IsEof()` 区分超时与真 EOF，否则空闲时误判断连
6. **Release 自动 LTO**（Clang ThinLTO / MSVC LTCG / GCC IPO），本地 Release 链接慢属正常
7. **非 CI 默认 `-march=native`**（`MCP_IS_CI` 由环境变量 `CI` 决定，`Platform.cmake`），二进制不可跨机器分发
8. **日志默认全关**：调试先设 `MCP_LOG_LEVEL=4`（0-5）
9. **`SamplingHandler`/`RootsHandler` 已废弃**（SEP-2577），新代码用 `ElicitationHandler`（MRTR）
10. **Auto 模式协商回退按传输分类**：stdio 类传输回退 `initialize`，网络类传输抛错（超时/异常各不同），且 `-32022` 有 corrective 重试分支——不要按 `VersionNegotiation.hpp` 头注释假设，详见 [wiki/concepts/version-negotiation.md](wiki/concepts/version-negotiation.md)

## 测试约定

- GoogleTest（`gtest_discover_tests` 注册）：`tests/unit/` 12 个目标、`tests/integration/`、`tests/conformance/`（共 14 目标 / 278 用例；conformance 111 用例）
- 文件命名 `XxxTests.cpp`，套件 `TEST(XxxTest, CaseName)`，断言 `EXPECT_*`/`ASSERT_*`，链接 `GTest::gtest_main`
- `tests/test_utils/` 是空目录；共享工具在 `tests/unit/TestServerUtil.hpp`
- `WireCodec::ValidateResponse`/`StampOutgoingRequest` 生产代码无调用者但**有测试守护**——不是死代码，勿删
- 集成测试 `RunWithTimeout`：body 挂起超 10s 会 `std::_Exit(1)` 快速失败（勿改回永久阻塞）

## wiki 知识库维护

`wiki/` 是源码知识库（面向源码理解，与 `docs/` 面向使用者的在线文档分离），入口 [wiki/index.md](wiki/index.md)。维护规则：

- **触发时机**：仅在完成功能、交付操作指南、提交前统一更新；微调或小改动不更新
- **更新前核查**：`git status` 与 `git diff HEAD` 对照全部实际变更（含用户手动修改的文件），禁止仅凭对话记忆撰写；无法核实的内容标注 `> [!todo] 待补充`
- **彻底清除过时描述**，不留废弃标记
- **changelog/**：按天分文件 `<YYYY-MM-DD>-log.md`，每条记录时间戳 `<YYYY-MM-DD-HH>`（精确到小时）
- **log.md**：摘要文件，仅保留最近 7 条
- **frontmatter**：概念页面（modules/classes/transports/concepts 等）必须含 YAML frontmatter——`type` 必需（同类页面取值一致），推荐 `title`/`description`/`tags`/`timestamp`；对应源码资产必须加 `resource` 指向源码路径
- **index.md 与 log.md** 为保留文件，不含 frontmatter
- **链接**：内部链接以 `/` 开头指向知识库根；正文禁止大段粘贴源码

## 文档导航（改动前先查阅）

- [docs/zh/](docs/zh/)（VitePress 教程，面向使用者；[docs/en/](docs/en/) 为其英文镜像）——guide/、server/、client/、advanced/
- [wiki/](wiki/index.md)（面向源码理解，按实际代码维护）——modules/（按库）、classes/（关键类）、transports/、concepts/（版本协商、MRTR、并发、存储、OAuth）
- 示例：`examples/EchoServer`（注册 API 样板）、`examples/WeatherServer`（多工具）、`examples/SimpleClient`（客户端用法）
