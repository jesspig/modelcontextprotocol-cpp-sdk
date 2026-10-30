---
type: Concept
title: 实验开关
description: 服务端 8 开关与客户端 4 开关：tasks/skills/apps/server-card 门控语义、tasks 隐式兼容已移除（只设 task_store 不再声明，需显式开）、etag/渐进发现/文件输入/触发器已实现语义与构造期同步、DPoP 门在传输侧而非 ServerOptions、interceptor 为无门控别名。
tags: [experimental, 实验开关, tasks, skills, apps, server-card, 兼容]
timestamp: 2026-10-30T23:55:50+08:00
resource: include/mcp/server/ServerOptions.hpp
---

# 实验开关

`ad547f9` 起 Tier1 对齐引入的实验能力大多默认关闭，须显式开启。服务端 8 开关（[ServerOptions.hpp:80-88](../../include/mcp/server/ServerOptions.hpp)），客户端 4 开关（[ClientOptions.hpp:71-75](../../include/mcp/client/ClientOptions.hpp)）。服务端 8 开关与客户端 tasks/skills/渐进发现默认 `false`，客户端 etag 默认 `true`。其中 tasks/skills 为最早接线的一批；etag/渐进发现/文件输入/触发器四开关在后续批次全部接线（默认关实现）；apps/server_card 两开关在本轮接线（能力声明 + 请求门控，见下）。DPoP 不在本表——服务端门为传输侧 `StreamableHttpServerOptions::BearerAuthConfig::enable_dpop`，客户端为 `OAuthClientOptions::enable_dpop`（见 [/concepts/dpop.md](/concepts/dpop.md)）；`Interceptor` 仍为 `MessageFilter` 的无门控别名。

## 开关表

服务端：

| 开关 | 开：语义 | 关：语义 |
|------|----------|----------|
| `enable_tasks_extension` | 声明 tasks 扩展；`tasks/get\|update\|cancel` 与任务化 `tools/call` 生效 | 无 `task_store` 时对应方法 `MethodNotFound`（`tasks extension is disabled`）；仅设 `task_store` 不再声明，必须显式开（见下） |
| `enable_skills_extension` | 已注册技能且具 resources 能力时声明 skills 扩展；`skills/list\|get` 与 `directory/read` 生效 | 三方法一律 `MethodNotFound`（`skills extension is disabled`），即使已注册技能；无隐式路径 |
| `enable_apps_extension` | 声明 `io.modelcontextprotocol/ui` 扩展（[Apps.hpp:11](../../include/mcp/ext/Apps.hpp) 的 `kAppsExtensionId`，对标 C# `McpApps.ExtensionId`）；开时 `ui://` 资源读走正常路径并经 `ValidateUiMeta` 校验 | 关时 `ui://` 的 `resources/read` 一律 `HandlerError`（`apps extension is disabled`），非 `ui://` 资源不受影响 |
| `enable_server_card` | 声明 `io.modelcontextprotocol/server-card` 扩展（[ServerCard.hpp:14](../../include/mcp/ext/ServerCard.hpp) 的 `kServerCardExtensionId`，SEP-2127）；开时 `GetServerCard()` 返回显式卡（`server_card`，[ServerOptions.hpp:84](../../include/mcp/server/ServerOptions.hpp)）或由 `server_info` + 支持版本构建的默认卡，均经 `ValidateCard` 校验 | 关时 `GetServerCard()` 抛 `HandlerError`（`server card is disabled`）；HTTP `GET /server-card` 仍由传输侧同名开关独立门控 |
| `enable_etag_cache` | 列表类结果序列化附内容哈希 `etag`（载荷超 `kEtagMaxBytes` 8MB 跳过不附；TTL 语义不动）；客户端 8 读取路径经 meta 扩展附 `ifNoneMatch` 条件发送，服务端回 `notModified` 时以存表回退 | 全局关闭：结果不附 `etag`，客户端存取表短路（`GetStoredEtag`/`StoreEtagBody` 直接返回），无条件发送 |
| `enable_progressive_discovery` | `tools/list` 支持 `query` 子串过滤 + `pageSize`（1–100）翻页，不再走缓存全量；`subscriptions/listen` 即刷并持开，`list_changed` 按订阅缓冲 | 原路：`tools/list` 走缓存全量返回；`listen` 与 `list_changed` 走原广播路径 |
| `enable_file_input` | `tools/call` 的 `arguments` 内 `file://`/`resource://` 引用对象按范围切片并内联小文件；`http(s)://` 只透传；越界与超限回错误结果 | 参数原样透传，不做任何引用解析 |
| `enable_triggers_events` | `RegisterTrigger` 注册表 + `EmitTriggerEvent` 按类型匹配，经订阅过滤投递 `notifications/triggers/event`，命中项的 webhook 失败只记 `Warning` 不抛 | `EmitTriggerEvent` 直接返回（事件静默丢弃）；注册/注销本身不限门 |

