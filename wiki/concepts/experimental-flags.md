---
type: Concept
title: 实验开关
description: 服务端 10 开关与客户端 4 开关（默认全关）：tasks/skills/apps/server-card 门控语义、tasks 隐式兼容已移除（只设 task_store 不再声明，需显式开）、etag/渐进发现/文件输入/触发器/dpop 已实现默认关语义与构造期同步、interceptor 为无门控别名。
tags: [experimental, 实验开关, tasks, skills, apps, server-card, 兼容]
timestamp: 2026-10-13T11:10:48+08:00
resource: include/mcp/server/ServerOptions.hpp
---

# 实验开关

`ad547f9` 起 Tier1 对齐引入的实验能力默认全部关闭，须显式开启。服务端 10 开关（[ServerOptions.hpp:81-91](../../include/mcp/server/ServerOptions.hpp)），客户端 4 开关（[ClientOptions.hpp:51-54](../../include/mcp/client/ClientOptions.hpp)），默认值均为 `false`。其中 tasks/skills 为最早接线的一批；etag/渐进发现/文件输入/触发器/dpop 五开关在后续批次全部接线（默认关实现），`enable_interceptor_alias` 为无门控的纯别名；apps/server_card 两开关在本轮接线（能力声明 + 请求门控，见下）。

## 开关表

服务端：

| 开关 | 开：语义 | 关：语义 |
|------|----------|----------|
| `enable_tasks_extension` | 声明 tasks 扩展；`tasks/get\|update\|cancel` 与任务化 `tools/call` 生效 | 无 `task_store` 时对应方法 `MethodNotFound`（`tasks extension is disabled`）；仅设 `task_store` 时走隐式兼容（见下） |
| `enable_skills_extension` | 已注册技能且具 resources 能力时声明 skills 扩展；`skills/list\|get` 与 `directory/read` 生效 | 三方法一律 `MethodNotFound`（`skills extension is disabled`），即使已注册技能；无隐式路径 |
| `enable_apps_extension` | 声明 `io.modelcontextprotocol/ui` 扩展（[Apps.hpp:11](../../include/mcp/ext/Apps.hpp) 的 `kAppsExtensionId`，对标 C# `McpApps.ExtensionId`）；开时 `ui://` 资源读走正常路径并经 `ValidateUiMeta` 校验 | 关时 `ui://` 的 `resources/read` 一律 `HandlerError`（`apps extension is disabled`），非 `ui://` 资源不受影响 |
| `enable_server_card` | 声明 `io.modelcontextprotocol/server-card` 扩展（[ServerCard.hpp:14](../../include/mcp/ext/ServerCard.hpp) 的 `kServerCardExtensionId`，SEP-2127）；开时 `GetServerCard()` 返回显式卡（`server_card`，[ServerOptions.hpp:85](../../include/mcp/server/ServerOptions.hpp)）或由 `server_info` + 支持版本构建的默认卡，均经 `ValidateCard` 校验 | 关时 `GetServerCard()` 抛 `HandlerError`（`server card is disabled`）；HTTP `GET /server-card` 仍由传输侧同名开关独立门控 |
| `enable_etag_cache` | 列表类结果序列化附内容哈希 `etag`（载荷超 `kEtagMaxBytes` 8MB 跳过不附；TTL 语义不动）；客户端 8 读取路径经 meta 扩展附 `ifNoneMatch` 条件发送，服务端回 `notModified` 时以存表回退 | 全局关闭：结果不附 `etag`，客户端存取表短路（`GetStoredEtag`/`StoreEtagBody` 直接返回），无条件发送 |
| `enable_progressive_discovery` | `tools/list` 支持 `query` 子串过滤 + `pageSize`（1–100）翻页，不再走缓存全量；`subscriptions/listen` 即刷并持开，`list_changed` 按订阅缓冲 | 原路：`tools/list` 走缓存全量返回；`listen` 与 `list_changed` 走原广播路径 |
| `enable_file_input` | `tools/call` 的 `arguments` 内 `file://`/`resource://` 引用对象按范围切片并内联小文件；`http(s)://` 只透传；越界与超限回错误结果 | 参数原样透传，不做任何引用解析 |
| `enable_triggers_events` | `RegisterTrigger` 注册表 + `EmitTriggerEvent` 按类型匹配，经订阅过滤投递 `notifications/triggers/event`，命中项的 webhook 失败只记 `Warning` 不抛 | `EmitTriggerEvent` 直接返回（事件静默丢弃）；注册/注销本身不限门 |
| `enable_dpop` | 服务端在 Bearer 链上加 DPoP 证明验签（`htm`/`htu` 比对，失败回 401 并带 `DPoP-Nonce`；`jti` 内存重放表 + 窗口淘汰；无 OpenSSL 时硬报错）。生效门是传输侧 `StreamableHttpServerOptions::bearer_auth->enable_dpop`，本 `ServerOptions` 字段已声明但 `src/server` 无读取点（置 true 无效果），详见 [/concepts/dpop.md](/concepts/dpop.md) | 原 Bearer 链，无证明校验 |
| `enable_interceptor_alias` | 别名即实现：`Interceptor` 为 `MessageFilter` 的别名，`AddInterceptor` 转发既有过滤器管线，执行顺序与管线一致，零行为变化；开关本身无门控，开/关无可观测差异 | 同左 |

