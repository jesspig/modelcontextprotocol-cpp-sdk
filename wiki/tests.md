---
type: Test Suite
title: 测试体系
description: 18 个测试目标（15 unit + integration/conformance/framework），715 个测试用例（ctest 注册口径，自研测试框架）；静态断言统计 2857；支持致命断言、SCOPED_TRACE、匹配器、过滤/重复/乱序/JSON 报告与超时护栏。
tags: [test, framework, ctest, conformance]
timestamp: 2026-10-12T01:00:00+08:00
resource: tests/CMakeLists.txt
---

# 测试体系

全部经自研 `mcp_discover_tests()`（tests/framework 框架）逐用例注册进 ctest。用例数为 **ctest 注册口径**（本轮实测 715，逐目标分布见下表；与静态 `^(TEST|TEST_F)\(` 宏计数一致：665 + 50 = 715）；断言数为静态统计（`(EXPECT|ASSERT)_[A-Z0-9_]+` 宏 token 计数，含 `EXPECT_CALL_COUNT` 调用；口径为 `tests/**/*.cpp`，不含框架头文件中的宏定义；本轮实测 2857）。

目标数维持 18：本分支（tasks/skills/HTTP 加固/OAuth/conformance）全部测试并入既有目标，未新增目标——与原计划 20 目标的差异原因即此（任务存储测入 `mcp-task-store-tests`、技能类型测入 `mcp-core-tests`、服务端技能/任务分流测入 `mcp-server-tests`、客户端技能/任务守卫测入 `mcp-client-tests`、头/会话/重试测入 `mcp-streamable-http-tests` 与 `mcp-transport-tests`）。G 系列冻结批同样无新目标（T11b 基线 691 → 本轮 715，增量并入 `mcp-server-tests` +9、`mcp-client-tests` +7、`mcp-streamable-http-tests` +5、`mcp-wire-codec-tests` +2、`mcp-core-tests` +1）。

## 目标清单

