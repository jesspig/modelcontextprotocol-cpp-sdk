---
type: Test Suite
title: 测试体系
description: 18 个测试目标（15 unit + integration/conformance/framework），632 个测试用例（ctest 注册口径，自研测试框架）；静态断言统计 2154；支持致命断言、SCOPED_TRACE、匹配器、过滤/重复/乱序/JSON 报告与超时护栏。
tags: [test, framework, ctest, conformance]
timestamp: 2026-09-20T03:14:18+08:00
resource: tests/CMakeLists.txt
---

# 测试体系

全部经自研 `mcp_discover_tests()`（tests/framework 框架）逐用例注册进 ctest。用例数为 **ctest 注册口径**（本轮实测 632，逐目标分布见下表；与静态 `^(TEST|TEST_F)\(` 宏计数一致：585 + 47 = 632）；断言数为静态统计（`(EXPECT|ASSERT)_[A-Z0-9_]+` 宏 token 计数，含 `EXPECT_CALL_COUNT` 调用；口径为 `tests/**/*.cpp`，不含框架头文件中的宏定义；本轮实测 2154）。

## 目标清单

| 目标 | 文件 | 用例 | 断言 |
|------|------|------|------|
| mcp-core-tests | JsonRpcTests、McpTypesTests、JsonParserTests、LogTests | 122 | 182 |
| mcp-wire-codec-tests | WireCodecTests、SessionHandlerTests、SpanHookTests | 51 | 215 |
| mcp-server-tests | McpServerTests | 48 | 263 |
| mcp-client-tests | McpClientTests | 52 | 232 |
| mcp-oauth-tests | OAuthTests | 16 | 33 |
| mcp-transport-tests | TransportTests、MessageChannelTests | 12 | 49 |
| mcp-net-tests | NetStackTests、WebSocketClientTests | 25 | 54 |
| mcp-http-tests | HttpServerTests | 35 | 152 |
| mcp-message-filter-tests | MessageFilterTests | 5 | 12 |
| mcp-token-cache-tests | FileTokenCacheTests | 5 | 19 |
| mcp-task-store-tests | FileTaskStoreTests | 9 | 29 |
| mcp-event-store-tests | FileEventStoreTests | 10 | 44 |
| mcp-streamable-http-tests | StreamableHttpTransportTests、SessionStoreTests | 40 | 157 |
| mcp-websocket-tests | WebSocketTransportTests | 2 | 5 |
| mcp-uri-template-tests | UriTemplateTests | 21 | 133 |
| mcp-integration-tests | ClientServerRoundTrip、Phase1InteropTests、Phase2ClosureTests | 19 | 113 |
| mcp-conformance-tests | ProtocolConformance | 123 | 386 |
| mcp-framework-self-tests | SelfTests | 37 | 76 |
| **合计** | | **632** | **2154** |

## 测试注意点

