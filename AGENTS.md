# AGENTS.md — MCP C++17 SDK

Model Context Protocol 的 C++17 实现，全静态库。依赖链：`mcp-core` → `mcp-transport` → `mcp-protocol` → `mcp-server` | `mcp-client`；`mcp-http` 依赖 `mcp-transport`。架构见 [wiki/index.md](wiki/index.md) 与 [docs/zh/guide/architecture.md](docs/zh/guide/architecture.md)。

## 构建与测试

```bash
cmake --preset debug            # 仅 Ninja 生成器；CMake 3.28+，C++17 强制
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

- 单用例：`ctest --preset debug -R 'XxxTest.CaseName'`，或直接跑 `build/debug/tests/unit/mcp-core-tests --gtest_filter='XxxTest.*'`（自研框架兼容该参数名）。
- 示例需 `-DMCP_BUILD_EXAMPLES=ON`（预设默认 OFF）；官方 conformance fixture 需 `-DMCP_BUILD_CONFORMANCE=ON`（`examples/conformance/server`，配套 `.github/workflows/conformance.yml` 与 `scripts/run-conformance.sh`）。
- 编译器自动探测：Windows 优先 clang-cl，Linux 优先 clang++-19 起，仅 MSVC 时显式 `-DCMAKE_CXX_COMPILER=cl`。
- `-DMCP_WERROR=ON` 才开警告即错误（CI 自动加，本地默认关）。
- configure 无第三方拉取；唯一可选系统依赖是 OpenSSL（未找到则禁用 TLS；PKCE 随机源回退 `BCryptGenRandom`/`random_device`，SHA-256 恒为内置实现）。
- 验证全量：`ctest -N` 看实际用例数（17 目标 / 578 用例）。
- 自研测试框架（`tests/framework/`）：套件 `TEST(XxxTest, CaseName)`，断言 `EXPECT_*/ASSERT_*`，链接 `mcp-test-main`；`--gtest_filter` 参数名兼容。
- `WireCodec::ValidateResponse`/`StampOutgoingRequest` 生产代码无调用者但**有测试守护**——不是死代码，勿删。
- 集成测试超时护栏：`MCP_RUN_WITH_TIMEOUT`（`tests/framework/include/mcp/test/McpTimeout.hpp`，默认 10s）——超时记为用例失败并 `MarkAbandoned()`（跳过 TearDown、runner 继续后续用例），勿改回永久阻塞。

## 关键陷阱（改动前必读，每条都曾踩坑）

1. **协议版本共 5 个**（`ProtocolVersion.hpp` 的 `kProtocolVersions[]`：2024-11-05 至 2026-07-28；缺失版本声明默认 `kDefaultNegotiatedProtocolVersion="2025-03-26"`），现代判定字典序 `>= "2026-07-28"`（`IsModernProtocolVersion`）。新增协议方法/通知须同时考虑双 era，`WireCodec` 按协商版本 era-gating。
2. **Unity build 默认 ON**（core/server/client/http），但 **protocol/transport 为 OFF**。新增 `.cpp` 前先确认目标 `UNITY_BUILD`——含匿名 namespace 或同名静态符号会重复定义。
3. **OpenSSL 可选**：`MCP_HAVE_OPENSSL` 定义才用；`src/.../TlsSocket.cpp`、`Sha1.hpp`、`OAuthClientProvider.cpp` 的 `<openssl/rand.h>` 均有 `#ifdef` 保护，勿删。
4. **IO 线程回调内调 `Close()` 会 self-join**：stdio/SSE/HTTP 的 IO 线程直接执行用户回调，回调里调 `Close()` 会 join 自身线程。所有 `Close()` 必须用 `detail::JoinThreadSafely`（`include/mcp/detail/ThreadUtils.hpp`）。
5. **`PipeHandle::Read` 返回 0 不一定是 EOF**（`include/mcp/transport/detail/PlatformIO.hpp`）：Posix 实现 poll 轮询（100ms 超时），无数据返回 0；读循环必须用 `IsEof()` 区分超时与真 EOF，否则空闲误判断连。
6. **Release 自动 LTO**（Clang ThinLTO / MSVC LTCG / GCC IPO），本地 Release 链接慢属正常。
7. **非 CI 且非 Apple 默认 `-march=native`**（`MCP_IS_CI` 由环境变量 `CI` 决定），二进制不可跨机分发。
8. **日志默认全关**：调试设 `MCP_LOG_LEVEL=4`（0-5）；运行时可用 `mcp::SetLogLevel` 覆盖。
9. **`SamplingHandler`/`RootsHandler` 已废弃**（SEP-2577），新代码用 `ElicitationHandler`（MRTR）。
10. **Auto 模式协商回退按传输分类**：stdio 类（`IsStdioLikeTransport` RTTI 匹配 `InMemoryTransportImpl`/`StdioClientSessionTransport`）探测失败回退 `initialize`；网络类抛错；`-32022` 且 supported 含最新版本时 corrective 重试一次，二次拒绝为硬错误。详见 [wiki/concepts/version-negotiation.md](wiki/concepts/version-negotiation.md)。

