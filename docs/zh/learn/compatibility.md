# 弃用登记表

本页是兼容性宪法要求的弃用登记表：每条弃用项给出起始版本、替代写法与移除版本，到期版本不删除即视为违约。分级口径见 [0.3.11 发布说明](/zh/migration/0.3.11) 的「兼容性分级」。

## 登记规则

- 弃用项必须保留实现并标注 `[[deprecated("since X.Y.Z, removed in A.B.C; <替代写法>")]]`，弃用窗口至少跨一个 minor 版本。
- 新增或删除弃用项时，必须同批更新本页与对应的迁移/发布说明，并保持中英两侧一致。
- 由协议修订而非本库版本决定的弃用，移除版本标注为「随协议」，不承诺具体版本号。

## 当前登记项

| API | 起始 | 替代写法 | 移除版本 |
|---|---|---|---|
| `mcp::McpErrorCode::ResourceNotFound` | 0.3.11 | `McpErrorCode::InvalidParams`（modern era 已不发 `-32002`） | 0.4.0 |
| `McpClient::Ping()` | 2026-07-28 协议修订 | 无（协议移除该方法） | 随协议 |
| `McpClient::SetSamplingHandler()` | SEP-2577 | `SetElicitationHandler()` | 随协议 |
| `McpClient::SetRootsHandler()` | SEP-2577 | 无（roots 在 2026-07-28 弃用） | 随协议 |
| `McpClient::SetLegacyUrlElicitationHandler()` | 0.3.9 | `SetUrlElicitationHandler()`（三态结果） | 随协议 |
| `ServerOptions::TasksImplicitlyEnabled()` | 0.3.7 | 显式设置 `enable_tasks_extension=true` | 随协议 |
| `ServerOptions::SkillsImplicitlyEnabled()` | 0.3.7 | 显式设置 `enable_skills_extension=true` | 随协议 |

## 0.3.13 核验

- 本轮逐条核对公共头文件的 `[[deprecated]]` 属性：`ResourceNotFound` 标注 `since 0.3.11, removed in 0.4.0`，替代写法为 `InvalidParams`，弃用窗口跨 0.3.11 → 0.3.13 一个以上 minor，尚未到期，实现保留。
- `Ping`、`SetSamplingHandler`、`SetRootsHandler`、`SetLegacyUrlElicitationHandler`、`TasksImplicitlyEnabled`、`SkillsImplicitlyEnabled` 的起始与移除由协议修订或更早版本决定，登记表统一写「随协议」，属性中的替代写法与上表逐字一致；实现均保留。
- **0.3.13 候选没有新增弃用项，也没有到期删除项**；没有为了「清理」而静默删除旧 API。`tests/compat/expected_breaks.txt` 与 0.3.12 逐字一致，条目只减不增。
- 逐项核对记录见 `.handoff/evidence/T-24/deprecation-review.txt`，迁移对照见 [0.3.13 迁移说明](/zh/migration/0.3.13)。

## 到期检查

发版前逐条核对：到期版本是否已删除、本页与 `docs/{zh,en}/migration/` 是否同步、`mcp-compat-smoke` 是否仍通过；任何一项不满足都不得发版。
