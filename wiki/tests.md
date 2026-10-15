---
type: Test Suite
title: 测试体系
description: 18 个测试目标（15 unit + integration/conformance/framework），874 个测试用例（静态 `TEST/TEST_F/TEST_P` 口径，自研测试框架）；静态断言统计 3762；支持致命断言、SCOPED_TRACE、匹配器、过滤/重复/乱序/JSON 报告与超时护栏。
tags: [test, framework, ctest, conformance]
timestamp: 2026-10-15T13:14:29+08:00
resource: tests/CMakeLists.txt
---

# 测试体系

全部经自研 `mcp_discover_tests()`（tests/framework 框架）逐用例注册进 ctest。用例数为**静态口径**：逐文件行首 `TEST(`/`TEST_F(`/`TEST_P(` 计数（`git grep -c -E '^[[:space:]]*(TEST|TEST_F|TEST_P)\('`，口径为 `tests/**/*.cpp`），现为 **874**，与 `ctest -N` 注册口径精确一致（873 → 本轮 +1）；断言数同为静态统计（`\b(EXPECT|ASSERT)_[A-Z0-9_]+` 宏 token 计数，含 `EXPECT_CALL_COUNT` 调用，不含框架头文件中的宏定义），现为 **3762**（3757 → 本轮 +5）。本轮增量：`StreamableHttpTransportTests` +1 用例 / +5 断言（`NotificationBroadcastWaitsForSseListener`：通知在无 SSE 监听者时有界等待、监听者注册后投递——守护 SSE 流注册竞态修复，无修复时两处断言均失败）。注：10-14 两轮（JsonValue 迭代式析构、span 拒收修复）未同步本页，本轮按实测逐行回填表格（`mcp-core-tests` 179→182 / 390→399、`mcp-oauth-tests` 225→226 系前两轮遗留偏差，非本轮增量）。全量 `ctest`（串行，`--timeout 120`）874 注册：本机 873 通过——唯一未过 `FileEventStoreTest.ConcurrentAppendKeepsOrder` 为本机文件锁环境波动（修复前对照同样挂起，与本轮无关）；CI 常规环境 874/874 全绿（见 [/changelog/2026-10-15-log.md](/changelog/2026-10-15-log.md)），以后者为准。`a7ab6da` 未新增用例（`git diff-tree` 未碰本页与测试代码，总数不动）；`7005b81` 碰本页，即本轮 +1 用例 / +5 断言。

目标数维持 18：本分支（tasks/skills/HTTP 加固/OAuth/conformance）全部测试并入既有目标，未新增目标——与原计划 20 目标的差异原因即此（任务存储测入 `mcp-task-store-tests`、技能类型测入 `mcp-core-tests`、服务端技能/任务分流测入 `mcp-server-tests`、客户端技能/任务守卫测入 `mcp-client-tests`、头/会话/重试测入 `mcp-streamable-http-tests` 与 `mcp-transport-tests`）。G 系列冻结批同样无新目标（T11b 基线 691 → 本轮 715，增量并入 `mcp-server-tests` +9、`mcp-client-tests` +7、`mcp-streamable-http-tests` +5、`mcp-wire-codec-tests` +2、`mcp-core-tests` +1）。spec-20260728-tier1-parity 批同样无新目标（715 → 748，+33 并入 `mcp-server-tests` +18、`mcp-streamable-http-tests` +7、`mcp-task-store-tests` +4、`mcp-client-tests` +3、`mcp-core-tests` +1；断言 2857 → 3145，+288）；其后 748 → 769（前瞻六开关批，见下）→ 874（+105，后续批次，同样全部并入既有目标，目标数仍 18，分布见上表；715 → 748 起始基线见 [/changelog/2026-10-12-log.md](/changelog/2026-10-12-log.md)）。

## 目标清单

