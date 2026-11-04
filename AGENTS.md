# AGENTS.md — MCP C++17 SDK

Model Context Protocol 的 C++17 实现，全静态库。依赖链：`mcp-core` → `mcp-transport` → `mcp-protocol` → `mcp-server` | `mcp-client`；`mcp-http` 依赖 `mcp-transport`。架构见 [wiki/index.md](wiki/index.md) 与 [docs/zh/learn/architecture.md](docs/zh/learn/architecture.md)。

## 构建与测试

```bash
cmake --preset debug            # 仅 Ninja 生成器；CMake 3.28+，C++17 强制
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

- 单用例：`ctest --preset debug -R 'XxxTest.CaseName'`，或直接跑 `build/debug/tests/unit/mcp-core-tests --gtest_filter='XxxTest.*'`（原生 googletest 参数）。
- 示例需 `-DMCP_BUILD_EXAMPLES=ON`（预设默认 OFF）；官方 conformance fixture 需 `-DMCP_BUILD_CONFORMANCE=ON`（`examples/conformance/server`，配套 `.github/workflows/conformance.yml` 与 `scripts/run-conformance.sh`）。
- 编译器自动探测：Windows 优先 clang-cl，Linux 优先 clang++-19 起，仅 MSVC 时显式 `-DCMAKE_CXX_COMPILER=cl`。
- `-DMCP_WERROR=ON` 才开警告即错误（CI 自动加，本地默认关）。
- configure 无第三方拉取；唯一可选系统依赖是 OpenSSL，由 `-DMCP_WITH_OPENSSL=AUTO|ON|OFF`（默认 `AUTO`）控制——`AUTO` 按 `find_package(OpenSSL)` 结果启用，`ON` 未找到即 configure 失败，`OFF` 强制禁用（未启用则禁用 TLS；PKCE 随机源回退 `BCryptGenRandom`/`random_device`，SHA-256 恒为内置实现）。支持 OpenSSL 3.x/4.x；不支持的主版本 configure 失败，不静默关闭 TLS。
- 验证全量：`ctest --test-dir build/debug -N` 看实际用例数。本机 Windows、`MCP_WITH_OPENSSL=AUTO` 未探到 OpenSSL（TLS 关闭）这一档实测 **18 目标 / 1019 用例**；静态行首 `TEST/TEST_F/TEST_P` **1030**，差的 11 个是条件编译用例——9 个 `#ifndef _WIN32` 的 POSIX 专属（`FileTokenCacheTest` 4、`SpawnTest.Posix*` 3、`TransportTest.Stdio*` 2）+ 2 个 `#ifdef MCP_HAVE_OPENSSL` 的 TLS 专属（`TlsSocketTest.HandshakeAndReadFirstFrame`、`TlsSocketTest.IpSanCertificateVerifies`）；静态断言 token **4873**（口径 `tests/**/*.cpp` 逐匹配）。明细见 [wiki/tests.md](wiki/tests.md)。
- 测试栈为 googletest（子模块 `third_party/googletest`）：套件 `TEST(XxxTest, CaseName)`，断言 `EXPECT_*/ASSERT_*`，测试目标链 `mcp-test-support` + `GTest::gtest_main`，注册统一走 `mcp_discover_tests`。
- `WireCodec::ValidateResponse`/`StampOutgoingRequest` 生产代码无调用者但**有测试守护**——不是死代码，勿删。
- 集成测试超时护栏：`MCP_RUN_WITH_TIMEOUT`（`tests/support/include/mcp/test/McpTimeout.hpp`，默认 10s）——超时打印 Failure 并以 `std::_Exit(EXIT_FAILURE)` 硬退出（整进程终止，非"跳过该用例继续跑"），勿改回永久阻塞。

## 关键陷阱（改动前必读，每条都曾踩坑）

