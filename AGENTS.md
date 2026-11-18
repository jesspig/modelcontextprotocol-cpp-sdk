# AGENTS.md — MCP C++17 SDK

C++17 全静态库。依赖链 `mcp-core` → `mcp-transport` → `mcp-protocol` → `mcp-server` | `mcp-client`；`mcp-server`/`mcp-client`/`mcp-http` 均链接 `mcp-ext`，`mcp-http` 另依赖 `mcp-transport`。架构见 [wiki/index.md](wiki/index.md) 与 [docs/zh/learn/architecture.md](docs/zh/learn/architecture.md)。

## 构建与测试

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

- 仅 Ninja；CMake 3.28+、C++17 强制。单用例 `ctest --preset debug -R 'XxxTest.CaseName'`，或 `build/debug/tests/unit/mcp-core-tests --gtest_filter='XxxTest.*'`。示例需 `-DMCP_BUILD_EXAMPLES=ON`（预设默认 OFF）；conformance fixture 需 `-DMCP_BUILD_CONFORMANCE=ON`，fixture 与基线见 [wiki/tests.md](wiki/tests.md)。
- 编译器自动探测：Windows 优先 clang-cl，Linux 优先 clang++-19 起，仅 MSVC 时显式 `-DCMAKE_CXX_COMPILER=cl`；`-DMCP_WERROR=ON` 才警告即错误（本地默认关，CI 自动开）。configure 不拉第三方，唯一可选系统依赖 OpenSSL：`-DMCP_WITH_OPENSSL=AUTO|ON|OFF`（默认 `AUTO` 按 `find_package` 结果启用，`ON` 未找到即 configure 失败，`OFF` 强制禁用；仅 3.x/4.x）；未启用则禁 TLS，PKCE 随机源回退 `BCryptGenRandom`/`random_device`，SHA-256 恒为内置实现。
- 本机基线（Windows、OpenSSL 未探到）：18 目标 / 1021 ctest 用例；静态 `TEST/TEST_F/TEST_P` 1032，条件编译差 11（9 个 POSIX 专属 + 2 个 TLS 专属）、断言 token 4887 的逐项明细见 [wiki/tests.md](wiki/tests.md)；自查 `ctest --test-dir build/debug -N`。
- 测试栈 googletest（子模块 `third_party/googletest`），目标链 `mcp-test-support` + `GTest::gtest_main`，注册走 `mcp_discover_tests`。`WireCodec::ValidateResponse`/`StampOutgoingRequest` 无生产调用者但有测试守护，不是死代码，勿删；集成测试护栏 `MCP_RUN_WITH_TIMEOUT`（`tests/support/include/mcp/test/McpTimeout.hpp`，默认 10s）超时打印 Failure 并 `std::_Exit(EXIT_FAILURE)` 硬退出整个进程，勿改回永久阻塞。

## 关键陷阱（每条都曾踩坑）

1. **协议版本 5 个**（`ProtocolVersion.hpp` 的 `kProtocolVersions[]`：2024-11-05 至 2026-07-28；缺失声明默认 `kDefaultNegotiatedProtocolVersion="2025-03-26"`；现代判定 `IsModernProtocolVersion` 字典序 `>= "2026-07-28"`）。新增方法/通知须同时处理双 era，`WireCodec` 按协商版本门控。
2. **Unity build 默认 ON**（core/server/client/http/ext；protocol/transport OFF）：新增 `.cpp` 前确认目标 `UNITY_BUILD`，匿名 namespace 或同名静态符号会重复定义。
3. **OpenSSL 可选**：`MCP_HAVE_OPENSSL` 宏与链接只经 INTERFACE 目标 `mcp-openssl` 施加到 transport/client/ext；**禁止用 `__has_include` 嗅探头文件推断可用**（头可见 ≠ 宏已定义 ≠ 已链接）。`TlsSocket.cpp`、`Sha1.hpp`、`OAuthClientProvider.cpp`、`DPoP.cpp`、`McpServer.cpp` 的 OpenSSL 头文件包含均有 `#ifdef MCP_HAVE_OPENSSL` 保护，勿删。CI 显式锁 `openssl@3`/`openssl@4`，禁 `brew install openssl` 滚动别名；主版本升级先看 canary；不内置 OpenSSL 源码，不新增 Perl/NASM 前置。
4. **IO 线程回调内调 `Close()` 会 self-join**，所有 `Close()` 用 `detail::JoinThreadSafely`（`include/mcp/detail/ThreadUtils.hpp`）；**`PipeHandle::Read` 返回 0 不一定是 EOF**（`PlatformIO.hpp`）：Posix poll 100ms 无数据返回 0，读循环必须用 `IsEof()` 区分超时与真 EOF。
5. **Release 自动 LTO**（ThinLTO/LTCG/IPO），链接慢正常；非 CI 且非 Apple 的 clang/gcc 加 `-march=native`（`MCP_IS_CI` 读环境变量 `CI`），产物不可跨机分发。**日志默认全关**：调试设 `MCP_LOG_LEVEL=4`（0-5），运行时 `mcp::SetLogLevel` 覆盖。
6. **`SamplingHandler`/`RootsHandler` 已废弃**（SEP-2577），新代码用 `ElicitationHandler`（MRTR）。
7. **Auto 模式协商回退按传输分类**：stdio 类（`IsStdioLikeTransport` RTTI 匹配 `InMemoryTransportImpl`/`StdioClientSessionTransport`）探测失败回退 `initialize`，网络类抛错；`-32022` 且 supported 含最新版本时 corrective 重试一次，二次拒绝为硬错误。详见 [wiki/concepts/version-negotiation.md](wiki/concepts/version-negotiation.md)。
8. **GCC 独有 `-Wdangling-else`**：断言宏自带 `if/else`，禁止作无括号单语句体——`if (c) EXPECT_EQ(a, b);` 在 GCC 下即 `-Werror` 硬错误，必须补 `{}`；只有 Linux-gcc job 能兜住。