- URI 模板（RFC 6570）匹配器由 `mcp-uri-template-tests`（[UriTemplateTests.cpp](../tests/unit/UriTemplateTests.cpp)，21 用例 / 133 断言）覆盖：算子与变量列表解析、非法模板拒绝（括号不平衡/非法变量名/前缀修饰符/双贪婪变量/相邻无分隔变量/长度与变量数上限）、`Expand` 与 `Match` 往返、pct-encoded 解码、贪婪方向与超长 URI 拒绝；实现说明见 [/concepts/uri-template.md](/concepts/uri-template.md)
- `InMemoryTransport` 是**同步**的：消息在 `Send()`/`AsyncReceive()` 时交付（`MessageChannel` 有界队列，默认 64），无外部事件循环
- 集成测试超时护栏：`MCP_RUN_WITH_TIMEOUT`（[McpTimeout.hpp](../tests/framework/include/mcp/test/McpTimeout.hpp)，默认 10s，可 `(timeout, body)` 覆盖）为唯一入口，三个集成文件（ClientServerRoundTrip/Phase1InteropTests/Phase2ClosureTests）的本地重复实现已删除；超时记为该用例失败并 `MarkAbandoned()`——跳过 TearDown、故意泄漏 fixture 对象（防挂起工作线程访问已析构对象 UAF）、runner 继续执行后续用例，ctest 侧另有逐用例 `TIMEOUT 120` 兜底（[McpDiscoverTests.cmake.script](../cmake/McpDiscoverTests.cmake.script)）
- 关键协议行为有盲区守护测试：`RejectsRequestsBeforeInitialized`、`InitializeEchoesClientVersion`（回显旧版版本号）、`ProgressNotificationExtendsDeadline`、`IncomingFilterInterceptsRequests`
- `WireCodec::ValidateResponse`/`StampOutgoingRequest` 生产代码无调用者但**有测试守护**——不是死代码，勿删；`WireCodec::ExtractIncomingMeta` 已不再有派生实现（基类默认返回 nullopt，WireCodec 版测试 `Rev2026ExtractMeta` 已删）——meta 提取现由 `McpSessionHandler::ExtractIncomingMeta` 承担（有真实调用者），守护测试为 `IncomingMetaCarriesProtocolVersion`
- 2026 时代相关新增测试：`PingRejectedIn2026`/`PingAvailableIn2025`（SessionHandler）、`Rev2025ValidateInitializeRequest( +MissingProtocolVersion)`（WireCodec）、`Rev2026EncodeResultFlattensCacheHint`/`Rev2025EncodeResultKeepsCacheHintNested`、`AutoNegotiationCorrectsVersionOnSharedVersion`/`AutoNegotiationFallsBackWhenOnlyLegacySupported`（McpClient）；旧名 `Rev2026HasTaskAndSubscriptionNotifications` 已更名 `Rev2026HasMessageAndSubscriptionNotificationsNoTasks`
- 第一期互操作测试（[Phase1InteropTests.cpp](../tests/integration/Phase1InteropTests.cpp)，4 用例）：InMemory 与 HTTP 的 progress 双向（服务端 `SendProgress` → 客户端 `on_progress`）、客户端 `SendRootsListChanged`、GET SSE 接收流收 `tools/list_changed`；另与 TS conformance 服务器（2025-11-25 legacy）人工联调 7/7 通过（协商、tools/list、progress 0/50/100、roots 通知、logging 通知）
- 第二期收尾测试（[Phase2ClosureTests.cpp](../tests/integration/Phase2ClosureTests.cpp)，7 用例）：任务全生命周期与运行中取消（InMemory/HTTP 双形态）、URL elicitation 往返（InMemory/HTTP）、`ListToolsAll` 翻页聚合、`max_total_timeout` 截断请求
- 期二关键守护测试：`RequireInitialized` era 门控（modern era 未初始化放行 / legacy 拒绝）、任务化 tools/call 立即返回 `CreateTaskResult` + 状态通知流转、`tasks/cancel` 终态不迁移、`RequestContext::IsCancellationRequested` 协作取消、404 会话自愈重放恰一次（SessionExpired）、discover `serverInfo` 容缺与 `supportedVersions` 交集、`ElicitResult` wire 键 `content`、发送侧独立 POST 与接收侧边读边分发（StreamableHttp 传输层）
- 本地行为变更的守护测试（现役用例）：`StreamableHttpTest.ClientOmitsMcpParamHeaders`（未注解参数不生成 `Mcp-Param-*`，仅发 `Mcp-Method`/`Mcp-Name`）、`McpServerTest.RegisterResourceTemplateRejectsInvalidTemplate`（非法模板抛 `InvalidParams`）与 `ReadResourceMatchesRegisteredTemplate`/`ReadResourceTemplateValueIsPctDecoded`/`ReadResourcePrefersStaticOverTemplate`（模板实例 URI 路由、pct 解码、静态优先）、`Conformance.InputRequestsSpecSampleRoundTrip`/`InputResponseValuesAreBareResults`/`InputRequestsRejectsEntryWithoutMethod`/`InputRequestsAcceptsEntryWithoutParamsObject`/`InputRequestsRejectsEntryWithNonObjectParams`（MRTR 线上结构契约）、`McpClientTest.MrtrEchoesServerAssignedInputRequestKeys`/`MrtrUnknownInputRequestMethodThrows`（按 `method` 分派与未知 method 报错）
- SEP-2243 参数头测试覆盖 `x-mcp-header` 的嵌套属性路径、非法 schema、标准 Base64 哨兵编码、服务端与 body 一致性校验、服务端结果响应头镜像，以及客户端只镜像已注解参数；`ClientOmitsMcpParamHeaders` 仅验证未注解参数不生成头
- Span hook 测试覆盖未注入回归、服务端请求/传输边界、trace context 传递和拒绝请求的 Begin/End 配对，详见 [/concepts/span-hooks.md](/concepts/span-hooks.md)
- 期三关键守护测试：Bearer 鉴权 9 用例（无头/非 Bearer 挑战、invalid_token、insufficient_scope、有效 token 透传、元数据端点匿名、未配置零回归、客户端 `auth_challenge_handler` 闭环成功与 scope 不足）；FileEventStore 10 用例（双实例共享、重启恢复、并发写、trim、净化文件名等）；SessionStore 5 用例（外部接管、未知 id 404 `-32009`）；MRTR 端到端 3 用例（`MrtrToolInputRequiredElicitationRoundTrip`、`MrtrMintedRequestStateRoundTrip`、`MrtrForgedRequestStateRejectedWithReason`）；`SendLoggingMessageEncodesLevelAsString`（wire 真 bug 回归）；`ListPromptsIncludesArguments`；`CompleteRequestParamsStandardShape`/`CompleteRequestParamsFlatShapeCompat`；显式能力声明 2 用例
- 非 ctest 基准：`tests/bench/TransportBench.cpp` 构建 `mcp-transport-bench`，覆盖 HTTP 头解析、body 吞吐、并发、延迟响应和连接复用；基准不计入上述 18 个测试目标和 632 个 ctest 用例