1. **协议版本共 5 个**（`ProtocolVersion.hpp` 的 `kProtocolVersions[]`：2024-11-05 至 2026-07-28；缺失版本声明默认 `kDefaultNegotiatedProtocolVersion="2025-03-26"`），现代判定字典序 `>= "2026-07-28"`（`IsModernProtocolVersion`）。新增协议方法/通知须同时考虑双 era，`WireCodec` 按协商版本 era-gating。
2. **Unity build 默认 ON**（core/server/client/http），但 **protocol/transport 为 OFF**。新增 `.cpp` 前先确认目标 `UNITY_BUILD`——含匿名 namespace 或同名静态符号会重复定义。
3. **OpenSSL 可选**：`MCP_HAVE_OPENSSL` 定义才用；启用与否由 `MCP_WITH_OPENSSL`（`AUTO`/`ON`/`OFF`，默认 `AUTO`）与 `find_package(OpenSSL)` 结果共同决定，`MCP_HAVE_OPENSSL` 定义与 OpenSSL 链接统一由单一 INTERFACE 目标 `mcp-openssl` 施加到 `mcp-transport`/`mcp-client`/`mcp-ext`——**禁止用 `__has_include` 嗅探头文件推断 OpenSSL 可用**（头可见不等于已定义、更不等于已链接，曾在 `mcp-ext` 造成链接失败）；`src/.../TlsSocket.cpp`、`Sha1.hpp`、`OAuthClientProvider.cpp` 的 `<openssl/rand.h>` 均有 `#ifdef` 保护，勿删。支持范围 3.x/4.x；必跑 CI 用 `openssl@3`/`openssl@4` 显式锁定，禁止 `brew install openssl` 滚动别名；主版本升级先看 canary；不内置 OpenSSL 源码，不新增 Perl/NASM 前置。
4. **IO 线程回调内调 `Close()` 会 self-join**：stdio/SSE/HTTP 的 IO 线程直接执行用户回调，回调里调 `Close()` 会 join 自身线程。所有 `Close()` 必须用 `detail::JoinThreadSafely`（`include/mcp/detail/ThreadUtils.hpp`）。
5. **`PipeHandle::Read` 返回 0 不一定是 EOF**（`include/mcp/transport/detail/PlatformIO.hpp`）：Posix 实现 poll 轮询（100ms 超时），无数据返回 0；读循环必须用 `IsEof()` 区分超时与真 EOF，否则空闲误判断连。
6. **Release 自动 LTO**（Clang ThinLTO / MSVC LTCG / GCC IPO），本地 Release 链接慢属正常。
7. **非 CI 且非 Apple 默认 `-march=native`**（`MCP_IS_CI` 由环境变量 `CI` 决定），二进制不可跨机分发。
8. **日志默认全关**：调试设 `MCP_LOG_LEVEL=4`（0-5）；运行时可用 `mcp::SetLogLevel` 覆盖。
9. **`SamplingHandler`/`RootsHandler` 已废弃**（SEP-2577），新代码用 `ElicitationHandler`（MRTR）。
10. **Auto 模式协商回退按传输分类**：stdio 类（`IsStdioLikeTransport` RTTI 匹配 `InMemoryTransportImpl`/`StdioClientSessionTransport`）探测失败回退 `initialize`；网络类抛错；`-32022` 且 supported 含最新版本时 corrective 重试一次，二次拒绝为硬错误。详见 [wiki/concepts/version-negotiation.md](wiki/concepts/version-negotiation.md)。
11. **GCC 独有 `-Wdangling-else`**（`-Wall` 成员；clang 不启用，显式加同参数亦不报该形态）：断言宏自带 `if/else`，禁止直接作无括号单语句体——`if (c) EXPECT_EQ(a, b);` 在 GCC 下即 `-Werror` 硬错误，必须补 `{}`。只有 Linux-gcc 两个 job 能兜住。

## 代码与提交约定

- 命名：类/接口 PascalCase（接口加 `I` 前缀，如 `ITransport`）；方法 PascalCase；成员 `snake_case_` 尾下划线；常量 `k` 前缀；`enum class` 值 PascalCase。
- **零代码注释**：禁止一切代码注释，意图靠命名与结构表达，一切说明由 `wiki/` 维护，禁止在代码中重复说明。仅保留两类例外：`// namespace xxx` 结尾注释（格式化工具会重建，删除无意义）与有功能效应的工具指令行（`NOLINT`、`clang-format`、`IWYU pragma` 等）；`/* 参数名 */` 占位、`TODO`、解释性 `//`、`/* */`、`#` 一律删除。新增代码不得引入新注释。
- `McpClient/McpServer::Create` 返回 `unique_ptr`；`IClientTransport::Connect()` 返回 `shared_ptr`；`JsonValue` 按值传递，可选字段用 `std::optional`。
- 错误处理：抛 `McpError(McpErrorCode, message)`（继承 `std::runtime_error`）；用户 handler 异常被包装为 `HandlerError`。
- 提交：带 scope 的 Conventional Commits，scope 与描述均为中文（如 `refactor(网络栈):`、`docs(文档):`）。
- 分支/发布：功能 `feature/*` → 合入 `develop`；发布 `release/*` → 打 tag `X.Y.Z`（无 v 前缀）。
- **docs.yml 部署分支铁律**：仅 `master` 分支触发并部署 GitHub Pages——绝不允许改为 `develop` 或任何其他分支，也绝不为其他分支开通 Pages 部署。

## 兼容性与弃用宪法（铁律）

本节约束一切面向使用方的变更。冲突时优先级高于其余小节；任何例外必须经用户明确批准并记入对应任务的 `STATE.md` 偏离段。

