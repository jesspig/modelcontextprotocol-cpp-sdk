# 更新摘要

- 2026-10-13 — T9 单连接语义与可观测锁定：生产零改动（stateless 单连接三元组 + 写完即关 + DELETE 405 + span 全路径配对均已具备），测试 +5（streamable-http +3 默认/405/独立连接、span-hook +2 成功与拒绝 End 回填 trace，待主代理实测），span-hooks 页增 OTel 差异节，详见 [/changelog/2026-10-13-log.md](/changelog/2026-10-13-log.md)

- 2026-10-13 — T8 Apps 扩展与 Server Card 接线 + tasks 隐式移除：生产 5 文件（ext 双头增扩展标识常量、ServerOptions 增 `server_card`、McpServer 增 `GetServerCard`、McpServer.cpp 能力推导/apps 资源读门/任务四处去隐式），测试 6 处显式化 + 新增矩阵与显式 tasks 两用例（待主代理实测），详见 [/changelog/2026-10-13-log.md](/changelog/2026-10-13-log.md)

- 2026-10-12 — 前瞻六开关文档同步（ad547f9 后续批次）：`experimental-flags.md` 六开关去“保留未接线”标注（etag/渐进/文件/触发器/dpop 已实现默认关语义 + 构造期同步节，interceptor 为无门控别名；客户端增至 4 开关），docs 中英 extensions.md 同表镜像，tests.md 仅留用例数待回填占位；只改 6 个 md，详见 [/changelog/2026-10-12-log.md](/changelog/2026-10-12-log.md)

- 2026-10-12 — 实验开关文档补齐（ad547f9/cada606 遗留）：新建 [/concepts/experimental-flags.md](/concepts/experimental-flags.md)（服务端 10 + 客户端 3 开关表、tasks 隐式兼容 + Warning + deprecated、关/开两态语义），tests.md 增开关矩阵、docs 中英 extensions.md 镜像开关表；只改 6 个 md，不碰代码与测试基线，详见 [/changelog/2026-10-12-log.md](/changelog/2026-10-12-log.md)

- 2026-10-12 — spec-20260728-tier1-parity 一次性对齐：T2 request_state/T3 schema/T5 错误面/T6 上限重定向/T7 头空载荷/T8 协商可观测/T9 PRM-first/T10 任务推送/T11 技能验签/T12 文档，748/748，详见 [/changelog/2026-10-12-log.md](/changelog/2026-10-12-log.md)

- 2026-10-12 — G 系列冻结批知识库收敛 + GATE1 文档收尾（GATE1）：生产只读（失败语义 failed+内联 error、2026 扁平/2025 嵌套、`_meta` 合并、非工具 input_required、请求流缓冲、applyDefaults），修正 mcp-server/tasks-extension/modules-server 三处 G4 过时描述；tests.md 重列 18 目标 **715 用例 / 2857 断言**（无新目标；全量 715/715 通过）；基线仅刷头部注释（33 条不动，input-required/sep-2164/progress-modern 已绿）；脚本修 client leg 相对路径 spawn（转绝对路径）；Windows 复跑 client 双 leg 均为红（2025 unexpected elicitation 0/5、2026 unexpected sep-2322 0/5 + json-schema 0/1，三项待主会话裁决），与前提不符以实测为准，详见 [/changelog/2026-10-12-log.md](/changelog/2026-10-12-log.md)

- 2026-10-11 — 任务/技能双扩展与传输加固知识库收敛（T11b）：分支 12 提交冻结后一次收敛——wire-codec（26 方法并集/tasks 双时代交集/`ValidateTaskHeaders`）、mcp-server（任务分流/skills 三方法/目录视图/扩展声明）、mcp-client（扩展守卫/`UpdateTask` inputResponses/技能缓存/audience 透传）、file-task-store（6 新字段/`AdvanceTaskWithInput`）、oauth（audience/issuer 绑定/challenge 加固/403 分流）、streamable-http（版本头 400/taskId 头/无状态选项/503 满存与闲置过期/legacy SSE 默认关/重试钳制）、sse（T9b reconciliate）、version-negotiation（skills/tasks 门控表）；新增 tasks/skills 两概念页；tests.md 重列 18 目标 **691 用例 / 2638 断言**（无新目标，conformance 双 leg：2025 双绿/2026 双红，删 4 条保留 sse-retry/server-sse-multiple-streams/http-custom-headers）；docs 中英 6 页镜像同步，详见 [/changelog/2026-10-11-log.md](/changelog/2026-10-11-log.md)
