---
type: Concept
title: Tier1 技术对标声明（2026-07-28）
description: 与 Tier1 五实现（TS v2.3.1 / Python v2.3.0 / C# v2.2.0 / Go v1.8.0 / Rust v3.5.1，spec 2026-07-28）的逐项技术对等状态；仅技术对标，无官方 Tier 申报意向；自研 WebSocket client 为非标超集。
tags: [tier1, parity, 2026-07-28, conformance, 对标]
timestamp: 2026-10-13T11:18:04+08:00
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
| Apps 扩展与 Server Card | 对等 | `2f2fc8d`；开关见 [experimental-flags](/concepts/experimental-flags.md) |
| 单连接语义与 trace 配对可观测 | 对等 | `a581292`；[span-hooks](/concepts/span-hooks.md) |
| 服务端现代任务用例形态 | 对等 | `b7a613e`（逐请求声明） |
| `http-custom-headers` 18/18 | 对等（基线已删） | `baseline-2026-07-28.yaml` 删除 10 行注释 + 条目；CI Linux 报 stale 即为变绿证据 |

## conformance 四 leg（本分支 `feature/gap-and-ci-green` 口径，主代理实测）

- server legacy：67/67 绿
- server modern：194 passed + `server-stateless` 基线 1 条绿，仅剩 2 SHOULD WARNING（`ServerSendsPromptsListChangedOnSubscription`、`ServerSendsToolsListChangedOnSubscription`，均为 SHOULD 级 listen 推送缺口）
- client legacy：179 passed + 4 条 auth 基线（删 10 条目转绿：`metadata-default`、`basic-cimd`、`scope-from-www-authenticate`、`scope-from-scopes-supported`、`scope-omitted-when-undefined`、`scope-retry-limit`、`token-endpoint-auth-basic/post/none`、`pre-registration`；保留 `metadata-var1/var2/var3` + `scope-step-up`；`http-custom-headers` 另留 1 条）
- client modern：383 passed + 保留 10 条目（删 21 条目转绿：`metadata-default`、`basic-cimd`、`scope`×5、`token-endpoint`×3、`pre-registration`、`resource-mismatch`、`offline-access-scope`、`offline-access-not-supported`、`iss`×6 含 normalized、`metadata-issuer-mismatch`；保留 `metadata-var1/var2/var3`、`authorization-server-migration`（部分失败）、`client-credentials-jwt/basic`、`enterprise-managed-authorization`、`dpop`、`dpop-nonce`、`wif-jwt-bearer` 共 10 条）
- 专项：`request_state` 5/5、`$ref` 1/1、`schema-preservation` 9/9

ctest 口径 848 用例全绿（`51f35d0` 主代理实测），其后两次提交新增用例尚未复跑：静态 `TEST/TEST_F/TEST_P` 口径现为 **854**（848 + `1668e2e` +3：`OAuthTests` +1、`StreamableHttpTransportTests` +2 + `374a591` +3：`McpTypesTests`），断言 token **3631**（3606 + `1668e2e` +16 + `374a591` +9）；ctest 注册口径待主代理 `ctest -N` 复核，逐目标分布见 [/tests.md](/tests.md)。

T11 口径留档：server legacy 67/67、server modern 194+1、client legacy 11+43、client modern 66+68（auth 全走基线时代）。

## A-T2 auth 进展（fixture 真实流联调）

`examples/conformance/client/main.cpp`（+1143 行，分支 `feature/gap-and-ci-green` 的 `63c6c15/333130c`）为 client 夹具接上真实 OAuth 流：PRM-first 发现（`/.well-known/oauth-protected-resource` → `authorization_servers[0]` → RFC 8414 元数据）、CIMD 客户端标识（`http(s)://` `client_id` 拉元数据文档）与 DCR 回退（仅未配置 `client_id` 时注册）、授权码交换（`AuthCodeTokenFields` + `ManualToken`）、授权响应 `iss` 校验（缺 `iss`/错 issuer 拒绝）、`token-exchange`/`id-jag` 变体路径。`dpop`/`dpop-nonce` 场景明确标 untestable（逐请求 DPoP proof 需传输层支持，见下生产缺口记账）。

证据为 referee `stale`：转绿条目从两 per-leg 基线删除后复跑不再报缺条目（`baseline-2026-07-28.yaml` 删 21、`baseline-2025-11-25.yaml` 删 10；`84f931f/333130c`）。

## B2 证伪（fixture 侧分流不可行，转 B3）

B2（fixture 侧按 listen 拓扑分流以消 `server-stateless`/`server-sse-multiple-streams` 基线）已证伪：listen 拓扑由 referee 控制，fixture 侧无法决定分流。转 B3：基线保留 + 本结论记账，零代码改动。

## CI 红根因与修复（三项）

1. DPoP 虚构 API（一行，已修，ubuntu proof 待 CI）：`src/ext/DPoP.cpp` 的 `ECDSA_SIG_get0_sig` 为虚构 API，改为 `ECDSA_SIG_get0`（`48134d8`）
2. inline 链接（已修，release + WERROR 本地证毕）：`src/http/StreamableHttpClientTransport.cpp` 的 `ReconnectDelayForAttempt` 去 `inline`（`48134d8`）
3. 深嵌套栈溢出（解析器改显式栈，已修）：`src/core/JsonParser.cpp` 递归下降（`ParseObject`/`ParseArray` 经 `depth_` 计数）改显式栈迭代（`ParseContainer` + `Frame` 栈，`kMaxDepth` 改按栈深判定）（`ac1d4a3`）；macOS Bus error 为我方 T6/T7 引入、现已修，windows SEGFAULT 早有、本次一并修好