- **兼容优先**：任何改动先评估能否在不破坏源码兼容与线协议兼容的前提下完成。能则按以下三级处理：
  - **A 完全兼容**：只改内部实现，公共签名与协议行为不变；
  - **B 源码兼容 + 行为收紧**：签名不变，运行时或线协议更严格，**必须**在迁移指南写「旧行为 / 新行为 / 受影响时怎么改」三段式；
  - **C 源码兼容 + 新增编译警告**：如新增 `[[nodiscard]]`，**必须**给出下游消除方式（`(void)` 写法或改为检查）。禁止为了让警告消失而删除该属性。
- **弃用优于删除**：可兼容但维护收益低的旧用法，保留实现并标 `[[deprecated("since X.Y.Z, removed in A.B.C; <替代写法>")]]`，弃用窗口至少跨一个 minor；禁止直接删除或改语义。
- **破坏性变更三条件**：只有同时满足以下三条才允许新增 `tests/compat/expected_breaks.txt` 条目——① 迁移指南说明为何无法提供兼容路径（类型布局重构、返回值语义根本冲突）；② 迁移指南有 old/new 完整代码块；③ `docs/snippets/` 有对应新写法且通过 `mcp-docs-snippets` 编译。缺任一条退回弃用路径。
- **新增默认不改旧行为**：新增能力用 `std::optional` 字段且默认 `nullopt` 等价旧行为；禁止用「新增默认值」造成隐性破坏；需三态时用 `std::optional<bool>`。
- **文档同批**：行为变更必须与迁移说明同批提交，禁止先破坏后补文档；`expected_breaks.txt` 的变化即是破坏清单，必须与迁移表格逐条对应。
- **零破坏举证**：发版前 `mcp-compat-smoke` 必须通过，且 `expected_breaks.txt` 与上一版本逐字一致（除非本轮确属 E 类且满足三条件）。
- **弃用登记表**：每条弃用项必须同时登记在 `docs/{zh,en}/learn/compatibility.md`，含替代写法与移除版本；到期版本不删除即视为违约。

## 文件编辑纪律（铁律）

- **绝对禁止**用 shell/脚本（PowerShell/批处理/Python 等）批量写入、替换或修改文件内容——曾因批量替换造成 BOM 污染与 timestamp 全量误改。所有文件编辑必须用 `edit` / `write` 工具逐个进行，包括 wiki。
- 子代理同样禁止用 shell/脚本编辑文件；大批量更新时启动多个子代理，各自对负责的文件集逐个 `edit`/`write`。

## 项目知识库

源码知识库在 `wiki/`（面向源码理解，与 `docs/zh`、`docs/en` 在线文档分离），入口 [wiki/index.md](wiki/index.md)。当前基线为 18 个测试目标、1019 个 ctest 用例（本机 Windows 且 OpenSSL 未启用这一档；静态 1030，差 11 = 9 个 POSIX 专属 + 2 个 TLS 专属）、4873 个源码断言 token。改动落地后及时更新对应页面并记 changelog，不推迟到提交前统一处理。

- **核查**：更新前 `git status` 与 `git diff HEAD` 对照全部实际变更（含用户手动修改），禁止凭对话记忆；无法核实处标 `> [!todo] 待补充`。彻底清除过时描述，不留废弃标记。
- **frontmatter**：概念页（modules/classes/transports/concepts）必须含 YAML frontmatter——`type` 必填（同类一致），推荐 `title`/`description`/`tags`/`timestamp`（ISO 8601 真实时间，`Get-Date` 获取，仅内容实际变更时更新）；对应源码资产加 `resource`。`index.md` 与 `log.md` 为保留文件，无 frontmatter。
- **changelog/**：按天分文件 `<YYYY-MM-DD>-log.md`，每条时间戳 `<YYYY-MM-DD-HH>`（精确到小时）。`log.md` 仅保留最近 7 条。
- **链接**：内部链接以 `/` 开头指向知识库根或使用相对链接（两者均可解析，行号写进链接文本如 `[Foo.cpp:12](path/Foo.cpp)`，禁止把 `:行号` 写进 URL 路径）；正文禁止大段粘贴源码。

## 文档导航

- 在线文档：[docs/zh/](docs/zh/)（教程）、[docs/en/](docs/en/)（镜像）。
- 源码知识库：[wiki/index.md](wiki/index.md) — modules/（按库）、classes/（关键类）、transports/、concepts/（版本协商、MRTR、并发、存储、OAuth、_meta 与过滤器、日志）。
- 示例：`examples/EchoServer`（注册 API 样板）、`examples/WeatherServer`（多工具）、`examples/SimpleClient`（客户端用法）。
