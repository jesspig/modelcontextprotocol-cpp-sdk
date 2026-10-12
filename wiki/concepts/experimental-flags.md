---
type: Concept
title: 实验开关
description: 服务端 10 开关与客户端 3 开关（默认全关）：tasks/skills 双端门控语义、tasks 隐式兼容过渡与 Warning、其余保留开关未接线说明。
tags: [experimental, 实验开关, tasks, skills, 兼容]
timestamp: 2026-10-12T17:48:18+08:00
resource: include/mcp/server/ServerOptions.hpp
---

# 实验开关

`ad547f9` 起 Tier1 对齐引入的实验能力默认全部关闭，须显式开启。服务端 10 开关（[ServerOptions.hpp:80-89](../../include/mcp/server/ServerOptions.hpp)），客户端 3 开关（[ClientOptions.hpp:51-53](../../include/mcp/client/ClientOptions.hpp)），默认值均为 `false`。

## 开关表

服务端：

| 开关 | 开：语义 | 关：语义 |
|------|----------|----------|
| `enable_tasks_extension` | 声明 tasks 扩展；`tasks/get\|update\|cancel` 与任务化 `tools/call` 生效 | 无 `task_store` 时对应方法 `MethodNotFound`（`tasks extension is disabled`）；仅设 `task_store` 时走隐式兼容（见下） |
| `enable_skills_extension` | 已注册技能且具 resources 能力时声明 skills 扩展；`skills/list\|get` 与 `directory/read` 生效 | 三方法一律 `MethodNotFound`（`skills extension is disabled`），即使已注册技能；无隐式路径 |
| `enable_apps_extension` | 保留：已声明、默认 `false`，暂无门控接线（apps 校验见 [Apps.hpp](../../include/mcp/ext/Apps.hpp)，独立于开关） | 同左 |
| `enable_server_card` | 保留：已声明、默认 `false`，暂无门控接线（校验见 [ServerCard.hpp](../../include/mcp/ext/ServerCard.hpp)，独立于开关） | 同左 |
| `enable_etag_cache` | 保留：已声明、默认 `false`，暂无门控接线 | 同左 |
| `enable_progressive_discovery` | 保留：已声明、默认 `false`，暂无门控接线 | 同左 |
| `enable_file_input` | 保留：已声明、默认 `false`，暂无门控接线 | 同左 |
| `enable_triggers_events` | 保留：已声明、默认 `false`，暂无门控接线 | 同左 |
| `enable_dpop` | 保留：已声明、默认 `false`，暂无门控接线 | 同左 |
| `enable_interceptor_alias` | 保留：已声明、默认 `false`，暂无门控接线 | 同左 |

客户端：

| 开关 | 开：语义 | 关：语义 |
|------|----------|----------|
| `enable_tasks_extension` | `extensions` 映射含任务键即声明；`UpdateTask`/`PollTaskToCompletion` 与任务句柄路径生效 | 上述调用抛 `MethodNotFound`（`tasks extension is disabled`），且声明判定恒为 false |
| `enable_skills_extension` | `ListSkills`/`GetSkill`/`ReadDirectory` 生效 | 三调用抛 `MethodNotFound`（`skills extension is disabled`） |
| `enable_apps_extension` | 保留：已声明、默认 `false`，暂无门控接线 | 同左 |

门控位置：服务端任务守卫（[McpServer.cpp:1124、1168、1397](../../src/server/McpServer.cpp)）、能力推导（[McpServer.cpp:1565-1574](../../src/server/McpServer.cpp)）、任务化调用（[McpServer.cpp:1732-1733](../../src/server/McpServer.cpp)）、技能三方法（[McpServer.cpp:2288、2329、2372](../../src/server/McpServer.cpp)）；客户端声明判定（[McpClient.cpp:150](../../src/client/McpClient.cpp)）、技能三调用（[McpClient.cpp:1244、1270、1303](../../src/client/McpClient.cpp)）、任务两调用（[McpClient.cpp:1408、1439](../../src/client/McpClient.cpp)）。

## Tasks/Skills 兼容期（0.3.5 → 0.3.8，本次任务给定口径）

- Tasks：仅设 `task_store` 而未显式开 `enable_tasks_extension` 时仍隐式启用（[ServerOptions.hpp:91-93](../../include/mcp/server/ServerOptions.hpp) 的 `TasksImplicitlyEnabled()`），但每次经过能力推导与任务路径均记 `Warning` 日志（[McpServer.cpp:51](../../src/server/McpServer.cpp) 的 `kTasksImplicitWarning` 文本）。该辅助标记 `[[deprecated("set enable_tasks_extension=true explicitly")]]`，0.3.8 起计划移除隐式路径，届时只设 `task_store` 不再声明 tasks 扩展。注：0.3.8 为任务口径，代码内无版本标记；现版本为 0.3.5。
- Skills：`SkillsImplicitlyEnabled()`（[ServerOptions.hpp:94-96](../../include/mcp/server/ServerOptions.hpp)）同样标记 deprecated，但实现只返回开关本身、无隐式路径——技能自 `ad547f9` 起必须显式开 `enable_skills_extension=true`，否则能力不声明、三方法 `MethodNotFound`。

## 关/开两态语义

- 关：对应方法返回/抛出 `MethodNotFound`（服务端与客户端文案均为 `tasks extension is disabled` / `skills extension is disabled`），能力映射不声明对应扩展键。
- 开：恢复既有行为——前置条件满足即声明扩展键并放行方法：tasks 需设 `task_store`（服务端）/ 扩展映射含任务键（客户端），skills 需已注册技能且具 resources 能力（服务端）。tasks 与 skills 行为细节见 [/concepts/tasks-extension.md](/concepts/tasks-extension.md) 与 [/concepts/skills-extension.md](/concepts/skills-extension.md)。
- 技能守卫先于时代守卫：开关关闭时无论 modern/legacy 时代均报 `skills extension is disabled`，优先于时代拒绝。
