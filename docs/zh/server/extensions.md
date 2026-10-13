# 扩展

协议扩展通过 `ClientCapabilities` 和 `ServerCapabilities` 上的 `extensions` 映射进行协商。每个条目将扩展标识符映射到其配置值。

## 服务端自动推导

在 `ServerOptions` 上设置 `task_store` 会自动声明任务扩展：

```cpp
auto transport = std::make_shared<StdioServerTransport>();

ServerOptions opts;
opts.task_store = std::make_shared<FileTaskStore>("tasks.json");

auto server = McpServer::Create(transport, opts);
```

`ServerOptions` 没有 `capabilities` 字段。能力始终自动推导：

| 条件 | 声明的扩展键 | 值 |
|------|-------------|-----|
| 设置了 `task_store` | `"io.modelcontextprotocol/tasks"` | 空对象 `{}` |
| 注册了至少一个技能（`RegisterSkill`）**且**具备 resources 能力 | `"io.modelcontextprotocol/skills"` | 空对象；`SetSkillsDirectoryRead(true)` 后含 `"directoryRead": true` |

客户端侧声明见 [客户端概述](/zh/client/overview)（`ClientOptions::capabilities.extensions` 或 `ClientOptions::extensions` 含任务扩展键）；2026 时代下未声明任务扩展却收到任务结果时客户端抛 `HandlerError`。

## 任务通知路由

2026 时代下任务状态通知（任务化 `tools/call` 的初始 Working 与终态、`tasks/update` 的 `tasks/completed`/`tasks/working`、`tasks/cancel` 的 `tasks/cancelled`）不再抑制，改经服务端订阅表按订阅路由：投递给全部现存订阅者（任务类通知不过滤器），每帧 `_meta` 携带 `io.modelcontextprotocol/subscriptionId`（`subscriptions/listen` 传入 ID 则回显，否则用服务端自增 ID）；尚无订阅者时直接丢弃，客户端靠 `PollTaskToCompletion` 轮询取终态（任务 `poll_interval_ms > 0` 时采用服务端指示间隔，上限 300s）。2025 时代保持广播（`notifications/tasks/*`）。

## 技能 URI 形态与验签助手

技能条目 `uri` 须为 `<scheme>://<名>/SKILL.md` 形态（scheme 无关：只要求非空 `://`、名段非空、`/SKILL.md` 结尾；`skill://` 仍为约定 scheme）。服务端只做形态放行，是否取用他源技能由客户端经 `mcp/client/SkillVerify.hpp` 五助手判定：`CheckSkillLimits`（manifest ≤ 512 文件、总量 ≤ 16 MB）、`VerifyFileBytes`（`size` 等长 + `sha256:` 摘要实算比对）、`VerifyFrontmatter`（两份一致且 `name` 等于 URI 名段）、`IsApprovalStillValid`（授权快照复核）、`ApprovalKey`（`"<host>\x1f<skill-uri>"` 授权键）。

## 扩展约定

| 字段 | 说明 |
|-------|-------------|
| 键 | 反向 DNS 标识符（例如 `"io.modelcontextprotocol/tasks"`） |
| 值 | 包含扩展配置的自由格式 JSON 对象 |

## 实验开关

`ServerOptions` 10 开关与 `ClientOptions` 4 开关默认全为 `false`，须显式置 `true`（开关构造期同步：`McpServer::Create` 按值拷贝，事后改调用方 `options` 不生效；etag 为进程级全局，任一服务端开态构造即打开全进程读写）：

| 端 | 开关 | 开 | 关 |
|----|------|----|----|
| 服务端 | `enable_tasks_extension` | 声明 tasks 扩展；`tasks/get\|update\|cancel` 与任务化 `tools/call` 生效 | 无 `task_store` 时对应方法 `MethodNotFound`（`tasks extension is disabled`）；仅设 `task_store` 时隐式启用并记 `Warning`（0.3.5→0.3.8 过渡，见下） |
| 服务端 | `enable_skills_extension` | 已注册技能且具 resources 能力时声明 skills 扩展；`skills/list\|get` 与 `directory/read` 生效 | 三方法一律 `MethodNotFound`（`skills extension is disabled`），无隐式路径 |
| 服务端 | `enable_etag_cache` | 列表类结果附内容哈希 `etag`（超 8MB 跳过）；命中 `notModified` 时客户端以存表回退，请求经 meta 扩展附 `ifNoneMatch` | 结果不附 `etag`，客户端存取短路，无条件发送 |
| 服务端 | `enable_progressive_discovery` | `tools/list` 支持 `query` 过滤 + `pageSize` 翻页；`listen` 即刷持开，`list_changed` 按订阅缓冲 | `tools/list` 走缓存全量；`listen` 与 `list_changed` 走原路径 |
| 服务端 | `enable_file_input` | `tools/call` 参数内 `file://`/`resource://` 引用按范围切片并内联小文件（`http(s)` 透传，越界超限回错误） | 参数原样透传 |
| 服务端 | `enable_triggers_events` | 注册表 + 按类型匹配，经订阅投递触发器事件通知并打 webhook（失败只记 `Warning`） | 发射直接返回，事件静默丢弃 |
| 服务端 | `enable_dpop`（`ServerOptions::enable_dpop` **无读取点，不生效**；实际生效门是传输侧 `bearer_auth->enable_dpop`） | 由传输侧 `StreamableHttpServerTransport` 的 Bearer 鉴权配置加 DPoP 证明验签（失败 401 带 nonce，`jti` 防重放；无 OpenSSL 硬报错）；该字段仅在 `ServerOptions` 声明，全仓无读取点 | 原 Bearer 链 |
| 服务端 | `enable_interceptor_alias` | 别名即实现：`Interceptor` 为 `MessageFilter` 别名，注册转发既有管线，零行为变化；开关无门控 | 同左 |
| 服务端 | `enable_apps_extension` / `enable_server_card` | 保留：已声明、默认 `false`，暂无门控接线 | 同左 |
| 客户端 | `enable_tasks_extension` | `extensions` 映射含任务键即声明；`UpdateTask`/`PollTaskToCompletion` 与任务句柄路径生效 | 上述调用抛 `MethodNotFound`（`tasks extension is disabled`） |
| 客户端 | `enable_skills_extension` | `ListSkills`/`GetSkill`/`ReadDirectory` 生效 | 三调用抛 `MethodNotFound`（`skills extension is disabled`） |
| 客户端 | `enable_progressive_discovery` | `SearchTools` 翻页请求附 `query`/`pageSize` | 翻页聚合仍可用，但不附 `query`/`pageSize` |
| 客户端 | `enable_apps_extension` | 保留：已声明、默认 `false`，暂无门控接线 | 同左 |

兼容过渡（0.3.5→0.3.8，本次任务给定口径）：`TasksImplicitlyEnabled()` / `SkillsImplicitlyEnabled()` 均标 `[[deprecated]]`，提示显式置开关；其中仅 tasks 有真实隐式路径（只设 `task_store` 即启用，伴 `Warning` 日志），skills 无隐式路径、必须显式开启。