## 代码与提交约定

- 命名：类/接口 PascalCase（接口加 `I` 前缀，如 `ITransport`）；方法 PascalCase；成员 `snake_case_` 尾下划线；常量 `k` 前缀；`enum class` 值 PascalCase。
- **不写代码注释**（意图靠命名与结构表达）；仅在项目已有注释惯例处补必要注释。
- `McpClient/McpServer::Create` 返回 `unique_ptr`；`IClientTransport::Connect()` 返回 `shared_ptr`；`JsonValue` 按值传递，可选字段用 `std::optional`。
- 错误处理：抛 `McpError(McpErrorCode, message)`（继承 `std::runtime_error`）；用户 handler 异常被包装为 `HandlerError`。
- 提交：带 scope 的 Conventional Commits，scope 与描述均为中文（如 `refactor(网络栈):`、`docs(文档):`）。
- 分支/发布：功能 `feature/*` → 合入 `develop`；发布 `release/*` → 打 tag `X.Y.Z`（无 v 前缀）。
- **docs.yml 部署分支铁律**：仅 `master` 分支触发并部署 GitHub Pages——绝不允许改为 `develop` 或任何其他分支，也绝不为其他分支开通 Pages 部署。

## 文件编辑纪律（铁律）

- **绝对禁止**用 shell/脚本（PowerShell/批处理/Python 等）批量写入、替换或修改文件内容——曾因批量替换造成 BOM 污染与 timestamp 全量误改。所有文件编辑必须用 `edit` / `write` 工具逐个进行，包括 wiki。
- 子代理同样禁止用 shell/脚本编辑文件；大批量更新时启动多个子代理，各自对负责的文件集逐个 `edit`/`write`。

## wiki 知识库维护

源码知识库在 `wiki/`（面向源码理解，与 `docs/zh`、`docs/en` 在线文档分离），入口 [wiki/index.md](wiki/index.md)。改动落地后及时更新对应页面并记 changelog，不推迟到提交前统一处理。

- **核查**：更新前 `git status` 与 `git diff HEAD` 对照全部实际变更（含用户手动修改），禁止凭对话记忆；无法核实处标 `> [!todo] 待补充`。彻底清除过时描述，不留废弃标记。
- **frontmatter**：概念页（modules/classes/transports/concepts）必须含 YAML frontmatter——`type` 必填（同类一致），推荐 `title`/`description`/`tags`/`timestamp`（ISO 8601 真实时间，`Get-Date` 获取，仅内容实际变更时更新）；对应源码资产加 `resource`。`index.md` 与 `log.md` 为保留文件，无 frontmatter。
- **changelog/**：按天分文件 `<YYYY-MM-DD>-log.md`，每条时间戳 `<YYYY-MM-DD-HH>`（精确到小时）。`log.md` 仅保留最近 7 条。
- **链接**：内部链接以 `/` 开头指向知识库根；正文禁止大段粘贴源码。

## 文档导航

- 在线文档：[docs/zh/](docs/zh/)（教程）、[docs/en/](docs/en/)（镜像）。
- 源码知识库：[wiki/index.md](wiki/index.md) — modules/（按库）、classes/（关键类）、transports/、concepts/（版本协商、MRTR、并发、存储、OAuth、_meta 与过滤器、日志）。
- 示例：`examples/EchoServer`（注册 API 样板）、`examples/WeatherServer`（多工具）、`examples/SimpleClient`（客户端用法）。
