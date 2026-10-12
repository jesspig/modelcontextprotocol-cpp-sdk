# 更新摘要

- 2026-10-13 — gap-and-ci-green 分支 wiki 收尾（纯文档）：tier1-parity 页增 A-T2 真实流（client 夹具 +1143 行 OAuth 真实流，modern 删 21 / legacy 删 10 转绿）+ B2 证伪转 B3 + CI 三修复（DPoP 虚构 API、inline 链接、解析器显式栈）+ 生产缺口记账 6 项，tests.md 口径 848/3606（C-T1 握手断言 +1）+ 四 leg 新数（server legacy 67/67、server modern 194+1、client legacy 179+4、client modern 383 + 保留 10 条）+ 基线现状，详见 [/changelog/2026-10-13-log.md](/changelog/2026-10-13-log.md)

- 2026-10-13 — T11 文档收尾与遗留记账（纯文档）：新建 Tier1 技术对标声明页 [/concepts/tier1-parity.md](/concepts/tier1-parity.md)（TS v2.3.1/Python v2.3.0/C# v2.2.0/Go v1.8.0/Rust v3.5.1，仅技术对标、无官方 Tier 申报意向；WebSocket 非标超集；9 项遗留记账），tests.md 口径 848/3605（分布已回填） + 四 leg（server legacy 67/67、server modern 194+1、client legacy 11+43、client modern 66+68）+ 专项（request_state 5/5、`$ref` 1/1、schema 9/9），`http-custom-headers` 18/18 已删基线，详见 [/changelog/2026-10-13-log.md](/changelog/2026-10-13-log.md)

- 2026-10-13 — T9 单连接语义与可观测锁定：生产零改动（stateless 单连接三元组 + 写完即关 + DELETE 405 + span 全路径配对均已具备），测试 +5（streamable-http +3 默认/405/独立连接、span-hook +2 成功与拒绝 End 回填 trace，待主代理实测），span-hooks 页增 OTel 差异节，详见 [/changelog/2026-10-13-log.md](/changelog/2026-10-13-log.md)

- 2026-10-13 — T8 Apps 扩展与 Server Card 接线 + tasks 隐式移除：生产 5 文件（ext 双头增扩展标识常量、ServerOptions 增 `server_card`、McpServer 增 `GetServerCard`、McpServer.cpp 能力推导/apps 资源读门/任务四处去隐式），测试 6 处显式化 + 新增矩阵与显式 tasks 两用例（待主代理实测），详见 [/changelog/2026-10-13-log.md](/changelog/2026-10-13-log.md)

- 2026-10-12 — 前瞻六开关文档同步（ad547f9 后续批次）：`experimental-flags.md` 六开关去“保留未接线”标注（etag/渐进/文件/触发器/dpop 已实现默认关语义 + 构造期同步节，interceptor 为无门控别名；客户端增至 4 开关），docs 中英 extensions.md 同表镜像，tests.md 仅留用例数待回填占位；只改 6 个 md，详见 [/changelog/2026-10-12-log.md](/changelog/2026-10-12-log.md)

- 2026-10-12 — 实验开关文档补齐（ad547f9/cada606 遗留）：新建 [/concepts/experimental-flags.md](/concepts/experimental-flags.md)（服务端 10 + 客户端 3 开关表、tasks 隐式兼容 + Warning + deprecated、关/开两态语义），tests.md 增开关矩阵、docs 中英 extensions.md 镜像开关表；只改 6 个 md，不碰代码与测试基线，详见 [/changelog/2026-10-12-log.md](/changelog/2026-10-12-log.md)

- 2026-10-12 — spec-20260728-tier1-parity 一次性对齐：T2 request_state/T3 schema/T5 错误面/T6 上限重定向/T7 头空载荷/T8 协商可观测/T9 PRM-first/T10 任务推送/T11 技能验签/T12 文档，748/748，详见 [/changelog/2026-10-12-log.md](/changelog/2026-10-12-log.md)
