# 更新摘要

- 2026-10-12 — 前瞻六开关文档同步（ad547f9 后续批次）：`experimental-flags.md` 六开关去“保留未接线”标注（etag/渐进/文件/触发器/dpop 已实现默认关语义 + 构造期同步节，interceptor 为无门控别名；客户端增至 4 开关），docs 中英 extensions.md 同表镜像，tests.md 仅留用例数待回填占位；只改 6 个 md，详见 [/changelog/2026-10-12-log.md](/changelog/2026-10-12-log.md)

- 2026-10-12 — 实验开关文档补齐（ad547f9/cada606 遗留）：新建 [/concepts/experimental-flags.md](/concepts/experimental-flags.md)（服务端 10 + 客户端 3 开关表、tasks 隐式兼容 + Warning + deprecated、关/开两态语义），tests.md 增开关矩阵、docs 中英 extensions.md 镜像开关表；只改 6 个 md，不碰代码与测试基线，详见 [/changelog/2026-10-12-log.md](/changelog/2026-10-12-log.md)

- 2026-10-12 — spec-20260728-tier1-parity 一次性对齐：T2 request_state/T3 schema/T5 错误面/T6 上限重定向/T7 头空载荷/T8 协商可观测/T9 PRM-first/T10 任务推送/T11 技能验签/T12 文档，748/748，详见 [/changelog/2026-10-12-log.md](/changelog/2026-10-12-log.md)

- 2026-10-12 — G 系列冻结批知识库收敛 + GATE1 文档收尾（GATE1）：生产只读（失败语义 failed+内联 error、2026 扁平/2025 嵌套、`_meta` 合并、非工具 input_required、请求流缓冲、applyDefaults），修正 mcp-server/tasks-extension/modules-server 三处 G4 过时描述；tests.md 重列 18 目标 **715 用例 / 2857 断言**（无新目标；全量 715/715 通过）；基线仅刷头部注释（33 条不动，input-required/sep-2164/progress-modern 已绿）；脚本修 client leg 相对路径 spawn（转绝对路径）；Windows 复跑 client 双 leg 均为红（2025 unexpected elicitation 0/5、2026 unexpected sep-2322 0/5 + json-schema 0/1，三项待主会话裁决），与前提不符以实测为准，详见 [/changelog/2026-10-12-log.md](/changelog/2026-10-12-log.md)

- 2026-10-11 — 任务/技能双扩展与传输加固知识库收敛（T11b）：分支 12 提交冻结后一次收敛——wire-codec（26 方法并集/tasks 双时代交集/`ValidateTaskHeaders`）、mcp-server（任务分流/skills 三方法/目录视图/扩展声明）、mcp-client（扩展守卫/`UpdateTask` inputResponses/技能缓存/audience 透传）、file-task-store（6 新字段/`AdvanceTaskWithInput`）、oauth（audience/issuer 绑定/challenge 加固/403 分流）、streamable-http（版本头 400/taskId 头/无状态选项/503 满存与闲置过期/legacy SSE 默认关/重试钳制）、sse（T9b reconciliate）、version-negotiation（skills/tasks 门控表）；新增 tasks/skills 两概念页；tests.md 重列 18 目标 **691 用例 / 2638 断言**（无新目标，conformance 双 leg：2025 双绿/2026 双红，删 4 条保留 sse-retry/server-sse-multiple-streams/http-custom-headers）；docs 中英 6 页镜像同步，详见 [/changelog/2026-10-11-log.md](/changelog/2026-10-11-log.md)

- 2026-09-20 — 知识库一致性审计：三路只读审计确认基线三数 18/632/2154 零差值、语义层全一致；修正 15 处行号漂移与链接写法（http/http-server/streamable-http 6 处行号，concurrency/version-negotiation/meta-and-filters/storage 9 处 `:行号` 移出 URL，其中 6 处行号亦漂移一并修正，均以源码实测为准）；AGENTS.md 链接规则澄清为根链接或相对链接均可；7 页 timestamp 同步，详见 [/changelog/2026-09-20-log.md](/changelog/2026-09-20-log.md)

- 2026-09-20 — 版本号更新至 0.3.5：`include/mcp/McpVersion.hpp` 的 `kSdkVersion` `0.3.4` → `0.3.5`（唯一来源，`CMakeLists.txt` 自动提取，无需改 CMake；`client_info`/`server_version`/`server_info` 默认值随之更新）；在线文档中英 `client/overview.md` 与知识库 `classes/mcp-client.md` 默认客户端标识同步，详见 [/changelog/2026-09-20-log.md](/changelog/2026-09-20-log.md)
