---
type: Concept
title: Tier1 技术对标声明（2026-07-28）
description: 与 Tier1 五实现（TS v2.3.1 / Python v2.3.0 / C# v2.2.0 / Go v1.8.0 / Rust v3.5.1，spec 2026-07-28）的逐项技术对等状态；仅技术对标，无官方 Tier 申报意向；自研 WebSocket client 为非标超集。
tags: [tier1, parity, 2026-07-28, conformance, 对标]
timestamp: 2026-10-13T02:37:22+08:00
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

## conformance 四 leg（本分支口径）

- server legacy：67/67 绿
- server modern：194+1 经 `server-stateless` 基线绿，仅剩 2 SHOULD WARNING（`ServerSendsPromptsListChangedOnSubscription`、`ServerSendsToolsListChangedOnSubscription`，均为 SHOULD 级 listen 推送缺口）
- client legacy：11+43 经 auth 基线绿
- client modern：66+68 经 auth 基线绿（`http-custom-headers` 18/18 已删基线）
- 专项：`request_state` 5/5、`$ref` 1/1、`schema-preservation` 9/9

ctest 口径 848 用例（原 769），分布见 [/tests.md](/tests.md)。

## 非标超集声明

自研 WebSocket client（[websocket](/transports/websocket.md)）为**非标超集定位**：RFC 6455 自研实现 + 客户端传输封装，spec 未要求 WebSocket 传输；保留为本 SDK 扩展能力，不计入 Tier1 对等口径，行为以本仓库源码为准。

## 遗留记账（未静默丢弃）

1. auth fixture 场景仍走基线（单元已闭环）：client 双 leg 的 auth 条目（legacy 43、modern 68）继续保留，靠基线放行；生产 OAuth 表面单测已闭环，fixture 侧联调待后续
2. `maxToolInputElements` 未实现：工具输入元素数上限缺口，待排期
3. `ttlMs`/`cacheScope` 客户端消费未验证：服务端附带字段已具备，客户端消费路径无验证覆盖
4. `enable_listen_stream` 默认 true：stateless 客户端默认仍持开 GET 监听，须显式关；是否纳入单连接强制待裁决（见 [experimental-flags](/concepts/experimental-flags.md)）
5. `StepUpAuthorization` 空元数据解引用：无元数据路径存在空解引用风险，待加固
6. `GetAccessToken` 持锁同步刷新：刷新整段持 `refresh_mutex_`，同步阻塞语义待优化（见 [oauth](/concepts/oauth.md)）
7. `MakeModernTaskEnv` 失效 initialize 握手：现代任务测试 helper 的握手形态已失效，由 `b7a613e` 改逐请求声明绕行，helper 本体待修
8. `unevaluated` 求值近似：`unevaluatedProperties`/`unevaluatedItems` 为近似求值，非完整语义
9. README 的 PKCE/DCR 表述待统一：`README.md:96`（`OAuth (PKCE/DCR)`）与 `README.md:276`（`jwt-bearer … is not supported, public-client PKCE/DCR flow remains the supported path`）并存，面向 Tier1 全量鉴权表面后的公开表述待统一

## 相关页面

- [/tests.md](/tests.md) — 用例数与 conformance 实测
- [/concepts/version-negotiation.md](/concepts/version-negotiation.md) — 协商双轨
- [/concepts/mrtr.md](/concepts/mrtr.md) — MRTR
- [/concepts/oauth.md](/concepts/oauth.md) — OAuth
- [/concepts/experimental-flags.md](/concepts/experimental-flags.md) — 实验开关
- [/transports/streamable-http.md](/transports/streamable-http.md) — 传输语义
- [/transports/websocket.md](/transports/websocket.md) — 非标超集