| 目标 | 文件 | 用例 | 断言 |
|------|------|------|------|
| mcp-core-tests | JsonRpcTests、McpTypesTests、JsonParserTests、LogTests | 142 | 292 |
| mcp-wire-codec-tests | WireCodecTests、SessionHandlerTests、SpanHookTests | 57 | 261 |
| mcp-server-tests | McpServerTests | 66 | 535 |
| mcp-client-tests | McpClientTests | 65 | 304 |
| mcp-oauth-tests | OAuthTests | 17 | 42 |
| mcp-transport-tests | TransportTests、MessageChannelTests | 14 | 60 |
| mcp-net-tests | NetStackTests、WebSocketClientTests | 25 | 54 |
| mcp-http-tests | HttpServerTests | 37 | 161 |
| mcp-message-filter-tests | MessageFilterTests | 5 | 12 |
| mcp-token-cache-tests | FileTokenCacheTests | 5 | 19 |
| mcp-task-store-tests | FileTaskStoreTests | 11 | 80 |
| mcp-event-store-tests | FileEventStoreTests | 11 | 48 |
| mcp-streamable-http-tests | StreamableHttpTransportTests、SessionStoreTests | 58 | 276 |
| mcp-websocket-tests | WebSocketTransportTests | 2 | 5 |
| mcp-uri-template-tests | UriTemplateTests | 21 | 133 |
| mcp-integration-tests | ClientServerRoundTrip、Phase1InteropTests、Phase2ClosureTests | 19 | 113 |
| mcp-conformance-tests | ProtocolConformance | 123 | 386 |
| mcp-framework-self-tests | SelfTests | 37 | 76 |
| **合计** | | **715** | **2857** |

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
- 非 ctest 基准：`tests/bench/TransportBench.cpp` 构建 `mcp-transport-bench`，覆盖 HTTP 头解析、body 吞吐、并发、延迟响应和连接复用；基准不计入上述 18 个测试目标和 715 个 ctest 用例
- 本分支新增守护测试（均并入既有目标）：任务扩展——`WireCodec` 双时代集合与 `ValidateTaskHeaders` 缺 taskId/头失配、`McpServer` modern 头校验与通知抑制/`tasks/update` inputResponses 分支（互斥/状态门）/`tasks/result|list` 现代拒绝、任务存储新字段 roundtrip 与 `AdvanceTaskWithInput` 状态门、`McpClient` 未声明扩展收 task 抛 `HandlerError`/`UpdateTask` 第三参、`CreateTaskResult` 顶层/嵌套双形态；技能扩展——`Skill/SkillResources` 序列化 roundtrip、`RegisterSkill` 非法 uri 拒绝、目录视图派生与 `directoryRead` 门、客户端技能缓存（`use/bypass/refresh` + `max_age_ms`）；HTTP 加固——未知版本头 400 `-32022`、任务头 400、`initialize` 满存 503、会话闲置过期与 `SweepExpired`、EventStore 扫描追加与 10000 上限、监听流 5xx `GivenUp` 与 `retry:` 钳制（450–1000ms）、SSE `FlushSseBuffer` 尾块补派发；OAuth——`application_type` 注册透传、token `resource` 持久化与失配门、issuer 变更清注册/令牌、challenge 加固（超长/非法 URL 拒绝不抛）；服务端 audience 失配 401、客户端 403 仅 `insufficient_scope` 重试
- 本批（G 系列冻结批）新增守护测试（均并入既有目标，无新目标）：服务端——`CreateTaskResultFlatNoNested`（2026 扁平无嵌套）/`TaskTimestampsPopulated`（创建即置双时间戳）/`ToolErrorMapsToCompleted`（`is_error` 结果入库落 Completed）/`ProtocolErrorMapsToFailedWithError`（`McpError` 按原码内联 error 落 Failed）/`PromptsGetInputRequired`/`ResourceReadInputRequired`（非工具 input_required + 自动签名）/`MetaReachesHandler`（顶层 `_meta` 合并进 params）；客户端——`FlatTaskResultPreferredOverNested`（双形态反序列化顶层优先）/`ElicitationApplyDefaults*`（`applyDefaults` 缺省回填：五字段/禁用透传/保留既有/拒绝忽略/嵌套与变体递归）/`MrtrElicitationApplyDefaultsFillsInputResponses`；传输——`RequestStreamHoldOpenDeliversProgressBeforeResult`/`RequestStreamHoldOpenDeliversMessageBeforeResult`（modern POST 常开按 token 路由缓冲）/`PlainRequestKeepsSingleWriteAndClose`（普通请求仍单写即关）

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
- **退出码语义由 referee `--expected-failures` 机制保证**（基线 `tests/conformance/baseline.yaml`，本轮共 33 条：server 1 + client 32 = `http-custom-headers` 1 + auth 31）：基线外失败 → exit 1 真回归；过期基线条目（实现落地）→ exit 1 必须删条目——基线只减不增；新增场景一律按实跑结果逐条归因后才入基线，绝不预先臆测
- server fixture：`examples/conformance/server`（`conformance-server`，`MCP_BUILD_CONFORMANCE=ON`），stateful 28 工具 / 4 资源（含 1 URI 模板）/ 5 提示词全集；stateless 另增 `SkillsAndTasks.cpp`（3 技能：weather/greeter manifest + dynamic 动态 + `SetSkillsDirectoryRead(true)`，4 任务工具：greet 同步 + slow_compute/failing_job/protocol_error_job 任务模式，`FileTaskStore` 落临时目录）与 MRTR/Content 修补（`_meta` 合并入 `Params().meta` 后 fixture 改读 `Params().meta` 取 progressToken/logLevel/clientCapabilities，不再走 `GetRequest().meta` 旁路）；`InputRequiredResultPrompt` 改两阶段（首轮回 `input_required` + `request_state`，次轮带上下文渲染）；client 驱动：`examples/conformance/client`（`conformance-client`，随 `MCP_BUILD_EXAMPLES`），15 场景注册表（含新增 `sep-2640-client-no-prefetch/verify-digest/verify-size/verify-frontmatter` 技能场景），`MCP_CONFORMANCE_SCENARIO` env + argv[1] URL，退出码回报结果
- server leg 实测：**2025 leg 绿**（31 场景；GATE1 主会话结论，本任务未复跑）；**2026 leg 红**（GATE1 主会话结论：modern requirements 23 个 scored 失败，归因表见主会话收口，本任务未复跑）：G 系列冻结批已解决旧 4 缺口中的 3 个——input-required（`resources/read` 与 `prompts/get` 支持 `InputRequiredResult` + 自动签名，`PromptsGetInputRequired`/`ResourceReadInputRequired` 单测守护）、sep-2164（data 通道修补后 `data.uri` 回声）、tools-call-with-progress-modern（modern POST 常开 + progress/logging 按 token 路由缓冲进同一 SSE 流，`RequestStreamHoldOpen*` 单测守护；legacy leg 早已绿）；剩余生产缺口为 server-stateless（`_meta` 缺失拒收的 400 映射/-32020 对 -32022/initialize 现代 404/-32021 能力拒收/subscriptions/listen 流）
- client leg 实测（本任务 2026-10-12 于 Windows 复跑，脚本修复后直跑）：**2025 leg 红**（core 集 `--spec-version 2025-11-25`，6 passed / 48 failed，exit 1）：unexpected 唯一 `elicitation-sep1034-client-defaults` 0/5（string/integer/number/enum/boolean 五项缺省均未生效）——G 批实现 `applyDefaults` 后删基线条目，但 referee 场景仍失败，疑 fixture 侧未声明 `applyDefaults` 能力致客户端回填短路（推测，待主会话裁决：恢复条目或补 fixture 声明）；`sse-retry` 3/3 通过（G8b 删除证据再确认）；14 个 auth `Unknown scenario` exit 1 由基线覆盖。**2026 leg 红**（`--requirements 2026-07-28` 39 场景，两轮复跑 run1 43 passed / 78 failed、run2 `--verbose` 42 passed / 79 failed，均 exit 1）：unexpected 为 `sep-2322-client-request-state` 0/5（5 项 MRTR 客户端检查全败；2026-10-11 的 5/0 删条目证据作废，疑 G 批 request_state 链路改动致回归，待主会话裁决）与 `json-schema-ref-no-deref` 0/1（新场景，未归因、未入基线，基线只减不增）；25 auth + `http-custom-headers` 由基线覆盖（后者 Windows 下仍 `invalid UTF-8` 解析失败 + 30s 超时，条目继续保留；CI Linux 报 stale 即删）；其余非 auth（tools_call、request-metadata、http-standard-headers、http-invalid-tool-headers、json-schema-2020-12-preservation）全绿。保留条目：`server-sse-multiple-streams`（跨 leg 保留：legacy suite 内 skip 以 WARNING 记账，无条目 legacy 退出 1；modern 2/2 通过但 invocation 本就为红；fixture 不设 session_store 防 DELETE 毒化后续场景）
- CI 配置：server/client 双 job `node-version: 22`（referee 依赖 Node 22+ 的 `fs.globSync`）；server 就绪探测为 POST（`probe_ready()`，`--max-time 2`）——旧 `curl GET /mcp` 探测会建立 SSE 长流，`--max-time` 必然超时（退出码 28）
- 本地复现：`scripts/run-conformance.sh server|client`（`CONFORMANCE_SPEC_VERSION` 默认 `2025-11-25`，`2026-07-28` 时自动切 stateless/requirements/probe）；GATE1 修一处 client leg 缺陷：`--command` 此前传相对路径，经 referee `shell:true` spawn 在 Windows 下全败，现转绝对路径（`resolve_binary` 定位逻辑不动，与 CI 工作流的 `${GITHUB_WORKSPACE}` 绝对路径一致），server leg 直启不受影响

## 相关页面

- [/build.md](/build.md) — 构建与运行测试
- [/modules/protocol.md](/modules/protocol.md) — 测试守护的核心行为
- [/transports/in-memory.md](/transports/in-memory.md) — 同步测试传输
