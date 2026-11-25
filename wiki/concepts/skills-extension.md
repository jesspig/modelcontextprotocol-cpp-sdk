---
type: Concept
title: 技能扩展
description: SEP-2640 技能扩展：skills/list|get 与 resources/directory/read（2026-only）、skill:// URI 约定、manifest/digest、目录派生视图与缓存，以及客户端惰性读取、真实 YAML frontmatter 安全解析、字节预算、modern/legacy 双路由与诊断语义。
tags: [skills, 扩展, 2026, resources, 安全, 预算]
timestamp: 2026-11-26T01:00:00+08:00
resource: src/server/McpServer.cpp
---

# 技能扩展

0.3.13 把 blob 内容的 size/digest 改为比对严格 Base64 **解码后**字节，frontmatter 含分隔符时走真实 YAML 解析（无分隔符保持旧自比较），新增 `SkillReadParts` 惰性读取与 `ResolveSkillLocalPath` 安全解析；两参数旧 `ReadSkill` 行为不变。旧行为/新行为/调用方改法见在线文档 `/zh/migration/0.3.13` 与 `/en/migration/0.3.13`，证据见 `.handoff/evidence/T-07/`。

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

服务端只做形态放行，不做信任判定；是否取用他源技能由客户端经 `SkillVerify` 原语决定（`mcp-client` 库，见 [SkillVerify.cpp](../../src/client/SkillVerify.cpp)，conformance `RunSkillsVerifyClient` 为用例样板）：

- `CheckSkillLimits(skill, error_out)`：manifest 文件数 ≤ 512（`kMaxSkillFiles`）、`size` 总和 ≤ 16 MB（`kMaxSkillTotalBytes`，负 `size` 直接拒绝）；`"dynamic"` 资源直接放行
- `VerifyFileBytes(expected, bytes, error_out)`：`size` 与实取字节等长 + `digest` 为 `sha256:` 前缀（大小写无关）+ 十六进制摘要与内置 SHA-256 实算一致
- `VerifyFrontmatter(advertised, fetched, error_out)`：两份 frontmatter 相等、为对象、`name` 为字符串且等于 uri 名段（`://` 后末段去 `/SKILL.md` 后缀）；保留的 eager 原语
- `ParseFrontmatter(yaml, out, error_out)`：用 rapidyaml `parse_in_arena` 真实解析，逐节点校验映射、字段名/类型，拒绝重复键、别名/锚点、自定义 tag、`<<` merge、隐式标量转换、多文档与 `%` 指令；应用 B-52..B-61 字节/深度/节点/标量/键预算，失败即拒绝、不回退字符串搜索
- `SplitSkillDocument` / `VerifyFrontmatterBytes`：解析 `---` 分隔符并把广告 frontmatter 与真实字节解析结果比对；无分隔符时沿用旧自比较行为
- `SkillBlob::FromEncoded` 先计量 raw size 与 SHA-256 再 `Decode` 严格 Base64（字母表/padding/canonical，拒绝截断/尾随/膨胀），`Frontmatter`/`Body` 惰性展开并各自复用预算
- `ResolveSkillRoute(version, capabilities)`：按协商版本与已声明扩展 capability 双路由；legacy 版本或显式未声明 skills 扩展走 `Legacy`，能力表缺省视为未知、保持旧行为
- `ResolveSkillLocalPath(root, relative, resolved, error_out)`：根目录规范化 + `..` 拒绝 + `weakly_canonical` 符号链接逃逸检查，拒绝绝对路径与越界相对路径
- `IsApprovalStillValid(approved, current)`：uri/frontmatter/meta/resources 四项快照比对，供用户授权持久化后复核
- `ApprovalKey(host_label, skill)`：授权键 `"<host>\x1f<skill-uri>"`

## 高层读取 API（客户端）

`McpClient::ReadSkill(uri, options?)` 与 `McpClient::ReadSkillDirectory(uri, options?)`（`include/mcp/client/McpClient.hpp`、`src/client/McpClient.cpp`）把上述原语串成一次调用，避免调用方漏检 digest。二者为新增、源码兼容的 API，不改 `ListSkills`/`GetSkill`/`ReadDirectory` 行为，也不引入新的 wire 方法或第二套缓存。