## 自研框架能力（P0+P1）

- **致命断言**：`ASSERT_*` 与 `FAIL()` 记录失败后置 fatal 标志并立即 `return`，可用 `HasFatalFailure()` 查询；`ASSERT_NO_FATAL_FAILURE`/`EXPECT_NO_FATAL_FAILURE` 包裹子过程；`ASSERT_DOUBLE_EQ`、`EXPECT/ASSERT_FLOAT_EQ`、`EXPECT/ASSERT_NEAR` 补齐浮点族。比较宏经 `CompareOperands` 单次求值（双求值修复，自测 `ExpectCompareEvaluatesOperandsOnce*` 守护）
- **调试定位**：`SCOPED_TRACE(expr)`（[McpTrace.hpp](../tests/framework/include/mcp/test/McpTrace.hpp)，thread_local 栈）把 `Trace: file:line: msg` 调用链自动附加到失败消息；`ToString` 优先级为 ADL `PrintTo` → `operator<<` → 容器展开（`begin/end` 可迭代类型：vector/map/pair 实测输出 `[1, 2, 3]`/`[("a", 1)]`/`(1, "x")`）→ 类型名兜底，另有 `PrintToString`
- **扩展断言**：`STRNE`、`STRCASEEQ`/`STRCASENE`（ASCII 大小写不敏感）、`ANY_THROW`、`THROW_MSG`（异常类型 + 消息子串）、`ADD_FAILURE`/`ADD_FAILURE_AT`/`SUCCEED`；匹配器断言 `EXPECT_THAT`/`ASSERT_THAT` 支持 `Eq`/`HasSubstr`/`SizeIs`/`ElementsAre`
- **运行控制**：`--gtest_filter` 支持 `:` 多模式与 `-` 取反（glob `*`/`?`，匹配 `Suite.Case`，如 `'McpClientTest.*:TransportTest.Fake*'`）；`--gtest_list_tests`（gtest 风格）与 `--list-tests`（`Suite.Case` 行）；`--gtest_repeat=<n>`（n>=1）；`--gtest_shuffle`/`--gtest_random_seed=<n>`（同套件保持连续，seed 0 取时钟并打印）；`--gtest_break_on_failure`（首个失败即 abort）；`--gtest_output=json:<path>`（跨轮次汇总 status/elapsed_ms/properties，失败用例附 `failures` 消息数组（内含 SCOPED_TRACE 调用链））；`DISABLED_` 前缀自动跳过
- **生命周期**：`GTEST_SKIP()` 抛 `SkipException`，在 SetUp 中跳过整用例（body 不执行、TearDown 仍执行），body 中跳过同样记为 SKIPPED；`SetUpTestSuite`/`TearDownTestSuite` 经 TEST_F 注册钩子每套件恰执行一次；`Environment` 全局环境（`AddGlobalTestEnvironment`，SetUp 先于全部用例、TearDown 逆序收尾）；`RecordProperty` + `AddTestEndListener`（`TestResult{suite,name,passed,skipped,elapsed_ms}`）
- **归因**：`CurrentTest` 先取 thread_local、回退全局原子指针，子线程内断言仍归因到当前用例
- **测试替身**：[TestFakes.hpp](../tests/unit/TestFakes.hpp) 的 `FakeTransport`（`Sent()` 记录序列化出站消息、`PushIncoming` 注入入站、`LastSent`/`Closed`/`Started`）与 `EXPECT_CALL_COUNT`（按 `Sent().size()` 断言），支撑无 IO 传输行为测试
- **用例数变化**：自测 14 → 37（新增 23 用例：双求值修复与致命语义、SCOPED_TRACE、跳过/套件钩子/全局环境、扩展断言/匹配器/ToString、属性与结束监听）；mcp-transport-tests 11 → 12（`FakeTransportRecordsSends`）

