# AGENTS.md — MCP C++17 SDK

Model Context Protocol 的 C++17 实现。全部为静态库，依赖链：`mcp-core` → `mcp-transport` → `mcp-protocol` → `mcp-server | mcp-client`，`mcp-http` 依赖 `mcp-transport`。架构详见 [docs/zh/guide/architecture.md](docs/zh/guide/architecture.md) 与 [wiki/modules/](wiki/modules/)。

## 构建与测试

```bash
cmake --preset debug          # 或 release；Ninja 生成器，构建目录 build/<preset>
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

- 运行单个用例：`ctest --preset debug -R 'XxxTest.CaseName'`，或直接跑二进制：`build/debug/tests/unit/mcp-core-tests --gtest_filter='XxxTest.*'`（自研框架兼容该参数名）
- 构建示例需 `-DMCP_BUILD_EXAMPLES=ON`（预设默认 OFF）
- configure 无需联网（无第三方依赖拉取）；构建目录不可跨机拷贝
- 编译器自动探测：Windows 优先 clang-cl，Linux 优先 clang++-19 起；只有 MSVC 时显式传 `-DCMAKE_CXX_COMPILER=cl`
- 警告即错误：`-DMCP_WERROR=ON`（CI 自动加）；主分支 `develop`，CI 仅对其 push/PR 运行（8 job：Windows/Linux-clang/Linux-gcc/macOS × debug/release，fail-fast false + sccache，改动须在全部平台编译通过；Windows 上 CI 自动探测 `C:\Program Files\OpenSSL*` 传 `OPENSSL_ROOT_DIR`）
- 第三方依赖仅系统 OpenSSL（可选）；libhv/simdjson/googletest 已移除，文档若引用属过时
- 详细说明见 [wiki/build.md](wiki/build.md)、[wiki/tests.md](wiki/tests.md)

## 代码约定

- **命名**：类/接口 PascalCase（接口加 `I` 前缀，如 `ITransport`）；方法 PascalCase；成员 `snake_case_` 尾下划线；常量 `k` 前缀（`kLatestProtocolVersion`）；`enum class` 值 PascalCase
- **错误处理**：抛 `McpError(McpErrorCode, message)`（继承 `std::runtime_error`）；用户 handler 抛出的异常被包装为 `HandlerError`
- **所有权**：`McpClient/McpServer::Create` 返回 `unique_ptr`；`IClientTransport::Connect()` 返回 `shared_ptr`；`JsonValue` 按值传递，可选字段用 `std::optional`
- **头文件**：`#pragma once` + 首行 `// FileName.hpp — 说明`；4 空格缩进；单一 `namespace mcp`，内部实现放 `mcp::detail`
- `MCP_API` 导出宏为 no-op（当前全静态库）
- 不写代码注释，意图靠命名与结构表达
- 提交信息：带 scope 的 Conventional Commits，scope 与描述均为中文（如 `refactor(网络栈):`、`docs(文档):`、`bench(核心库):`）
- 分支/发布：功能开发在 `feature/*`（合入 `develop`），发布走 `release/*` → 打 tag `X.Y.Z`（无 v 前缀，历史至 0.3.1）
- **docs.yml 部署分支铁律**：`docs.yml` 仅允许在 `master` 分支触发并部署 GitHub Pages——这是唯一允许的分支，绝不允许改为 `develop` 或任何其他分支，也绝不允许为其他分支开通 Pages 部署权限；任何将 docs.yml 在除 `master` 以外分支触发的修改都是绝对错误的行为

## 关键陷阱

1. **协议版本共 5 个**（`ProtocolVersion.hpp` 的 `kProtocolVersions[]`：2024-11-05 至 2026-07-28；缺失版本声明时默认 `kDefaultNegotiatedProtocolVersion`="2025-03-26"），现代判定为字典序 `>= "2026-07-28"`（`IsModernProtocolVersion`）。新增协议方法/通知必须同时考虑两个 era（`WireCodec` 按协商版本 era-gating）
2. **Unity build 默认 ON**（core/server/client/http），但 **protocol/transport 为 OFF**。新增 `.cpp` 前先确认目标是否开启 `UNITY_BUILD`——含匿名 namespace 或同名静态符号会重复定义
3. **OpenSSL 是可选依赖**：`find_package(OpenSSL QUIET)` 找到才定义 `MCP_HAVE_OPENSSL`（仅 client/transport）；`src/transport/detail/net/TlsSocket.cpp`/`Sha1.hpp`/`src/client/auth/OAuthClientProvider.cpp` 的 `<openssl/rand.h>` 包含均有 `#ifdef` 保护——未找到时 TLS 被禁用、PKCE 回退内置 SHA-256。docs 中"必需"表述已过时，以代码为准
4. **IO 线程回调内调用 `Close()` 会 self-join**：stdio/SSE/HTTP 传输的 IO 线程直接执行用户回调（`on_transport_close`/`on_transport_error`），回调里调 `Close()` 会 join 自身线程。所有 `Close()` 必须用 `detail::JoinThreadSafely`（`include/mcp/detail/ThreadUtils.hpp`）
5. **`PipeHandle::Read` 返回 0 不一定是 EOF**（`include/mcp/transport/detail/PlatformIO.hpp`）：Posix 实现 poll 轮询（100ms 超时），无数据时返回 0；读循环必须用 `IsEof()` 区分超时与真 EOF，否则空闲时误判断连
6. **Release 自动 LTO**（Clang ThinLTO / MSVC LTCG / GCC IPO），本地 Release 链接慢属正常
7. **非 CI 且非 Apple 默认 `-march=native`**（`MCP_IS_CI` 由环境变量 `CI` 决定，`Platform.cmake`），二进制不可跨机器分发
8. **日志默认全关**：调试先设 `MCP_LOG_LEVEL=4`（0-5）
9. **`SamplingHandler`/`RootsHandler` 已废弃**（SEP-2577），新代码用 `ElicitationHandler`（MRTR）
10. **Auto 模式协商回退按传输分类**：stdio 类传输（`IsStdioLikeTransport` 用 RTTI 字符串匹配 `InMemoryTransportImpl`/`StdioClientSessionTransport`）探测失败回退 `initialize`，网络类传输抛错（超时/连接错误各不同）；`-32022` 且 supported 含最新版本时 corrective 重试一次，二次拒绝为硬错误。`VersionNegotiation.hpp` 头注释已同步该行为，详见 [wiki/concepts/version-negotiation.md](wiki/concepts/version-negotiation.md)