客户端：

| 开关 | 开：语义 | 关：语义 |
|------|----------|----------|
| `enable_tasks_extension` | 开关开启且 `extensions` 映射含任务键才声明；`UpdateTask`/`PollTaskToCompletion` 与任务句柄路径生效 | 上述调用抛 `MethodNotFound`（`tasks extension is disabled`），且声明判定恒为 false |
| `enable_skills_extension` | `ListSkills`/`GetSkill`/`ReadDirectory` 生效 | 三调用抛 `MethodNotFound`（`skills extension is disabled`） |
| `enable_progressive_discovery` | `SearchTools` 翻页请求附 `query`/`pageSize` | 翻页聚合仍可用，但不附 `query`/`pageSize`（服务端按缺省分页返回） |
| `enable_etag_cache`（默认 `true`） | 全局 `SetEtagCacheEnabled` 同时开启时，8 读取路径经 meta 附 `ifNoneMatch`，`notModified` 时以存表回退 | 客户端不附条件、不读存表，请求照常发送 |

门控位置：服务端任务守卫（[McpServer.cpp:1452、1493、1731](../../src/server/McpServer.cpp)）、能力推导（[McpServer.cpp:1897、1903、1909、1915](../../src/server/McpServer.cpp)）、任务化调用（[McpServer.cpp:2279](../../src/server/McpServer.cpp)）、技能三方法（[McpServer.cpp:2883、2924、2967](../../src/server/McpServer.cpp)）、apps 资源读门（[McpServer.cpp:2696](../../src/server/McpServer.cpp)）、卡片获取（[McpServer.cpp:3227](../../src/server/McpServer.cpp)）；客户端声明判定（[McpClient.cpp:182](../../src/client/McpClient.cpp)）、技能三调用（[McpClient.cpp:1524、1558、1599](../../src/client/McpClient.cpp)）、任务两调用（[McpClient.cpp:1712、1743](../../src/client/McpClient.cpp)）。四开关接线位置：etag 构造期同步（[McpServer.cpp:658](../../src/server/McpServer.cpp)）+ 结果附哈希（[McpTypesResults.cpp:104-109](../../src/core/McpTypesResults.cpp)）+ 客户端 8 读取路径条件发送与回退（[McpClient.cpp:1139-1633](../../src/client/McpClient.cpp)）；渐进发现服务端（[McpServer.cpp:1950](../../src/server/McpServer.cpp)）与客户端（[McpClient.cpp:1224](../../src/client/McpClient.cpp)）；文件输入（[McpServer.cpp:2084](../../src/server/McpServer.cpp)）；触发器发射门（[McpServer.cpp:1070](../../src/server/McpServer.cpp)）。

## 构造期同步

