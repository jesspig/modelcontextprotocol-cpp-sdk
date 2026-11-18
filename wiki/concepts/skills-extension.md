---
type: Concept
title: 技能扩展
description: SEP-2640 技能扩展：skills/list|get 与 resources/directory/read（2026-only）、skill:// URI 约定、manifest/digest、目录派生视图与缓存，以及客户端高层读取 API 与诊断语义。
tags: [skills, 扩展, 2026, resources]
timestamp: 2026-11-19T00:00:00+08:00
resource: src/server/McpServer.cpp
---

# 技能扩展

SEP-2640 技能扩展（扩展标识 `io.modelcontextprotocol/skills`，[Capabilities.hpp](../../include/mcp/Capabilities.hpp) 的 `kSkillsExtensionId`）让服务端发布可列举、可按 URI 获取的技能包，并可选支持目录浏览。

## 扩展声明

服务端同时满足三项时，`DeriveCapabilities` 在 `capabilities_.extensions` 中声明 `io.modelcontextprotocol/skills`（[McpServer.cpp](../../src/server/McpServer.cpp)）：`ServerOptions::enable_skills_extension` 为真**且** `skills_` 非空**且** `capabilities_.resources` 具备。值为 `SerializeSkillsExtensionSettings`——默认空对象，仅 `SetSkillsDirectoryRead(true)` 后携带 `"directoryRead": true`。开关关闭时三处理器入口一律回 `MethodNotFound "skills extension is disabled"`（[McpServer.cpp](../../src/server/McpServer.cpp)）。

## 三方法（均仅 2026）

| 方法 | 请求 | 结果 | 门控 |
|------|------|------|------|
| skills/list | `PaginatedRequestParams`（cursor） | `SkillsListResult{skills[], next_cursor?, ttlMs?, cacheScope?}` | modern + resources 能力 |
| skills/get | `{uri}` | `SkillsGetResult{skill, ttlMs?, cacheScope?}` | modern + resources 能力；未知 uri → `InvalidParams "skill not found: ..."` |
| resources/directory/read | `{uri, cursor?}` | `ReadDirectoryResult{resources[], next_cursor?, ttlMs?, cacheScope?}` | modern + resources 能力 + `directoryRead` 已声明；未知目录 → `InvalidParams "directory not found: ..."` |

三处理器均经 `RequireInitialized` 守卫；legacy 时代回 `MethodNotFound`。wire 层三方法仅属 2026 方法集合；2026 响应校验的列表方法集合含 `skills/list`（`resultType` 须为 `"complete"`）；结果的 `ttlMs`/`cacheScope` 由 `GetCacheHint` 按方法键回填（`WriteListResultCommon` 同时写嵌套 `cacheHint` 与顶层扁平字段）。

## 数据模型

- `Skill{uri, frontmatter, resources, meta?}`：`uri` 须为形态 `<scheme>://<名>/SKILL.md`（scheme 无关形态校验：含非空 `://`、名段非空、以 `/SKILL.md` 结尾；`RegisterSkill` 不符抛 `InvalidParams`；同 uri 覆盖）。`skill://` 仍为约定 scheme，但 `https://example.com/.../SKILL.md` 等他源形态同样放行（是否信任由调用方经下述验签助手判定）。`resources` 为二选一：文件 manifest（`SkillResource[]{uri, digest, size}` 数组）或字符串 `"dynamic"`（内容解析推迟到读取时）
- `SkillResources` 反序列化接受数组或字符串，其余形态抛 `DeserializeFailed`
- `DirectoryChild{uri, name, mimeType?, meta?}`：目录视图由 `CollectDirectoryChildren` 从已注册技能派生——skill 根（`SKILL.md` 剥后缀）与 manifest 资源子路径构成目录树，直属文件为叶子、中间段标 `mime_type "inode/directory"`，按 uri 排序；请求 uri 归一化（去尾斜杠）后匹配，`PaginateEntries`（100/页）分页

## 客户端

`McpClient::ListSkills(cursor?)`（翻页聚合 + 缓存，键 `skills/list\x1F<cursor>`，嵌套 `cacheHint` 扁平回填后再缓存）、`GetSkill(uri, options?)`（`CacheableRequestOptions`：`cache_mode use/bypass/refresh` + `max_age_ms`，键带 uri）、`ReadDirectory(uri, cursor?, options?)`（键带 uri + cursor）。TTL（`ttlMs > 0` 才缓存，钳制 24h）与 public/private 双分区语义与既有列表/读资源缓存一致。

## 验签助手（客户端侧，`mcp/client/SkillVerify.hpp`）