## 官方 conformance suite（.github/workflows/conformance.yml）

- referee pin `@modelcontextprotocol/conformance@0.2.0-alpha.11`，spec 版本单源 `CONFORMANCE_SPEC_VERSION` 环境变量（workflow env 定值 `2025-11-25`，`scripts/run-conformance.sh` 同名 env 默认兜底同值），server/client 双 leg 均用该变量传 `--spec-version`（legacy 有状态 wire；modern 场景因 applicability 窗口外被 referee skip，属运行配置排除而非逐条偏差）
- **退出码语义由 referee `--expected-failures` 机制保证**（基线 `tests/conformance/baseline.yaml`）：基线外失败 → exit 1 真回归；过期基线条目（实现落地）→ exit 1 必须删条目——基线只减不增；新增场景一律按实跑结果逐条归因后才入基线，绝不预先臆测
- server fixture：`examples/conformance/server`（`conformance-server`，`MCP_BUILD_CONFORMANCE=ON`），legacy 2025-11-25 有状态，28 工具 / 4 资源（含 1 URI 模板）/ 5 提示词全集；client 驱动：`examples/conformance/client`（`conformance-client`，随 `MCP_BUILD_EXAMPLES`），11 场景注册表，`MCP_CONFORMANCE_SCENARIO` env + argv[1] URL，退出码回报结果
- server leg 实测：**Baseline check passed**（exit 0）；`resources-templates-read` 缺陷已修复并从基线删除（referee 输出 `resources-templates-read: 2 passed, 0 failed`）——`RegisterResourceTemplate` 拒绝非法模板、`resources/read` 按 RFC 6570 匹配模板实例 URI 后路由到 `template_handler`；基线现存 server 2 条（`tools-call-with-progress` SEP-2260 缺口、`server-sse-multiple-streams` 传输层多流缺口）；首轮 29 failed → SSE 头重复修复 → 6 failed → 基线收口
- client leg 实测：**Baseline check passed**（exit 0）；client 基线 36 → 37 条，含新增 `sse-retry` 已知缺口条目——server 关流后未等待 retry 字段声明的 500ms 即重连（实测 -70ms，要求 >=450ms 且 <=1000ms），另有重连未携带 Last-Event-ID 的 WARNING/SHOULD；实现 retry 等待与 Last-Event-ID 后删除该条目（详见 [/changelog/2026-09-15-log.md](/changelog/2026-09-15-log.md)）
- CI 配置：server/client 双 job `node-version: 22`（referee 依赖 Node 22+ 的 `fs.globSync`）；server 就绪探测为 POST `initialize`（`probe_ready()`，`--max-time 2`）——旧 `curl GET /mcp` 探测会建立 SSE 长流，`--max-time` 必然超时（退出码 28）
- 本地复现：`scripts/run-conformance.sh server|client`（就绪探测与 workflow 一致，POST `initialize`）

## 相关页面

- [/build.md](/build.md) — 构建与运行测试
- [/modules/protocol.md](/modules/protocol.md) — 测试守护的核心行为
- [/transports/in-memory.md](/transports/in-memory.md) — 同步测试传输