- `McpServer::Create` 按值拷贝 `ServerOptions`（[McpServer.cpp:643-655](../../src/server/McpServer.cpp)）：构造完成后调用方再改手中的 `options` 不生效；除 etag 外的开关均在请求处理路径实时读实例内 `options_` 拷贝（渐进 `1950`、触发器 `1070`、文件输入 `2084` 行）。
- etag 例外：构造期经 `SetEtagCacheEnabled` 同步的是进程级全局开关（[McpTypesResults.cpp:50-56](../../src/core/McpTypesResults.cpp)），客户端 8 读取路径读的是同一全局（[McpClient.cpp:1139](../../src/client/McpClient.cpp) 起）——任一 `McpServer` 以 `enable_etag_cache=true` 构造即打开全进程的 etag 读写，同进程的客户端同步生效；关态构造则关闭全进程。
- DPoP 不属于本页开关：服务端门在传输配置 `StreamableHttpServerOptions::BearerAuthConfig::enable_dpop`（[StreamableHttpServerTransport.cpp:515](../../src/http/StreamableHttpServerTransport.cpp)，且必须先配置 `bearer_auth`），客户端门由 `OAuthClientProvider` 的 `options_.enable_dpop` 决定（[OAuthClientProvider.cpp:366](../../src/client/auth/OAuthClientProvider.cpp)），两端须各自显式开启；`ServerOptions` 已无 dpop 字段。

## Tasks/Skills 兼容期（0.3.5 → 0.3.8，本次任务给定口径）

- Tasks：隐式路径已移除——仅设 `task_store` 不再声明 tasks 扩展、不再记 `Warning`，必须显式开 `enable_tasks_extension=true`（能力推导 [McpServer.cpp:1897](../../src/server/McpServer.cpp)、任务三方法守卫 `1452、1493、1731`、任务化调用 `2279` 均只认显式开关）。`TasksImplicitlyEnabled()`（[ServerOptions.hpp:90-92](../../include/mcp/server/ServerOptions.hpp)）仍保留 `[[deprecated]]` 标记作公开 API 兼容，但服务端已不再调用。注：0.3.8 为任务口径，代码内无版本标记；现版本为 0.3.9。
- Skills：`SkillsImplicitlyEnabled()`（[ServerOptions.hpp:93-95](../../include/mcp/server/ServerOptions.hpp)）同样标记 deprecated，但实现只返回开关本身、无隐式路径——技能自 `ad547f9` 起必须显式开 `enable_skills_extension=true`，否则能力不声明、三方法 `MethodNotFound`。

## 关/开两态语义

- 关：tasks/skills 对应方法返回/抛出 `MethodNotFound`（服务端与客户端文案均为 `tasks extension is disabled` / `skills extension is disabled`），能力映射不声明对应扩展键；apps 关时 `ui://` 资源读抛 `HandlerError`（`apps extension is disabled`），server_card 关时 `GetServerCard()` 抛 `HandlerError`（`server card is disabled`），能力映射不声明对应扩展键。
- 上条中仅 tasks/skills 报 `MethodNotFound`，apps/server-card 报 `HandlerError`；etag/渐进/文件输入/触发器关态不报错，而是走原路（无 etag、全量列表、参数透传、事件丢弃）。`Interceptor` 是无门控别名，与本页开关无关。
- 开：恢复既有行为——前置条件满足即声明扩展键并放行方法：tasks 需显式开开关（服务端，`task_store` 仍为任务持久化前置）/ 扩展映射含任务键（客户端），skills 需已注册技能且具 resources 能力（服务端），apps 开即声明 `ui` 键并放行 `ui://` 资源读，server_card 开即声明卡片键并开放 `GetServerCard()`。tasks 与 skills 行为细节见 [/concepts/tasks-extension.md](/concepts/tasks-extension.md) 与 [/concepts/skills-extension.md](/concepts/skills-extension.md)。
- 技能守卫先于时代守卫：开关关闭时无论 modern/legacy 时代均报 `skills extension is disabled`，优先于时代拒绝。
- 原 T11 遗留已解除（`1668e2e`）：客户端 `enable_listen_stream` 为 `optional<bool>`，未设置时取 `!stateless`（`ShouldListenStream`，[StreamableHttpClientTransport.cpp:148-149](../../src/http/StreamableHttpClientTransport.cpp)），即 **stateful 默认开、stateless 默认关**，显式置 `true` 仍可强制开（`StatelessClientForcedListenStreamSendsGet` 锁定）；口径见 [/transports/streamable-http.md](/transports/streamable-http.md)。