客户端：

| 开关 | 开：语义 | 关：语义 |
|------|----------|----------|
| `enable_tasks_extension` | `extensions` 映射含任务键即声明；`UpdateTask`/`PollTaskToCompletion` 与任务句柄路径生效 | 上述调用抛 `MethodNotFound`（`tasks extension is disabled`），且声明判定恒为 false |
| `enable_skills_extension` | `ListSkills`/`GetSkill`/`ReadDirectory` 生效 | 三调用抛 `MethodNotFound`（`skills extension is disabled`） |
| `enable_apps_extension` | 保留：已声明、默认 `false`，暂无门控接线 | 同左 |
| `enable_progressive_discovery` | `SearchTools` 翻页请求附 `query`/`pageSize` | 翻页聚合仍可用，但不附 `query`/`pageSize`（服务端按缺省分页返回） |

门控位置：服务端任务守卫（[McpServer.cpp:1205、1246、1472](../../src/server/McpServer.cpp)）、能力推导（[McpServer.cpp:1637、1643、1649](../../src/server/McpServer.cpp)）、任务化调用（[McpServer.cpp:2018](../../src/server/McpServer.cpp)）、技能三方法（[McpServer.cpp:2587、2628、2671](../../src/server/McpServer.cpp)）、apps 资源读门（[McpServer.cpp:2431](../../src/server/McpServer.cpp)）、卡片获取（[McpServer.cpp:2916](../../src/server/McpServer.cpp)）；客户端声明判定（[McpClient.cpp:150](../../src/client/McpClient.cpp)）、技能三调用（[McpClient.cpp:1244、1270、1303](../../src/client/McpClient.cpp)）、任务两调用（[McpClient.cpp:1408、1439](../../src/client/McpClient.cpp)）。六开关接线位置：etag 构造期同步（[McpServer.cpp:505](../../src/server/McpServer.cpp)）+ 结果附哈希（[McpTypesResults.cpp:104-109](../../src/core/McpTypesResults.cpp)）+ 客户端 8 读取路径条件发送与回退（[McpClient.cpp:981-1451](../../src/client/McpClient.cpp)）；渐进发现服务端（[McpServer.cpp:1685](../../src/server/McpServer.cpp)）与客户端（[McpClient.cpp:1056](../../src/client/McpClient.cpp)）；文件输入（[McpServer.cpp:1819](../../src/server/McpServer.cpp)）；触发器发射门（[McpServer.cpp:827](../../src/server/McpServer.cpp)）；dpop 服务端校验（[StreamableHttpServerTransport.cpp:515](../../src/http/StreamableHttpServerTransport.cpp)）与客户端签发（[OAuthClientProvider.cpp:261、684、692](../../src/client/auth/OAuthClientProvider.cpp)）；拦截器别名（[MessageFilter.hpp:21、39](../../include/mcp/protocol/MessageFilter.hpp)）。

## 构造期同步