| 目标 | 文件 | 用例 | 断言 |
|------|------|------|------|
| mcp-core-tests | JsonRpcTests、McpTypesTests、JsonParserTests、LogTests | 182 | 399 |
| mcp-wire-codec-tests | WireCodecTests、SessionHandlerTests、SpanHookTests | 65 | 307 |
| mcp-server-tests | McpServerTests | 95 | 807 |
| mcp-client-tests | McpClientTests | 76 | 375 |
| mcp-oauth-tests | OAuthTests | 52 | 226 |
| mcp-transport-tests | TransportTests、MessageChannelTests | 15 | 62 |
| mcp-net-tests | NetStackTests、WebSocketClientTests | 26 | 55 |
| mcp-http-tests | HttpServerTests | 41 | 179 |
| mcp-message-filter-tests | MessageFilterTests | 5 | 12 |
| mcp-token-cache-tests | FileTokenCacheTests | 5 | 19 |
| mcp-task-store-tests | FileTaskStoreTests | 15 | 113 |
| mcp-event-store-tests | FileEventStoreTests | 11 | 48 |
| mcp-streamable-http-tests | StreamableHttpTransportTests、SessionStoreTests | 84 | 447 |
| mcp-websocket-tests | WebSocketTransportTests | 2 | 5 |
| mcp-uri-template-tests | UriTemplateTests | 21 | 133 |
| mcp-integration-tests | ClientServerRoundTrip、Phase1InteropTests、Phase2ClosureTests | 19 | 113 |
| mcp-conformance-tests | ProtocolConformance | 123 | 386 |
| mcp-framework-self-tests | SelfTests | 37 | 76 |
| **合计** | | **874** | **3762** |

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
- 非 ctest 基准：`tests/bench/TransportBench.cpp` 构建 `mcp-transport-bench`，覆盖 HTTP 头解析、body 吞吐、并发、延迟响应和连接复用；基准不计入上述 18 个测试目标和 874 个用例
- 平台差异：Windows 874 / Linux 873，差 1 例为 `StreamableHttpTransportTest.PostSseStreamEofDeliversConnectionClosedError`（`#ifdef _WIN32` 保护，[StreamableHttpTransportTests.cpp:717-718](../tests/unit/StreamableHttpTransportTests.cpp)），属平台条件编译的正常结果
- 本分支新增守护测试（均并入既有目标）：任务扩展——`WireCodec` 双时代集合与 `ValidateTaskHeaders` 缺 taskId/头失配、`McpServer` modern 头校验与通知抑制/`tasks/update` inputResponses 分支（互斥/状态门）/`tasks/result|list` 现代拒绝、任务存储新字段 roundtrip 与 `AdvanceTaskWithInput` 状态门、`McpClient` 未声明扩展收 task 抛 `HandlerError`/`UpdateTask` 第三参、`CreateTaskResult` 顶层/嵌套双形态；技能扩展——`Skill/SkillResources` 序列化 roundtrip、`RegisterSkill` 非法 uri 拒绝、目录视图派生与 `directoryRead` 门、客户端技能缓存（`use/bypass/refresh` + `max_age_ms`）；HTTP 加固——未知版本头 400 `-32022`、任务头 400、`initialize` 满存 503、会话闲置过期与 `SweepExpired`、EventStore 扫描追加与 10000 上限、监听流 5xx `GivenUp` 与 `retry:` 钳制（450–1000ms）、SSE `FlushSseBuffer` 尾块补派发；OAuth——`application_type` 注册透传、token `resource` 持久化与失配门、issuer 变更清注册/令牌、challenge 加固（超长/非法 URL 拒绝不抛）；服务端 audience 失配 401、客户端 403 仅 `insufficient_scope` 重试
- 本批（G 系列冻结批）新增守护测试（均并入既有目标，无新目标）：服务端——`CreateTaskResultFlatNoNested`（2026 扁平无嵌套）/`TaskTimestampsPopulated`（创建即置双时间戳）/`ToolErrorMapsToCompleted`（`is_error` 结果入库落 Completed）/`ProtocolErrorMapsToFailedWithError`（`McpError` 按原码内联 error 落 Failed）/`PromptsGetInputRequired`/`ResourceReadInputRequired`（非工具 input_required + 自动签名）/`MetaReachesHandler`（顶层 `_meta` 合并进 params）；客户端——`FlatTaskResultPreferredOverNested`（双形态反序列化顶层优先）/`ElicitationApplyDefaults*`（`applyDefaults` 缺省回填：五字段/禁用透传/保留既有/拒绝忽略/嵌套与变体递归）/`MrtrElicitationApplyDefaultsFillsInputResponses`；传输——`RequestStreamHoldOpenDeliversProgressBeforeResult`/`RequestStreamHoldOpenDeliversMessageBeforeResult`（modern POST 常开按 token 路由缓冲）/`PlainRequestKeepsSingleWriteAndClose`（普通请求仍单写即关）
- 实验开关门控矩阵（`ad547f9`，服务端 10 开关 / 客户端 3 开关默认全关，基线 748/3145 不动；详见 [/concepts/experimental-flags.md](/concepts/experimental-flags.md)；其后 769 → 874 桥接见下节，目标数始终 18）：

