---
type: Concept
title: 实验开关
description: 服务端 10 开关与客户端 4 开关（默认全关）：tasks/skills 双端门控语义、tasks 隐式兼容过渡与 Warning、etag/渐进发现/文件输入/触发器/dpop 已实现默认关语义与构造期同步、interceptor 为无门控别名。
tags: [experimental, 实验开关, tasks, skills, 兼容]
timestamp: 2026-10-12T18:55:34+08:00
resource: include/mcp/server/ServerOptions.hpp
---

# 实验开关

`ad547f9` 起 Tier1 对齐引入的实验能力默认全部关闭，须显式开启。服务端 10 开关（[ServerOptions.hpp:80-89](../../include/mcp/server/ServerOptions.hpp)），客户端 4 开关（[ClientOptions.hpp:51-54](../../include/mcp/client/ClientOptions.hpp)），默认值均为 `false`。其中 tasks/skills 为最早接线的一批；etag/渐进发现/文件输入/触发器/dpop 五开关在后续批次全部接线（默认关实现），`enable_interceptor_alias` 为无门控的纯别名；仅 apps/server_card 两开关仍为保留未接线。

## 开关表

服务端：

| 开关 | 开：语义 | 关：语义 |
|------|----------|----------|
| `enable_tasks_extension` | 声明 tasks 扩展；`tasks/get\|update\|cancel` 与任务化 `tools/call` 生效 | 无 `task_store` 时对应方法 `MethodNotFound`（`tasks extension is disabled`）；仅设 `task_store` 时走隐式兼容（见下） |
| `enable_skills_extension` | 已注册技能且具 resources 能力时声明 skills 扩展；`skills/list\|get` 与 `directory/read` 生效 | 三方法一律 `MethodNotFound`（`skills extension is disabled`），即使已注册技能；无隐式路径 |
| `enable_apps_extension` | 保留：已声明、默认 `false`，暂无门控接线（apps 校验见 [Apps.hpp](../../include/mcp/ext/Apps.hpp)，独立于开关） | 同左 |
| `enable_server_card` | 保留：已声明、默认 `false`，暂无门控接线（校验见 [ServerCard.hpp](../../include/mcp/ext/ServerCard.hpp)，独立于开关） | 同左 |
| `enable_etag_cache` | 列表类结果序列化附内容哈希 `etag`（载荷超 `kEtagMaxBytes` 8MB 跳过不附；TTL 语义不动）；客户端 8 读取路径经 meta 扩展附 `ifNoneMatch` 条件发送，服务端回 `notModified` 时以存表回退 | 全局关闭：结果不附 `etag`，客户端存取表短路（`GetStoredEtag`/`StoreEtagBody` 直接返回），无条件发送 |
| `enable_progressive_discovery` | `tools/list` 支持 `query` 子串过滤 + `pageSize`（1–100）翻页，不再走缓存全量；`subscriptions/listen` 即刷并持开，`list_changed` 按订阅缓冲 | 原路：`tools/list` 走缓存全量返回；`listen` 与 `list_changed` 走原广播路径 |
| `enable_file_input` | `tools/call` 的 `arguments` 内 `file://`/`resource://` 引用对象按范围切片并内联小文件；`http(s)://` 只透传；越界与超限回错误结果 | 参数原样透传，不做任何引用解析 |
| `enable_triggers_events` | `RegisterTrigger` 注册表 + `EmitTriggerEvent` 按类型匹配，经订阅过滤投递 `notifications/triggers/event`，命中项的 webhook 失败只记 `Warning` 不抛 | `EmitTriggerEvent` 直接返回（事件静默丢弃）；注册/注销本身不限门 |
| `enable_dpop` | 服务端 `StreamableHttpServerTransport` 在 Bearer 链上加 DPoP 证明验签（`htm`/`htu` 比对，失败回 401 并带 nonce；`jti` 内存重放表 + 窗口淘汰；无 OpenSSL 时硬报错） | 原 Bearer 链，无证明校验 |
| `enable_interceptor_alias` | 别名即实现：`Interceptor` 为 `MessageFilter` 的别名，`AddInterceptor` 转发既有过滤器管线，执行顺序与管线一致，零行为变化；开关本身无门控，开/关无可观测差异 | 同左 |

客户端：

| 开关 | 开：语义 | 关：语义 |
|------|----------|----------|
| `enable_tasks_extension` | `extensions` 映射含任务键即声明；`UpdateTask`/`PollTaskToCompletion` 与任务句柄路径生效 | 上述调用抛 `MethodNotFound`（`tasks extension is disabled`），且声明判定恒为 false |
| `enable_skills_extension` | `ListSkills`/`GetSkill`/`ReadDirectory` 生效 | 三调用抛 `MethodNotFound`（`skills extension is disabled`） |
| `enable_apps_extension` | 保留：已声明、默认 `false`，暂无门控接线 | 同左 |
| `enable_progressive_discovery` | `SearchTools` 翻页请求附 `query`/`pageSize` | 翻页聚合仍可用，但不附 `query`/`pageSize`（服务端按缺省分页返回） |