## 测试约定

- 自研测试框架（`tests/framework/`，mcp-test + mcp-test-main；`mcp_discover_tests` 逐用例注册 ctest）：`tests/unit/` 13 目标 + `tests/integration/` 1 + `tests/conformance/` 1 + `tests/framework/` 1（SelfTests）= 16 目标 / 445 用例（以 `ctest -N` 实测为准）
- 文件命名 `XxxTests.cpp`，套件 `TEST(XxxTest, CaseName)`，断言 `EXPECT_*`/`ASSERT_*`，链接 `mcp-test-main`；框架头 `#include <mcp/test/McpTest.hpp>`；`--gtest_filter` 参数名兼容
- `tests/test_utils/` 是空目录；共享工具在 `tests/unit/TestServerUtil.hpp`
- `WireCodec::ValidateResponse`/`StampOutgoingRequest` 生产代码无调用者但**有测试守护**——不是死代码，勿删
- 集成测试 `RunWithTimeout`：body 挂起超 10s 会 `std::_Exit(1)` 快速失败（勿改回永久阻塞）

## 文件编辑纪律

- **绝对禁止**用 shell 或脚本（PowerShell/批处理/Python 等）批量写入、替换或修改文件内容——该规则适用于仓库内**所有文件**（源码、CMake、测试、docs、wiki 等），曾因批量替换造成 BOM 污染与 timestamp 全量误改；所有文件编辑必须用 `edit` / `write` 工具逐个进行
- 子代理同样禁止用 shell/脚本编辑文件；需要大批量更新时启动多个子代理，每个子代理对各自负责的文件集逐个用 `edit`/`write` 更新

## wiki 知识库维护

`wiki/` 是源码知识库（面向源码理解，与 `docs/` 面向使用者的在线文档分离），入口 [wiki/index.md](wiki/index.md)。维护规则：

- **触发时机**：功能或代码变更落地后**及时**更新对应 wiki 页面并记录 changelog，不推迟到提交前统一处理；微调或小改动不更新
- **更新前核查**：`git status` 与 `git diff HEAD` 对照全部实际变更（含用户手动修改的文件），禁止仅凭对话记忆撰写；无法核实的内容标注 `> [!todo] 待补充`
- **彻底清除过时描述**，不留废弃标记
- **changelog/**：按天分文件 `<YYYY-MM-DD>-log.md`，每条记录时间戳 `<YYYY-MM-DD-HH>`（精确到小时）
- **log.md**：摘要文件，仅保留最近 7 条
- **frontmatter**：概念页面（modules/classes/transports/concepts 等）必须含 YAML frontmatter——`type` 必需（同类页面取值一致），推荐 `title`/`description`/`tags`/`timestamp`；对应源码资产必须加 `resource` 指向源码路径。`timestamp` 记录页面最后修改时刻，仅在内容实际变更时更新，内容未变的页面禁止改写；更新时必须获取系统**实际时间**（如 `Get-Date`）写入，禁止编造或沿用时间值
- **index.md 与 log.md** 为保留文件，不含 frontmatter
- **链接**：内部链接以 `/` 开头指向知识库根；正文禁止大段粘贴源码

## 文档导航（改动前先查阅）

- [docs/zh/](docs/zh/)（VitePress 教程，面向使用者；[docs/en/](docs/en/) 为其英文镜像）——guide/、server/、client/、advanced/
- [wiki/](wiki/index.md)（面向源码理解，按实际代码维护）——modules/（按库）、classes/（关键类）、transports/、concepts/（版本协商、MRTR、并发、存储、OAuth）
- 示例：`examples/EchoServer`（注册 API 样板）、`examples/WeatherServer`（多工具）、`examples/SimpleClient`（客户端用法）
