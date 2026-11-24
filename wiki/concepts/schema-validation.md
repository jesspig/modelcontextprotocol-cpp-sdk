---
type: Concept
title: JSON Schema 校验双引擎
description: detail 层保留 legacy 校验入口，并以显式 SchemaMode::Modern2020 启用内部 2020-12 编译/求值核心；说明动态作用域、RE2 方言限制与失败关闭预算。
tags: [schema, 2020-12, re2, dynamicRef, 预算, fail-closed]
timestamp: 2026-11-24T22:36:48+08:00
resource: src/detail/SchemaValidation.cpp
---

# JSON Schema 校验双引擎

SDK 内存在两套 JSON Schema 校验语义，边界由调用点显式选择，而不是按协议版本隐式切换。`detail::ValidateJsonSchema` 的三参数形式仍是 legacy 校验器；新的 2020-12 求值核心只有在传入 `SchemaMode::Modern2020`（或直接调用 `ValidateJsonSchema2020`）时才生效。两者都位于 `detail/` 内部层，不属于安装后的用户 API。

## 为什么默认不切换

根 AGENTS 的兼容优先规则要求：源码兼容但行为收紧的改动必须同批给出「旧行为 / 新行为 / 受影响怎么改」的迁移说明、完整 old/new 代码块与可编译 snippet。迁移页由 T-24 统一维护，本任务不写全局迁移页，因此默认继续走 legacy，新引擎通过显式模式开放。这是 A 类改动（仅新增内部实现与显式入口，旧行为零变化），不是破坏性变更。`CompareJsonSchemaEngines` 只做 shadow 对比并报告 `diverged`，不替调用方改变判定。

## 编译期与求值期分离

新引擎先由 `SchemaCompiler::Compile` 建立资源/anchor/dynamicAnchor 图，再交给 `EvaluationEngine::Validate` 对实例求值。`CompiledSchema` 自持 schema 副本与 RE2 编译结果，因此可编译一次、多次求值，且不受调用方临时对象生命周期影响。编译期完成以下检查：方言白名单、关键字白名单、所有 `$ref`/`$dynamicRef`/`$recursiveRef` 可解析、正则可编译。任何一项失败立即抛 `McpError(InvalidParams, ...)`，错误串带 `SL.*` 分类器与 schema 位置。

## 动态作用域的关键规则

`$dynamicRef` 不是普通 `$ref`：只有初始目标自带同名 `$dynamicAnchor` 时才进入动态解析，并在动态作用域中**从最外层向内层**查找同名动态锚点。这一方向性决定了「扩展 schema 覆盖基础 schema」的语义；若从内向外找，会把已经离开的作用域误当作当前目标。作用域条目在资源发生变化时压入/弹出（`$id` 引入新资源、引用跳转到新资源），根节点作为最外层条目始终在栈中。

## RE2 方言与适配层

安全正则固定为 RE2 `2025-11-05 @ 927f5d53...`，Abseil `20260817.0 @ 2065f4de...`；`RE2_INSTALL=OFF`、`RE2_BUILD_TESTING=OFF`、`RE2_USE_ICU=OFF`，Abseil CRT 与 SDK 逐档一致。匹配使用 `RE2::PartialMatch` 的 search 语义，UTF-8，`max_mem` 显式设为 8388608 字节。RE2 不是完整 ECMA-262：无 lookaround/backreference；非法 pattern 在编译期拒绝（legacy 的 fail-open 只属于旧入口）。适配层把常见 ECMA-262 长属性名映射到 RE2 内置短别名（如 `\p{Letter}` → `\p{L}`），未知长名仍由 RE2 拒绝，不为此启用 ICU。

## 离线引用与预算

默认完全离线：非片段引用一律以 `SL.RemoteRef` 拒绝，不解析 DNS、不联网。外部文档须通过 `SchemaOptions::registry` 显式注册，并计入文档数/字节预算。`$id` 的相对 URI 与 dot-segment 按 RFC 3986 归一化，fragment 先百分号解码再解析。T-03 的 B-37..B-51 给出 15 条整数预算（文档字节、总字节、文档数、引用深度、结构深度、节点数、求值步数、时间、`uniqueItems` 规模、展开节点、正则 pattern/subject/max_mem/缓存、编译失败放行数）；超限时在分配或求值前终止并返回分类错误，不返回部分通过结果。

## 已知边界

`$schema` 只接受标准 2020-12 URI。自定义 metaschema 与 `$vocabulary` 在编译期以 `SL.UnsupportedDialect` 拒绝（错误串含具体 URI 与策略说明）：本引擎不能按 vocabulary 动态启用/停用关键字，静默忽略会产生假通过。官方 Test Suite 固定 commit `e2a68a15cd490750fd6b95e604e4054d7d978d1f`，最近实跑 405 组 / 1300 例 / 142 跳过 / 2 失败，剩余 2 例正是该设计性拒绝，因此不声称官方 suite 完整通过。

## 相关页面

- [/concepts/version-negotiation.md](../concepts/version-negotiation.md) — 协议双 era 与 dialect 的区别
- [/concepts/meta-and-filters.md](../concepts/meta-and-filters.md) — schema 在协议载荷中的位置
- [/build.md](../build.md) — 第三方固定源码与构建选项
- [/tests.md](../tests.md) — 本地回归用例与官方 suite 驱动
