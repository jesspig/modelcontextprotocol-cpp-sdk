# AGENTS.md — wiki 知识库编辑规范

适用范围：`wiki/` 全部页面。源码知识库入口 [index.md](index.md)，与 `docs/zh`、`docs/en` 在线文档分离。

- 更新前对照 `git status` 与 `git diff HEAD`（含用户手动改动），禁止凭对话记忆；无法核实处标 `> [!todo] 待补充`，彻底删除过时描述。
- 概念页（modules/classes/transports/concepts）必须含 YAML frontmatter：`type` 必填，推荐 `title`/`description`/`tags`/`timestamp`（真实 ISO 8601，仅内容实际变更时更新），对应源码资产加 `resource`；`index.md` 与 `log.md` 无 frontmatter。
- `changelog/<YYYY-MM-DD>-log.md` 按天分文件，条目时间戳 `<YYYY-MM-DD-HH>`；`log.md` 仅保留最近 7 条。
- 链接以 `/` 指向知识库根或使用相对链接；行号写进链接文本如 `[Foo.cpp:12](path/Foo.cpp)`，禁止写进 URL；正文禁止大段粘贴源码。