| 端 | 开关 | 关：可观测行为 |
|----|------|----------------|
| 服务端 | `enable_tasks_extension` | `tasks/get\|update\|cancel` → `MethodNotFound`，不声明 tasks 键；仅设 `task_store` 时隐式启用 + `Warning` |
| 服务端 | `enable_skills_extension` | `skills/list\|get` 与 `directory/read` → `MethodNotFound`，不声明 skills 键 |
| 客户端 | `enable_tasks_extension` | `UpdateTask`/`PollTaskToCompletion` 抛 `MethodNotFound`，声明判定恒 false |
| 客户端 | `enable_skills_extension` | `ListSkills`/`GetSkill`/`ReadDirectory` 抛 `MethodNotFound` |

既有任务/技能用例的开关适配：客户端 7 处已显式置 flag（tasks 4 处：`CallToolAsTaskReturnsTaskHandle`/`FlatTaskResultPreferredOverNested`/`PollTaskViaTasksGet`/`UpdateTaskCarriesInputResponses`；skills 3 处：`ListSkillsRoundTrip`/`SkillDirectFetchWithoutList`/`TtlExpiryRefetch`）；服务端仅 2 处（`MakeSkillsEnv` 置 skills 开、`SkillsDiscoverIncludesExtensionsAndCache` 双开），其余服务端任务用例仍走 `task_store` 隐式兼容路径（伴 `Warning`，0.3.8 计划移除）

前瞻六开关批次（748 → 769，+21：`mcp-server-tests` +8（文件输入 5 + 触发器 3）、`mcp-client-tests` +7（搜索 3 + ETag 4）、`mcp-streamable-http-tests` +3（DPoP 服务端）、`mcp-oauth-tests` +3（DPoP 客户端）；断言 3145 → 3287，+142；全量 `ctest --preset debug` 769/769 全绿；其后 769 → 874（+105，后续批次，同样并入既有目标，目标数仍 18，分布见上表）；详见 [/concepts/experimental-flags.md](/concepts/experimental-flags.md)）。

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