CI 另改 per-leg 基线映射：`.github/workflows/conformance.yml` 的 server/client 双 job 按 `CONFORMANCE_SPEC_VERSION` 选择基线（2026-07-28 → `baseline-2026-07-28.yaml`、2025-11-25 → `baseline-2025-11-25.yaml`，未知回退 `baseline.yaml`）（`b1d529f`）；fallback 对齐同提交。

## 生产缺口记账（A-T2 上报，未静默丢弃）

1. token 响应无 `iss` 致 provider 流拿不到可用 token
2. `ExchangeCodeForToken` 忽略 auth method/secret
3. `Discover` 无 PRM 路径插入与 openid 回退
4. 无传输层 DPoP proof：`StreamableHttpClientTransport` 只发 `Authorization`、不逐请求附 `DPoP` 证明，且 `SetDPoPNonce` 无生产调用点（nonce 无回路），故 `dpop`/`dpop-nonce` fixture 侧只能标 untestable；服务端校验链与开关已具备，见 [dpop](/concepts/dpop.md)
5. 私钥 jwt 缺失（`client-credentials-jwt`/`wif-jwt-bearer` 仍走基线）
6. StepUp 无 403 链路

## 非标超集声明

自研 WebSocket client（[websocket](/transports/websocket.md)）为**非标超集定位**：RFC 6455 自研实现 + 客户端传输封装，spec 未要求 WebSocket 传输；保留为本 SDK 扩展能力，不计入 Tier1 对等口径，行为以本仓库源码为准。

## 遗留记账（未静默丢弃）

1. auth 基线现状（fixture 真实流后仅剩失败条目）：modern 保留 10 条（见上四 leg）、legacy 保留 4 条 + `http-custom-headers`；生产 OAuth 表面单测已闭环，未转绿条目归因见上生产缺口记账
2. ~~`maxToolInputElements` 未实现~~（已实现：`ServerOptions::max_tool_input_elements` 门在 `tools/call` 入参侧计数，超限回 `is_error` 工具结果而非协议错误——`ToolInputElementCount(ctx.Params().arguments) > *options_.max_tool_input_elements`，[McpServer.cpp:1993-1998](../../src/server/McpServer.cpp)；`optional` 未设即不限，上限语义见 [experimental-flags](/concepts/experimental-flags.md) 与 `README.md` 「Default upper limits」条）
3. ~~`ttlMs`/`cacheScope` 客户端消费未验证~~（已实现且有用例：客户端 `McpClient::CacheIfHinted`（[McpClient.cpp:967-982](../../src/client/McpClient.cpp)）兼容两种线格式——2026 时代结果顶层 `ttlMs`/`cacheScope` 与 2025 时代嵌套 `cacheHint`，`ttlMs > 0` 才入缓存，`cacheScope` 仅 `"private"` 落 Private 分区、其余按 Public，结果类型另落 `ttl_ms`/`cache_scope` 字段；守护用例见 [response-cache](/concepts/response-cache.md) 与 `tests/unit/McpClientTests.cpp` 的分页/skills/TTL 到期用例）
4. ~~`enable_listen_stream` 默认 true~~（本分支 `1668e2e` 已改：字段为 `optional<bool>`，未设置时取 `!stateless`——stateful 默认开、stateless 默认关，显式置 `true` 仍可强制开，`ShouldListenStream` 判定见 [StreamableHttpClientTransport.cpp:148-149](../../src/http/StreamableHttpClientTransport.cpp)，`StatelessClientSkipsListenStreamByDefault` / `StatelessClientForcedListenStreamSendsGet` 两用例锁定；口径见 [streamable-http](/transports/streamable-http.md)）
5. `StepUpAuthorization` 空元数据解引用：无元数据路径存在空解引用风险，待加固；另无 403 链路（见上生产缺口记账）
6. `GetAccessToken` 持锁同步刷新：刷新整段持 `refresh_mutex_`，同步阻塞语义待优化（见 [oauth](/concepts/oauth.md)）
7. `MakeModernTaskEnv` 死通知已删、握手守护断言保留：2026 时代服务端丢弃未初始化会话的 `notifications/initialized`，该死通知已从 helper 删除（`tests/unit/McpServerTests.cpp`），现代任务用例改走 `MakeBareModernTaskEnv` 已验证的直连路径——任务门控请求逐请求 `_meta` 声明（`b7a613e` 先例）；helper 仍保留 C-T1 的握手守护断言（initialize 回 `MethodNotFound` 即判失败），引用该 helper 的 12 处用例形态不变
8. `unevaluatedProperties`/`unevaluatedItems`：按 spec §7.3 注解收集 opt-out 归入忽略注解类，无近似路径——`374a591` 删除 `JsonSchemaValidator.hpp` 内两处近似求值实现，改由三条忽略行为守护测试锁定（`tests/unit/McpTypesTests.cpp`）
9. ~~README 的 PKCE/DCR 表述待统一~~（本分支已解决：`README.md`/`README_zh.md` 统一为 `PKCE/CIMD（DCR 回退）`，`ad8075a` 前序 + 本分支 `README` 口径；`jwt-bearer` 不支持声明同步更新）

## 相关页面

- [/tests.md](/tests.md) — 用例数与 conformance 实测
- [/concepts/version-negotiation.md](/concepts/version-negotiation.md) — 协商双轨
- [/concepts/mrtr.md](/concepts/mrtr.md) — MRTR
- [/concepts/oauth.md](/concepts/oauth.md) — OAuth
- [/concepts/dpop.md](/concepts/dpop.md) — DPoP 验签链与逐请求 proof 缺口
- [/concepts/experimental-flags.md](/concepts/experimental-flags.md) — 实验开关
- [/transports/streamable-http.md](/transports/streamable-http.md) — 传输语义
- [/transports/websocket.md](/transports/websocket.md) — 非标超集
