---
type: Concept
title: Tier1 技术对标声明（2026-07-28）
description: 与 Tier1 五实现（TS v2.3.1 / Python v2.3.0 / C# v2.2.0 / Go v1.8.0 / Rust v3.5.1，spec 2026-07-28）的逐项技术对等状态；含 2026 协议修复批次（T-01..T-13）落点与 sampling legacy partial 判定；仅技术对标，无官方 Tier 申报意向；自研 WebSocket client 为非标超集。
tags: [tier1, parity, 2026-07-28, conformance, 对标]
timestamp: 2026-10-29T23:10:00+08:00
resource: tests/conformance/baseline-2026-07-28.yaml
---

# Tier1 技术对标声明（2026-07-28）

本页记录分支 `feature/tier1-parity-2026-07-28`（含 `fb559d5/ba1f8c2/53fbce0/40134f4/e3fa585/8ad053e/2f2fc8d/a581292/b7a613e/8d589ae` 与 `baseline-2026-07-28.yaml` 的 http-custom-headers 删除）相对 Tier1 五实现的**技术对等状态**。比较基准：TypeScript v2.3.1、Python v2.3.0、C# v2.2.0、Go v1.8.0、Rust v3.5.1，均以 spec `2026-07-28` 为准。

**仅技术对标、无官方 Tier 申报意向**：本页不构成向 MCP 官方申请 Tier 分级的申报材料，不承诺与任一 Tier1 实现逐行行为一致；差异处以本 SDK 源码与知识库为准。

## 对标总览

| 领域 | 对标结论 | 依据 |
|------|----------|------|
| 现代准入与协商（`server/discover` → `initialize` 回退、双 era 门控） | 对等 | [version-negotiation](/concepts/version-negotiation.md)；`fb559d5` 收敛现代准入 |
| MRTR（InputRequired 循环、`request_state`、`inputResponses`） | 对等 | [mrtr](/concepts/mrtr.md)；`fb559d5` 回填；conformance `sep-2322-client-request-state` 5/5 |
| JSON Schema 2020-12（含 `$ref`、schema 保持） | 对等 | `e3fa585` 补齐关键字；`json-schema-ref-no-deref` 1/1、`schema-preservation` 9/9 |
| 来源校验与资源护栏 | 对等 | `40134f4` |
| 传输（stateless 单连接、持开 listen 流、变更推送、订阅广播寻址） | 对等 | [streamable-http](/transports/streamable-http.md)；`53fbce0` + `8d589ae` |
| OAuth 客户端鉴权表面（CIMD 加固、audience 硬化） | 对等 | [oauth](/concepts/oauth.md)；`ba1f8c2` + `8ad053e` |
| Apps 扩展与 Server Card | Apps 对等；Server Card 仅服务端 | `2f2fc8d`；服务端可生成（`McpServer::GetServerCard()`）并经 `/server-card` 返回，客户端不消费，开关见 [experimental-flags](/concepts/experimental-flags.md) |
| 单连接语义与 trace 配对可观测 | 对等 | `a581292`；[span-hooks](/concepts/span-hooks.md) |
| 服务端现代任务用例形态 | 对等 | `b7a613e`（逐请求声明） |
| `http-custom-headers` 18/18 | 对等（modern 基线已删，legacy 条目保留待复跑） | `baseline-2026-07-28.yaml` 删除 10 行注释 + 条目；CI Linux 报 stale 即为变绿证据 |

## 2026 协议修复批次（T-01..T-13）

本轮 `feature/mcp-2026-protocol-fixes` 修复双 era 行为，逐项落点如下（依据为源码与守护测试；conformance 实测留给 T-15/T-17，本页不预填结果）：