- referee pin `@modelcontextprotocol/conformance@0.2.0-alpha.12`（alpha 通道最新；requirements 内核与 alpha.11 一致，升级只收 referee 修补）；server/client 双 job 按 `spec` 矩阵（`2025-11-25` / `2026-07-28`，`fail-fast: false`）各跑双 leg：2025 leg 用 `--spec-version`（legacy 有状态 wire），2026 leg 用 `--requirements 2026-07-28`；conformance-server 以 `--stateless` 切换双模式（stateless 下不固定 `protocol_version`、挂 task_store + 全方法 cache_hints + 注册技能/任务 fixture；stateful 固定 `2025-11-25`），就绪探测按 leg 分化（legacy 探 `initialize`，modern 探 `server/discover`，兼作 liveness）
- **退出码语义由 referee `--expected-failures` 机制保证**（per-leg 基线映射，实测条目：`tests/conformance/baseline.yaml`（未知版本回退）5 条 = server 2（`server-sse-multiple-streams` + `server-stateless`）+ client 3（auth 3）；`baseline-2025-11-25.yaml`（legacy）2 条 = server 1 + client 1（`http-custom-headers`）；`baseline-2026-07-28.yaml`（modern）4 条 = server 1（`server-stateless`）+ client 3（auth 3））：基线外失败 → exit 1 真回归；过期基线条目（实现落地）→ exit 1 必须删条目——基线只减不增；新增场景一律按实跑结果逐条归因后才入基线，绝不预先臆测
- server fixture：`examples/conformance/server`（`conformance-server`，`MCP_BUILD_CONFORMANCE=ON`），stateful 28 工具 / 4 资源（含 1 URI 模板）/ 5 提示词全集；stateless 另增 `SkillsAndTasks.cpp`（3 技能：weather/greeter manifest + dynamic 动态 + `SetSkillsDirectoryRead(true)`，4 任务工具：greet 同步 + slow_compute/failing_job/protocol_error_job 任务模式，`FileTaskStore` 落临时目录）与 MRTR/Content 修补（`_meta` 合并入 `Params().meta` 后 fixture 改读 `Params().meta` 取 progressToken/logLevel/clientCapabilities，不再走 `GetRequest().meta` 旁路）；`InputRequiredResultPrompt` 改两阶段（首轮回 `input_required` + `request_state`，次轮带上下文渲染）；client 驱动：`examples/conformance/client`（`conformance-client`，随 `MCP_BUILD_EXAMPLES`），15 场景注册表（含新增 `sep-2640-client-no-prefetch/verify-digest/verify-size/verify-frontmatter` 技能场景），`MCP_CONFORMANCE_SCENARIO` env + argv[1] URL，退出码回报结果
- 基线现状（per-leg 文件为真源，`baseline.yaml` 仅作未知版本回退，条目逐字一致）：modern（`baseline-2026-07-28.yaml`）server 1（`server-stateless`，SHOULD 推送缺口）+ client 3（auth 兜底）；legacy（`baseline-2025-11-25.yaml`）server 1（`server-sse-multiple-streams`，跨 leg 保留保绿）+ client 1（`http-custom-headers`，Windows 非 ASCII 环境变量解析失败，Linux CI 预期通过）。四 leg 终态（server/client × legacy/modern）：**server legacy 连跑 3/3 `67 passed, 0 failed` + 基线通过；server modern 194 passed + 基线内 1 failed 通过；client legacy 233 passed / 0 failed（auth 段已清空）；client modern 450 passed + 基线内 9 failed**（`client-credentials-jwt` 1 + `dpop` 3 + `dpop-nonce` 5，本地无 OpenSSL 按既定契约抛 `InternalError`，正向路径待 CI runner 证实）；server 侧证据见 [/changelog/2026-10-15-log.md](/changelog/2026-10-15-log.md)，client 侧为 2026-10-13 实测（基线按 referee stale 报告删 8 条）；CI 双 job 已改 per-leg 基线映射（见 [/concepts/tier1-parity.md](/concepts/tier1-parity.md)）；B2 已证伪转 B3（基线保留）；生产缺口记账同见 tier1-parity 页
- 历史收敛：T11 四 leg 收口与 `gap-and-ci-green` 分支中间轮次计数已收敛为上表终态（T11 专项复核 request_state 5/5、`$ref` 1/1、schema-preservation 9/9 仍有效），明细与技术对标声明见 [/concepts/tier1-parity.md](/concepts/tier1-parity.md)（仅技术对标、无官方 Tier 申报意向）；auth fixture 场景仍走基线（单元已闭环，见 tier1-parity 页遗留记账）
- server leg 历史：2025 leg 绿（31 场景，GATE1 主会话结论，本任务未复跑）；2026 leg 曾红（modern requirements 23 个 scored 失败，归因表见主会话收口）：G 系列冻结批已解决旧 4 缺口中的 3 个（input-required 非工具双路径 + 自动签名、sep-2164 data 回声、tools-call-with-progress modern 持开缓冲；legacy leg 早已绿），剩余 server-stateless 生产缺口见 tier1-parity 页
- client leg 历史（2026-10-12 Windows 复跑，脚本修复后直跑；T11 收口结论见上表终态）：**2025 leg** unexpected 唯一 `elicitation-sep1034-client-defaults` 0/5（G 批删条目后仍失败，疑 fixture 未声明 `applyDefaults`，待主会话裁决），`sse-retry` 3/3 通过，auth 由基线覆盖；**2026 leg** unexpected 为 `sep-2322-client-request-state` 0/5（5/0 删除证据作废，疑 G 批链路回归，待主会话裁决）与 `json-schema-ref-no-deref` 0/1（新场景未归因、未入基线，基线只减不增），其余非 auth 全绿。保留条目 `server-sse-multiple-streams` 跨 leg 说明见基线注释- **本轮（2026-10-13）四 leg 终态实测**（主代理本地）：**client modern 450 passed + 基线内 9 failed**（`client-credentials-jwt` 1 + `dpop` 3 + `dpop-nonce` 5，全部因本地构建无 OpenSSL——ES256 签名与 DPoP proof 共用 OpenSSL 后端，无 OpenSSL 时按既定契约抛 `InternalError`，不伪造通过；正向路径待 CI runner 证实）；**client legacy 233 passed / 0 failed**（`http-custom-headers` 1 条 client 基线保留，auth 段已清空）；基线按 referee stale 报告删除 8 条（modern 删 `metadata-var1/var2/var3` + `authorization-server-migration`，legacy 删 `metadata-var1/var2/var3` + `scope-step-up`）。fixture 侧假绿（`did not_complete` 但同时被 referee 判绿）由 18 处降至 0 处参与环节，余 4 处 `iss-*` 为「fixture 主动拒绝错误 iss」的被测语义、referee 判绿，属正确行为。保留 6 条见 tier1-parity 页逐条归因
- **`auth/scope-retry-limit` modern leg 偶发挂起（已修）**：fixture 在 `server/discover timed out`（Auto 协商探测超时）后进程不退出，偶发、拖长单次 leg 约 6 分钟需强杀。**根因**（2026-10-14 定位）：`McpSessionHandler::max_total_timeout_` 默认 `0`（禁用，见 [McpSessionHandler.hpp:170](../../include/mcp/protocol/McpSessionHandler.hpp)）时 `absolute_deadlines_` 不登记，而 per-request deadline 会被服务端的 progress 通知不断延长（`ResetTimeoutByProgressToken`，该行为由 `McpClientTest.TotalTimeoutDisabledAllowsProgressExtension` 锁定为既定设计）——服务端只发 progress 不发最终响应时 request 永不超时，主线程永久等 future。**诊断方法**：`/proc/<pid>/task/*/wchan` 确认 4 线程 futex（健康等待）+ 1 线程 nanosleep（timeout 轮询）、无 I/O；把 fixture stderr 重定向到按 PID 命名的文件以绕过 referee 的 socket 全缓冲后，读到卡住进程真实日志——`CheckTimeouts` 持续运行但 `pending` 恒 1 且不超时、`OnResponse` 从未调用。**修复**：fixture 的 `ConnectBearerClient` 与 `ConnectDPoPClient` 显式设 `max_total_timeout = 10s`（< referee 30s 预算）。修复后 referee 正常收尾 EXIT=0，Close 链完整执行
- CI 配置：server/client 双 job `node-version: 22`（referee 依赖 Node 22+ 的 `fs.globSync`）；server 就绪探测为 POST（`probe_ready()`，`--max-time 2`）——旧 `curl GET /mcp` 探测会建立 SSE 长流，`--max-time` 必然超时（退出码 28）
- 本地复现：`scripts/run-conformance.sh server|client`（`CONFORMANCE_SPEC_VERSION` 默认 `2025-11-25`，`2026-07-28` 时自动切 stateless/requirements/probe）；GATE1 修一处 client leg 缺陷：`--command` 此前传相对路径，经 referee `shell:true` spawn 在 Windows 下全败，现转绝对路径（`resolve_binary` 定位逻辑不动，与 CI 工作流的 `${GITHUB_WORKSPACE}` 绝对路径一致），server leg 直启不受影响
- **`tools-call-with-progress` legacy 腿首条通知丢失（已修，2026-10-15）**：CI run 23（a351749）`conformance server (2025-11-25)` 腿 `1 passed, 1 failed`（unexpected，两 attempt 均如此），断言 "Expected at least 3 progress notifications, got 2"——首条 progress=0 被静默丢弃。**根因**：SSE 流注册竞态，`SendMessageAsync` 通知只广播给瞬间已注册客户端，注册在 GET 连接线程写 endpoint 帧之后（[HttpServerImpl.cpp:480](../src/http/HttpServerImpl.cpp)），裁判客户端异步开流后立即发 `tools/call`，工具入口零延迟的首条通知早于注册。**修复**：有界等待从仅 `IsRequest` 扩到 `IsRequest || IsNotification`（50ms 步长/2s 预算，modern hold-open 缓冲路径不受影响）。**修复后实测**（主代理本地，Windows）：server legacy 连跑 3/3 `67 passed, 0 failed` + 基线通过（修复前 4/4 失败）；上一全绿提交 ef1e26a 打同补丁后 2/2 同样 67/67；server modern `--requirements 2026-07-28` exit=0 基线通过（`tasks-lifecycle` 的 `sep-2663-tasks-get-status-failed` 1 失败为既有问题——无修复时 2/2 同样失败，且属 extension 不计分场景）。新增守护用例 `StreamableHttpTransportTest.NotificationBroadcastWaitsForSseListener`（无修复时两处断言均失败）。详见 [/changelog/2026-10-15-log.md](/changelog/2026-10-15-log.md) 与仓库根 `CI-失败分析-run-38557995243.md`

## 相关页面

- [/build.md](/build.md) — 构建与运行测试
- [/modules/protocol.md](/modules/protocol.md) — 测试守护的核心行为
- [/transports/in-memory.md](/transports/in-memory.md) — 同步测试传输