服务端只做形态放行，不做信任判定；是否取用他源技能由客户端经 `SkillVerify` 五助手决定（`mcp-client` 库，见 `src/client/SkillVerify.cpp`，conformance `RunSkillsVerifyClient` 为用例样板）：

- `CheckSkillLimits(skill, error_out)`：manifest 文件数 ≤ 512（`kMaxSkillFiles`）、`size` 总和 ≤ 16 MB（`kMaxSkillTotalBytes`，负 `size` 直接拒绝）；`"dynamic"` 资源直接放行
- `VerifyFileBytes(expected, bytes, error_out)`：`size` 与实取字节等长 + `digest` 为 `sha256:` 前缀（大小写无关）+ 十六进制摘要与内置 SHA-256 实算一致
- `VerifyFrontmatter(advertised, fetched, error_out)`：两份 frontmatter 相等、为对象、`name` 为字符串且等于 uri 名段（`://` 后末段去 `/SKILL.md` 后缀）
- `IsApprovalStillValid(approved, current)`：uri/frontmatter/meta/resources 四项快照比对，供用户授权持久化后复核
- `ApprovalKey(host_label, skill)`：授权键 `"<host>\x1f<skill-uri>"`

## 高层读取 API（客户端）

`McpClient::ReadSkill(uri, options?)` 与 `McpClient::ReadSkillDirectory(uri, options?)`（`include/mcp/client/McpClient.hpp`、`src/client/McpClient.cpp`）把上述原语串成一次调用，避免调用方漏检 digest。二者为新增、源码兼容的 API，不改 `ListSkills`/`GetSkill`/`ReadDirectory` 行为，也不引入新的 wire 方法或第二套缓存。

- `ReadSkill` 返回 `SkillReadResult{skill, files[], diagnostics[]}`：先 `GetSkill` 取 manifest，再 `CheckSkillLimits`，然后按文件 manifest 逐项 `ReadResource` + `VerifyFileBytes`，最后 `VerifyFrontmatter(skill, skill.frontmatter)` 做 frontmatter 自洽与 URI 名称一致性校验（项目无 YAML 解析器，校验对象是 manifest 已声明的 frontmatter；检查其为对象、含字符串 `name` 且等于 `<scheme>://<名>/SKILL.md` 的名段）。`"dynamic"` 资源无文件 manifest，跳过下载直接做 frontmatter 校验。
- `ReadSkillDirectory` 返回 `SkillDirectoryReadResult{skills[], diagnostics[]}`：按 `ReadDirectory` 既有 cursor 分页遍历（上限 `kMaxListPages` = 64 页），仅把以 `/SKILL.md` 结尾的子项当作技能逐项执行与 `ReadSkill` 相同的流水线；校验通过的条目进入 `skills`，失败的条目跳过并把其诊断汇入 `diagnostics`，因此部分无效目录下有效技能仍返回。`ReadDirectory` 不可用时仍按其既有 `McpError` 语义抛出。

两类结果均为结构化结果：校验失败**不抛异常**，而是写入 `diagnostics`，已验证通过的 `files` 作为部分结果保留；调用方须在 `diagnostics` 为空时才把结果当作可用技能。网络/协议错误仍沿既有 `McpError` 透传。`diagnostics[].uri` 为触发诊断的技能资源 URI；`diagnostics[].reason` 取三值之一：

| reason | 来源原语与触发条件 |
|--------|--------------------|
| `size_mismatch` | `VerifyFileBytes` 实取字节长度与 manifest `size` 不等；`CheckSkillLimits` 的文件数/总字节/负 size 超限亦归入此值 |
| `digest_mismatch` | `VerifyFileBytes` 的 `sha256:` 摘要缺失或实算十六进制摘要不匹配 |
| `frontmatter_mismatch` | `VerifyFrontmatter` 判定两份 frontmatter 不等、非对象、缺字符串 `name` 或 `name` 与 URI 名段不符 |

分页与缓存完全复用既有策略：`ReadSkill` 把 `CacheableRequestOptions` 透传给 `GetSkill`/`ReadResource`，`ReadSkillDirectory` 透传给 `ReadDirectory`，键与 TTL 沿用原方法，不新增缓存实例。

## 相关页面

- [/classes/mcp-server.md](../classes/mcp-server.md) — `RegisterSkill`/`SetSkillsDirectoryRead` 与三处理器
- [/classes/mcp-client.md](../classes/mcp-client.md) — 技能三方法与缓存
- [/classes/wire-codec.md](../classes/wire-codec.md) — 2026 方法集合与列表校验
- [/concepts/version-negotiation.md](version-negotiation.md) — 门控表