门控位置：服务端任务守卫（[McpServer.cpp:1124、1168、1397](../../src/server/McpServer.cpp)）、能力推导（[McpServer.cpp:1565-1574](../../src/server/McpServer.cpp)）、任务化调用（[McpServer.cpp:1732-1733](../../src/server/McpServer.cpp)）、技能三方法（[McpServer.cpp:2288、2329、2372](../../src/server/McpServer.cpp)）；客户端声明判定（[McpClient.cpp:150](../../src/client/McpClient.cpp)）、技能三调用（[McpClient.cpp:1244、1270、1303](../../src/client/McpClient.cpp)）、任务两调用（[McpClient.cpp:1408、1439](../../src/client/McpClient.cpp)）。六开关接线位置：etag 构造期同步（[McpServer.cpp:505](../../src/server/McpServer.cpp)）+ 结果附哈希（[McpTypesResults.cpp:104-109](../../src/core/McpTypesResults.cpp)）+ 客户端 8 读取路径条件发送与回退（[McpClient.cpp:981-1451](../../src/client/McpClient.cpp)）；渐进发现服务端（[McpServer.cpp:1685](../../src/server/McpServer.cpp)）与客户端（[McpClient.cpp:1056](../../src/client/McpClient.cpp)）；文件输入（[McpServer.cpp:1819](../../src/server/McpServer.cpp)）；触发器发射门（[McpServer.cpp:827](../../src/server/McpServer.cpp)）；dpop 服务端校验（[StreamableHttpServerTransport.cpp:454](../../src/http/StreamableHttpServerTransport.cpp)）与客户端签发（[OAuthClientProvider.cpp:234、604、612](../../src/client/auth/OAuthClientProvider.cpp)）；拦截器别名（[MessageFilter.hpp:21、39](../../include/mcp/protocol/MessageFilter.hpp)）。

## 构造期同步

- `McpServer::Create` 按值拷贝 `ServerOptions`（[McpServer.cpp:499-505](../../src/server/McpServer.cpp)）：构造完成后调用方再改手中的 `options` 不生效；除 etag 外的开关均在请求处理路径实时读实例内 `options_` 拷贝（渐进 `1685`、触发器 `827`、文件输入 `1819` 行）。
- etag 例外：构造期经 `SetEtagCacheEnabled` 同步的是进程级全局开关（[McpTypesResults.cpp:50-56](../../src/core/McpTypesResults.cpp)），客户端 8 读取路径读的是同一全局（[McpClient.cpp:981](../../src/client/McpClient.cpp) 起）——任一 `McpServer` 以 `enable_etag_cache=true` 构造即打开全进程的 etag 读写，同进程的客户端同步生效；关态构造则关闭全进程。
- dpop 双端各自构造期取值：服务端由传输 `config.enable_dpop` 决定（[StreamableHttpServerTransport.cpp:454](../../src/http/StreamableHttpServerTransport.cpp)），客户端由 `OAuthClientProvider` 的 `options_.enable_dpop` 决定（[OAuthClientProvider.cpp:234](../../src/client/auth/OAuthClientProvider.cpp)），两端须各自显式开启。

## Tasks/Skills 兼容期（0.3.5 → 0.3.8，本次任务给定口径）

- Tasks：仅设 `task_store` 而未显式开 `enable_tasks_extension` 时仍隐式启用（[ServerOptions.hpp:91-93](../../include/mcp/server/ServerOptions.hpp) 的 `TasksImplicitlyEnabled()`），但每次经过能力推导与任务路径均记 `Warning` 日志（[McpServer.cpp:51](../../src/server/McpServer.cpp) 的 `kTasksImplicitWarning` 文本）。该辅助标记 `[[deprecated("set enable_tasks_extension=true explicitly")]]`，0.3.8 起计划移除隐式路径，届时只设 `task_store` 不再声明 tasks 扩展。注：0.3.8 为任务口径，代码内无版本标记；现版本为 0.3.5。
- Skills：`SkillsImplicitlyEnabled()`（[ServerOptions.hpp:94-96](../../include/mcp/server/ServerOptions.hpp)）同样标记 deprecated，但实现只返回开关本身、无隐式路径——技能自 `ad547f9` 起必须显式开 `enable_skills_extension=true`，否则能力不声明、三方法 `MethodNotFound`。

## 关/开两态语义

- 关：对应方法返回/抛出 `MethodNotFound`（服务端与客户端文案均为 `tasks extension is disabled` / `skills extension is disabled`），能力映射不声明对应扩展键。
- 上条仅 tasks/skills：etag/渐进/文件输入/触发器/dpop 关态不报 `MethodNotFound`，而是走原路（无 etag、全量列表、参数透传、事件丢弃、原 Bearer）；interceptor 为别名，开/关无差异。
- 开：恢复既有行为——前置条件满足即声明扩展键并放行方法：tasks 需设 `task_store`（服务端）/ 扩展映射含任务键（客户端），skills 需已注册技能且具 resources 能力（服务端）。tasks 与 skills 行为细节见 [/concepts/tasks-extension.md](/concepts/tasks-extension.md) 与 [/concepts/skills-extension.md](/concepts/skills-extension.md)。
- 技能守卫先于时代守卫：开关关闭时无论 modern/legacy 时代均报 `skills extension is disabled`，优先于时代拒绝。