- `McpServer::Create` 按值拷贝 `ServerOptions`（[McpServer.cpp:499-505](../../src/server/McpServer.cpp)）：构造完成后调用方再改手中的 `options` 不生效；除 etag 外的开关均在请求处理路径实时读实例内 `options_` 拷贝（渐进 `1685`、触发器 `827`、文件输入 `1819` 行）。
- etag 例外：构造期经 `SetEtagCacheEnabled` 同步的是进程级全局开关（[McpTypesResults.cpp:50-56](../../src/core/McpTypesResults.cpp)），客户端 8 读取路径读的是同一全局（[McpClient.cpp:981](../../src/client/McpClient.cpp) 起）——任一 `McpServer` 以 `enable_etag_cache=true` 构造即打开全进程的 etag 读写，同进程的客户端同步生效；关态构造则关闭全进程。
- dpop 双端各自构造期取值：服务端由传输 `config.enable_dpop` 决定（[StreamableHttpServerTransport.cpp:515](../../src/http/StreamableHttpServerTransport.cpp)，且必须先配置 `bearer_auth`），客户端由 `OAuthClientProvider` 的 `options_.enable_dpop` 决定（[OAuthClientProvider.cpp:261](../../src/client/auth/OAuthClientProvider.cpp)），两端须各自显式开启；`ServerOptions::enable_dpop`（[ServerOptions.hpp:90](../../include/mcp/server/ServerOptions.hpp)）无读取点，不参与门控。

## Tasks/Skills 兼容期（0.3.5 → 0.3.8，本次任务给定口径）

- Tasks：隐式路径已移除——仅设 `task_store` 不再声明 tasks 扩展、不再记 `Warning`，必须显式开 `enable_tasks_extension=true`（能力推导 [McpServer.cpp:1637](../../src/server/McpServer.cpp)、任务三方法守卫 `1205、1246、1472`、任务化调用 `2018` 均只认显式开关）。`TasksImplicitlyEnabled()`（[ServerOptions.hpp:93-95](../../include/mcp/server/ServerOptions.hpp)）仍保留 `[[deprecated]]` 标记作公开 API 兼容，但服务端已不再调用。注：0.3.8 为任务口径，代码内无版本标记；现版本为 0.3.5。
- Skills：`SkillsImplicitlyEnabled()`（[ServerOptions.hpp:96-98](../../include/mcp/server/ServerOptions.hpp)）同样标记 deprecated，但实现只返回开关本身、无隐式路径——技能自 `ad547f9` 起必须显式开 `enable_skills_extension=true`，否则能力不声明、三方法 `MethodNotFound`。

## 关/开两态语义

- 关：tasks/skills 对应方法返回/抛出 `MethodNotFound`（服务端与客户端文案均为 `tasks extension is disabled` / `skills extension is disabled`），能力映射不声明对应扩展键；apps 关时 `ui://` 资源读抛 `HandlerError`（`apps extension is disabled`），server_card 关时 `GetServerCard()` 抛 `HandlerError`（`server card is disabled`），能力映射不声明对应扩展键。
- 上条中仅 tasks/skills 报 `MethodNotFound`，apps/server-card 报 `HandlerError`；etag/渐进/文件输入/触发器/dpop 关态不报错，而是走原路（无 etag、全量列表、参数透传、事件丢弃、原 Bearer）；interceptor 为别名，开/关无差异。
- 开：恢复既有行为——前置条件满足即声明扩展键并放行方法：tasks 需显式开开关（服务端，`task_store` 仍为任务持久化前置）/ 扩展映射含任务键（客户端），skills 需已注册技能且具 resources 能力（服务端），apps 开即声明 `ui` 键并放行 `ui://` 资源读，server_card 开即声明卡片键并开放 `GetServerCard()`。tasks 与 skills 行为细节见 [/concepts/tasks-extension.md](/concepts/tasks-extension.md) 与 [/concepts/skills-extension.md](/concepts/skills-extension.md)。
- 技能守卫先于时代守卫：开关关闭时无论 modern/legacy 时代均报 `skills extension is disabled`，优先于时代拒绝。
- 原 T11 遗留已解除（`1668e2e`）：客户端 `enable_listen_stream` 为 `optional<bool>`，未设置时取 `!stateless`（`ShouldListenStream`，[StreamableHttpClientTransport.cpp:148-149](../../src/http/StreamableHttpClientTransport.cpp)），即 **stateful 默认开、stateless 默认关**，显式置 `true` 仍可强制开（`StatelessClientForcedListenStreamSendsGet` 锁定）；口径见 [/transports/streamable-http.md](/transports/streamable-http.md)。