| 领域 | 状态 | 落点 |
|------|------|------|
| 缓存提示默认与校验 | 对等 | modern 未配置时返回 `ttlMs=0`、`cacheScope="private"`；显式提示必须同时含两字段且取值合法（`ttlMs>=0`、scope 为 public/private），否则 `InvalidParams`；legacy 线格式不变（`74c143b`） |
| logging 门控 | 对等 | modern 请求无 `logLevel` 时 `RequestContext::Log` 与两参 `SendLoggingMessage` 静默；HTTP 无响应流时丢弃日志通知，stdio/InMemory 保持点对点（`e1c41df`） |
| serverInfo 回填 | 对等 | modern 结果 `_meta["io.modelcontextprotocol/serverInfo"]` 由会话层逐结果注入，保留调用方已有值；discover/initialize 统一走 `ResolveServerInfo`；legacy 不变（`eab0e18`） |
| roots 变更通知门控 | 对等 | `notifications/roots/list_changed` 移入 2025-only 集合；modern 客户端发送抛 `ProtocolViolation`，legacy（>= 2025-06-18）保持可发（`6eea5c6`） |
| completion 上下文 | 对等 | `CompleteRequestParams::context_arguments` 映射 `context.arguments`，序列化仅在非空时写入，反序列化校验对象与字符串值（`e026c08`） |
| ResourceLink | 对等 | 补齐 `name`（必填）、`description`、`mimeType`、`size`（非负）、`icons`；序列化/反序列化校验 name/uri 非空与 size 非负（`35f7efc`） |
| 客户端能力声明 | 对等 | `ClientOptions` 新增 `elicitation_handler`/`url_elicitation_handler`/`sampling_handler`/`roots_handler`，创建时安装；`DeriveClientCapabilities` 在协商前自动补声明 elicitation form/url、sampling、roots.listChanged，显式 `capabilities` 优先（`644efee`） |
| MRTR 默认自动 | 对等 | `input_required_config` 默认构造（`auto_fulfill=true`、10 轮）；显式 `std::nullopt` 或缺少对应 handler 时返回未履行的 `input_required` 结果（`9ac4df2`） |
| URL elicitation 三态 | 对等 | `UrlElicitationHandler` 返回 `ElicitResult`，action 限定 accept/decline/cancel，非法值抛 `HandlerError`；未配置 handler 默认 decline；legacy 保留 complete 通知，modern 走 MRTR（`0e30e48`） |
| 订阅推送 | 对等 | 运行期注册工具/资源/模板/提示词时向匹配订阅推送 list_changed；握手未完成不推送，未订阅静默，手动推送保留（`18c659b`） |
| 客户端控制面 | 对等 | 新增 `SendRequest`（缺省注入 client meta）、`SendNotification`、`CancelRequest`（`notifications/cancelled`）、`SetLoggingLevel`（仅 legacy，modern 抛 `ProtocolViolation`）（`1b582d9`） |
| tools/list 顺序 | 对等 | 工具容器改为按名称有序的 `std::map`，普通列表与渐进分页输出一致顺序（`1c75c67`） |
| private 缓存授权分区 | 对等 | private 键附授权上下文；`cache_auth_context` 变化与 `Close` 清空 private；public 跨上下文共享；`Invalidate` 跨上下文删同键（`16cfa93`） |
| sampling | legacy partial | `SamplingHandler` 仍可用但已废弃（SEP-2577），仅覆盖基本 form/单内容采样，modern 不视为完整能力；MRTR 中仅在 handler 存在时回应 `sampling/createMessage`；完整实现不在本轮范围，不列为本轮 Tier1 收口项 |

## conformance 四 leg（2026-10-29 复跑实测）

- server legacy：67 passed / 0 failed
- server modern：197 passed / 0 failed / 0 warnings；`server-stateless` 场景 30/0/0 warnings（listen 成功响应不再追加空 result、客户端以 acknowledged 完成订阅），modern leg 基线条目已清空
- client legacy：233 passed / 0 failed
- client modern：450 passed / 9 failed；9 项均为本地构建无 OpenSSL 的 DPoP/ES256 环境限制（`auth/client-credentials-jwt`、`auth/dpop`、`auth/dpop-nonce`），CI runner 具备 OpenSSL 时预期通过，不新增基线
- 专项：`request_state` 5/5、`$ref` 1/1、`schema-preservation` 9/9

当前测试基线为静态 **954** 用例 / **4429** 断言、17 目标（Windows `ctest -N` 注册 949，差 5 为条件编译用例），逐目标分布见 [/tests.md](/tests.md)。计数口径为 `ctest -N` 注册与静态统计，本页不声明全量 `ctest` 运行结果。conformance 结果为本轮四 leg 实测；基线条目以 per-leg 文件为准（未知版本回退 `baseline.yaml` 1 条、legacy `baseline-2025-11-25.yaml` 2 条、modern `baseline-2026-07-28.yaml` 0 条）。

## A-T2 auth 进展（fixture 真实流联调）