## 代码 / 提交 / 兼容 / 编辑

- 命名：类/接口 PascalCase（接口 `I` 前缀）、方法 PascalCase、成员 `snake_case_` 尾下划线、常量 `k` 前缀、`enum class` 值 PascalCase。**零代码注释**：仅留 `// namespace xxx` 结尾注释与有功能效应的工具指令（`NOLINT`、`clang-format`、`IWYU pragma` 等）；`/* 参数名 */`、`TODO`、解释性注释一律删除，新增代码不得引入注释。
- API：`McpServer/McpClient::Create` 返回 `unique_ptr`，`IClientTransport::Connect()` 返回 `shared_ptr`，`JsonValue` 按值传递，可选字段用 `std::optional`；错误抛 `McpError(McpErrorCode, message)`（继承 `std::runtime_error`）；`HandlerError` 用于本地回调/扩展前置条件不满足，工具 handler 抛出的普通异常在服务端映射为 `InternalError`。
- 提交：带 scope 的 Conventional Commits，scope 与描述均中文（如 `refactor(网络栈):`）；`feature/*` → `develop`，`release/*` → tag `X.Y.Z`（无 v 前缀）。**docs.yml 只允许 `master` 触发并部署 GitHub Pages**，不得改为其他分支，也不得为其他分支开通 Pages。
- **兼容优先**（高于其余小节）：先评估能否不破坏源码与线协议。A 仅改内部实现；B 源码兼容但行为/线协议收紧，迁移指南须写「旧行为 / 新行为 / 受影响怎么改」；C 新增编译警告（如 `[[nodiscard]]`）须给下游消除方式（`(void)` 或改为检查），禁删属性。
- **弃用优于删除**：保留实现并标 `[[deprecated("since X.Y.Z, removed in A.B.C; <替代写法>")]]`，窗口至少跨一个 minor；禁直接删除或改语义。**破坏性变更三条件**：迁移指南说明为何无兼容路径 + old/new 完整代码块 + `docs/snippets/` 新写法通过 `mcp-docs-snippets` 编译；缺一退回弃用；齐备才可新增 `tests/compat/expected_breaks.txt` 条目。新增默认不改旧行为：用 `std::optional` 且默认 `nullopt` 等价旧行为，三态用 `std::optional<bool>`。
- **文档同批 + 零破坏举证**：行为变更与迁移说明同批提交，`expected_breaks.txt` 与迁移表格逐条对应；发版前 `mcp-compat-smoke` 必过，`expected_breaks.txt` 与上一版逐字一致（E 类例外须满足三条件）。弃用逐条登记 `docs/{zh,en}/learn/compatibility.md`，含替代写法与移除版本；到期不删除即违约，例外须用户明确批准并记入任务 `STATE.md` 偏离段。
- **文件编辑纪律**：禁止用 shell/脚本批量写入、替换或修改文件（曾造成 BOM 污染与 timestamp 全量误改），一律逐个 `edit`/`write`；子代理同样遵守，大批量更新启动多个子代理各编辑各自文件集。
- 知识库与文档：改动落地即更新 `wiki/` 对应页并记 changelog，不推迟到提交前；编辑规范见 [wiki/AGENTS.md](wiki/AGENTS.md)。在线文档 [docs/zh/](docs/zh/)（教程）、[docs/en/](docs/en/)（镜像）；示例 `examples/EchoServer`（注册 API 样板）、`examples/WeatherServer`（多工具）、`examples/SimpleClient`（客户端用法）。