- `ReadSkill` 返回 `SkillReadResult{skill, files[], diagnostics[]}`：保留的 eager 入口等价于 `SkillReadParts::All`。先 `GetSkill` 取 manifest，再 `CheckSkillLimits`（B-62/B-63），逐文件按「原始字节 size/SHA-256 → 严格 Base64 解码 → B-64/B-65 预算 → `VerifyFileBytes` size/digest」流水线处理：`TextResourceContents` 直接取文本字节，`BlobResourceContents` 先计量编码串再严格解码，绝不把编码串当正文。对入口资源用 `VerifyFrontmatterBytes` 把广告 frontmatter 与真实 `---` frontmatter 的 rapidyaml 解析结果比对并校验 URI 名段；无分隔符时沿用旧 `VerifyFrontmatter(skill, skill.frontmatter)` 自比较以保持旧调用行为。`"dynamic"` 资源无文件 manifest，跳过下载直接做 frontmatter 校验。legacy 会话在本地即抛 `MethodNotFound`，不发送 modern 技能请求。
- 惰性重载 `ReadSkill(uri, options, parts)`：`FrontmatterOnly` 只读取入口资源、不展开其他 manifest 文件（零预取 B-66），`BodyOnly` 只解码与校验正文、不解析 YAML，`All` 与旧入口一致；两条路径都执行解码与 YAML 预算检查，调用方只在需要时展开对应部分。
- `ReadSkillDirectory` 对子项调用 `ReadSkill(uri, options, All)`，行为与旧版一致。
- `ReadSkillDirectory` 返回 `SkillDirectoryReadResult{skills[], diagnostics[]}`：按 `ReadDirectory` 既有 cursor 分页遍历（上限 `kMaxListPages` = 64 页），仅把以 `/SKILL.md` 结尾的子项当作技能逐项执行与 `ReadSkill` 相同的流水线；校验通过的条目进入 `skills`，失败的条目跳过并把其诊断汇入 `diagnostics`，因此部分无效目录下有效技能仍返回。`ReadDirectory` 不可用时仍按其既有 `McpError` 语义抛出。

两类结果均为结构化结果：校验失败**不抛异常**，而是写入 `diagnostics`，已验证通过的 `files` 作为部分结果保留；调用方须在 `diagnostics` 为空时才把结果当作可用技能。网络/协议错误仍沿既有 `McpError` 透传。`diagnostics[].uri` 为触发诊断的技能资源 URI；`diagnostics[].reason` 取以下值：

| reason | 来源原语与触发条件 |
|--------|--------------------|
| `size_mismatch` | `VerifyFileBytes` 解码后字节长度与 manifest `size` 不等；`CheckSkillLimits` 的文件数/总字节/负 size 超限亦归入此值 |
| `digest_mismatch` | `VerifyFileBytes` 的 `sha256:` 摘要缺失或实算十六进制摘要不匹配 |
| `frontmatter_mismatch` | `VerifyFrontmatter` 判定两份 frontmatter 不等、非对象、缺字符串 `name` 或 `name` 与 URI 名段不符 |
| `invalid_encoding` | 原始编码字节超限、Base64 字母表/padding/canonical/截断/尾随错误或解码后字节超预算；不返回部分正文 |
| `frontmatter_invalid` | `---` 分隔符缺失闭合、frontmatter 字节/深度/节点/标量/键预算超限或 rapidyaml 解析/准入失败；不回退为字符串搜索 |

分页与缓存完全复用既有策略：`ReadSkill` 把 `CacheableRequestOptions` 透传给 `GetSkill`/`ReadResource`，`ReadSkillDirectory` 透传给 `ReadDirectory`，键与 TTL 沿用原方法，不新增缓存实例。

## 相关页面

- [/classes/mcp-server.md](../classes/mcp-server.md) — `RegisterSkill`/`SetSkillsDirectoryRead` 与三处理器
- [/classes/mcp-client.md](../classes/mcp-client.md) — 技能三方法与缓存
- [/classes/wire-codec.md](../classes/wire-codec.md) — 2026 方法集合与列表校验
- [/concepts/version-negotiation.md](version-negotiation.md) — 门控表