`examples/conformance/client/main.cpp`（+1143 行，分支 `feature/gap-and-ci-green` 的 `63c6c15/333130c`）为 client 夹具接上真实 OAuth 流：PRM-first 发现（`/.well-known/oauth-protected-resource` → `authorization_servers[0]` → RFC 8414 元数据）、CIMD 客户端标识（`http(s)://` `client_id` 拉元数据文档）与 DCR 回退（仅未配置 `client_id` 时注册）、授权码交换（`AuthCodeTokenFields` + `ManualToken`）、授权响应 `iss` 校验（缺 `iss`/错 issuer 拒绝）、`token-exchange`/`id-jag` 变体路径。`dpop`/`dpop-nonce` 场景明确标 untestable（逐请求 DPoP proof 需传输层支持，见下生产缺口记账）。

证据为 referee `stale`：转绿条目从两 per-leg 基线删除后复跑不再报缺条目（`baseline-2026-07-28.yaml` 删 21、`baseline-2025-11-25.yaml` 删 10；`84f931f/333130c`）。

## B2 证伪与 B3 收口（server-stateless 已转绿删除，server-sse-multiple-streams legacy 保留）

B2（fixture 侧按 listen 拓扑分流以消 `server-stateless`/`server-sse-multiple-streams` 基线）已证伪：listen 拓扑由 referee 控制，fixture 侧无法决定分流。B3 收口结果（2026-10-29 复跑）：`server-stateless` 以 30/0/0 warnings 转绿并删除（modern leg 基线清空）；`server-sse-multiple-streams` 仍因 legacy suite 内 skip 以 WARNING 记账，保留于 `baseline-2025-11-25.yaml` 保绿，零代码改动。

## CI 红根因与修复（三项）

1. DPoP 虚构 API（一行，已修，ubuntu proof 待 CI）：`src/ext/DPoP.cpp` 的 `ECDSA_SIG_get0_sig` 为虚构 API，改为 `ECDSA_SIG_get0`（`48134d8`）
2. inline 链接（已修，release + WERROR 本地证毕）：`src/http/StreamableHttpClientTransport.cpp` 的 `ReconnectDelayForAttempt` 去 `inline`（`48134d8`）
3. 深嵌套栈溢出（解析器改显式栈，已修）：`src/core/JsonParser.cpp` 递归下降（`ParseObject`/`ParseArray` 经 `depth_` 计数）改显式栈迭代（`ParseContainer` + `Frame` 栈，`kMaxDepth` 改按栈深判定）（`ac1d4a3`）；macOS Bus error 为我方 T6/T7 引入、现已修，windows SEGFAULT 早有、本次一并修好

CI 另改 per-leg 基线映射：`.github/workflows/conformance.yml` 的 server/client 双 job 按 `CONFORMANCE_SPEC_VERSION` 选择基线（2026-07-28 → `baseline-2026-07-28.yaml`、2025-11-25 → `baseline-2025-11-25.yaml`，未知回退 `baseline.yaml`）（`b1d529f`）；fallback 对齐同提交。

## 生产缺口记账（2026-10-13 逐条改判，未静默丢弃）

1. ~~token 响应无 `iss` 致 provider 流拿不到可用 token~~（**已修**）：原 `ValidateTokenIssuer` 要求 token 响应必须带 `iss`，而 MCP 规范与 RFC 9207 §2.4 只对**授权响应**的 `iss` 施加义务（后者明示「clients MAY accept 不带 iss，由本地策略决定」），属规范误引导致的过度收紧。改为「存在即校验」后，modern leg 假绿（fixture 退出码 1 但 referee 判绿）由 18 处降至 4 处，余 4 处为 `iss-*` 场景 fixture 主动拒绝错误 `iss` 的被测语义（referee 判绿）；`metadata-var1/2/3`、`authorization-server-migration` 四条随之转绿删除（referee stale 证据）
2. ~~`ExchangeCodeForToken` 忽略 auth method/secret~~（**已修**）：`ApplyClientAuthentication` 覆盖 client_credentials、授权码交换、refresh_token 三个令牌请求
3. ~~`Discover` 无 PRM 路径插入与 openid 回退~~（**已修**）：夹具侧补齐候选 URL 链（根路径优先 + openid 配置回退 + path-based 前置），provider 侧走 `OAuthMetadata::Discover`
4. 无传输层 DPoP proof：**发送侧已补**（`apply_per_request_headers` / `observe_response_challenge` 两个注入点 + `ApplyDPoPProofHeader` / `ObserveDPoPNonce`，夹具 `DriveDPoPScenario` 已接线真实授权流）；`dpop`/`dpop-nonce` 已转绿删除——WSL Linux + OpenSSL 3.5.5 实测 `sep-1932-client-*` 五项全 SUCCESS（0 FAILURE / 0 WARNING）；服务端校验链与开关已具备，见 [dpop](/concepts/dpop.md)；本地无 OpenSSL 构建的 modern leg 仍报 9 failed（`client-credentials-jwt`/`dpop`/`dpop-nonce`），属同一环境限制，不新增基线
5. 私钥 jwt（**已实现且已转绿**）：`client-credentials-jwt` 已转绿删除——WSL Linux + OpenSSL 实测 `client-credentials-jwt-verified` SUCCESS（35 checks / 0 FAILURE）。期间修正一处真实缺陷——`CreateClientAssertion` 的 `aud` 原取 metadata `token_endpoint`，而 RFC 7523 §3 第 3 条规定 `aud` 的确切字符串由 AS 与颁发方带外约定（token endpoint 仅为 MAY），官方 referee 以 issuer identifier 为约定值，已改为优先 `issuer`；`wif-jwt-bearer` 于 2026-10-29 实测 passed 后删除
6. StepUp 无 403 链路：`scope-step-up` 两 leg 均已转绿（modern 24 passed、legacy 20 passed），403 →提权链路可用

## 非标超集声明

自研 WebSocket client（[websocket](/transports/websocket.md)）为**非标超集定位**：RFC 6455 自研实现 + 客户端传输封装，spec 未要求 WebSocket 传输；保留为本 SDK 扩展能力，不计入 Tier1 对等口径，行为以本仓库源码为准。

## 遗留记账（未静默丢弃）

1. auth 基线现状（2026-10-29 复跑收口）：modern 0 条、legacy 仅留 `http-custom-headers`（本 leg 未运行，保留待复跑）；`client-credentials-basic`/`enterprise-managed-authorization`/`wif-jwt-bearer` 实测 passed 后已删除；两 leg 的 `did_not_complete` 假绿已清零（modern 4 处 `iss-*` 为主动拒绝的被测语义，referee 判绿）。client modern 450/9 的 9 项失败均为本地无 OpenSSL 的 DPoP/ES256 环境限制，不新增基线。完整逐条归因见 [baseline-2026-07-28.yaml](../../tests/conformance/baseline-2026-07-28.yaml) 与 [baseline.yaml](../../tests/conformance/baseline.yaml) 的头部注释
2. ~~`maxToolInputElements` 未实现~~（已实现：`ServerOptions::max_tool_input_elements` 门在 `tools/call` 入参侧计数，超限回 `is_error` 工具结果而非协议错误——`ToolInputElementCount(ctx.Params().arguments) > *options_.max_tool_input_elements`，[McpServer.cpp:1993-1998](../../src/server/McpServer.cpp)；`optional` 未设即不限，上限语义见 [experimental-flags](/concepts/experimental-flags.md) 与 `README.md` 「Default upper limits」条）
3. ~~`ttlMs`/`cacheScope` 客户端消费未验证~~（已实现且有用例：客户端 `McpClient::CacheIfHinted`（[McpClient.cpp:967-982](../../src/client/McpClient.cpp)）兼容两种线格式——2026 时代结果顶层 `ttlMs`/`cacheScope` 与 2025 时代嵌套 `cacheHint`，`ttlMs > 0` 才入缓存，`cacheScope` 仅 `"private"` 落 Private 分区、其余按 Public，结果类型另落 `ttl_ms`/`cache_scope` 字段；守护用例见 [response-cache](/concepts/response-cache.md) 与 `tests/unit/McpClientTests.cpp` 的分页/skills/TTL 到期用例）
4. ~~`enable_listen_stream` 默认 true~~（本分支 `1668e2e` 已改：字段为 `optional<bool>`，未设置时取 `!stateless`——stateful 默认开、stateless 默认关，显式置 `true` 仍可强制开，`ShouldListenStream` 判定见 [StreamableHttpClientTransport.cpp:148-149](../../src/http/StreamableHttpClientTransport.cpp)，`StatelessClientSkipsListenStreamByDefault` / `StatelessClientForcedListenStreamSendsGet` 两用例锁定；口径见 [streamable-http](/transports/streamable-http.md)）
5. `StepUpAuthorization` 空元数据解引用：无元数据路径存在空解引用风险，待加固；另无 403 链路（见上生产缺口记账）
6. ~~`GetAccessToken` 持锁同步刷新~~（**已修**）：网络 I/O 已移出 `refresh_mutex_`（`1668e2e` single-flight 合并）；同线程重入死锁亦已消除——新增 `refresh_owner_` 记录持有刷新标志的线程，`token_update_handler` 回调内同线程再调 `GetAccessToken` 时直接返回而不进条件变量等待（原会等自己永久阻塞）。守护用例 `TokenUpdateHandlerReentrantGetAccessTokenDoesNotDeadlock`（超时护栏）与 `ConcurrentWaiterSurvivesRefreshOwnerReentry`（跨线程等待不回归）见 [oauth](/concepts/oauth.md)
7. `MakeModernTaskEnv` 死通知已删、握手守护断言保留：2026 时代服务端丢弃未初始化会话的 `notifications/initialized`，该死通知已从 helper 删除（`tests/unit/McpServerTests.cpp`），现代任务用例改走 `MakeBareModernTaskEnv` 已验证的直连路径——任务门控请求逐请求 `_meta` 声明（`b7a613e` 先例）；helper 仍保留 C-T1 的握手守护断言（initialize 回 `MethodNotFound` 即判失败），引用该 helper 的 12 处用例形态不变
8. `unevaluatedProperties`/`unevaluatedItems`：按 spec §7.3 注解收集 opt-out 归入忽略注解类，无近似路径——`374a591` 删除 `JsonSchemaValidator.hpp` 内两处近似求值实现，改由三条忽略行为守护测试锁定（`tests/unit/McpTypesTests.cpp`）
9. ~~README 的 PKCE/DCR 表述待统一~~（本分支已解决：`README.md`/`README_zh.md` 统一为 `PKCE/CIMD（DCR 回退）`，`ad8075a` 前序 + 本分支 `README` 口径；`jwt-bearer` 不支持声明同步更新）
10. ~~`auth/scope-retry-limit` 在 legacy leg 挂 30s 超时~~（**已修**）：根因是 SSE 拆除路径——服务端不支持 `GET /mcp` 时返404，而 `ListenStateForStatusCode` 仅对 405 判 `Unsupported`，404 落 `Connecting` 触发 5 次退避重连，耗尽 referee 的 30s 预算（检查已跑完但进程退不出）。改判为 4xx 视为不支持监听流并静默放弃（对齐既有 405 语义，200 但 content-type 非 SSE 同样判 `Unsupported`；保留 408/429 与 2xx 非 200 为可重连）。属设计契约变更：既有断言 `ListenStateForStatusCode(404) == Connecting` 已同步翻转，并扩充 401/403/400/204/408/429 覆盖。修复后 legacy leg `scope-step-up` 20 passed 且进程正常退出
11. ~~`scope-retry-limit` fixture 永久挂起~~（**已修**）：现象是 referee modern leg 偶发卡 6 分钟需强杀。诊断路径：`/proc/<pid>/task/*/wchan` 显示 4 线程 futex + 1 线程 nanosleep（均为健康等待），主线程等 future；把 fixture stderr 重定向到按 PID 命名的文件绕过 referee socket 缓冲后，读到卡住进程真实日志——`CheckTimeouts` 持续运行但 `pending` 恒为 1 且**永不超时**，`OnResponse` 从未被调用。根因：`McpSessionHandler::max_total_timeout_` 默认 `0`（禁用，见 [McpSessionHandler.hpp:170](../../include/mcp/protocol/McpSessionHandler.hpp)），`absolute_deadlines_` 仅在非 0 时登记；而 per-request deadline 会被 progress 通知不断延长（`ResetTimeoutByProgressToken`，该行为由 `McpClientTest.TotalTimeoutDisabledAllowsProgressExtension` 锁定）。服务端只发 progress 不发最终响应时 → 永久挂起。修复：fixture 的 `ConnectBearerClient` 与 `ConnectDPoPClient` 显式设 `max_total_timeout = 10s`（不超过 referee 30s 预算）。修复后 referee 正常收尾 EXIT=0，Close 链完整执行

## 相关页面

- [/tests.md](/tests.md) — 用例数与 conformance 实测
- [/concepts/version-negotiation.md](/concepts/version-negotiation.md) — 协商双轨
- [/concepts/mrtr.md](/concepts/mrtr.md) — MRTR
- [/concepts/oauth.md](/concepts/oauth.md) — OAuth
- [/concepts/dpop.md](/concepts/dpop.md) — DPoP 验签链与逐请求 proof 缺口
- [/concepts/experimental-flags.md](/concepts/experimental-flags.md) — 实验开关
- [/transports/streamable-http.md](/transports/streamable-http.md) — 传输语义
- [/transports/websocket.md](/transports/websocket.md) — 非标超集
