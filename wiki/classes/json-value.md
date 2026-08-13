---
type: Class
title: JsonValue
description: 基于 std::variant 的 JSON 值类型，手写序列化 Dump() 与 simdjson 解析。
tags: [json, variant, 序列化]
timestamp: 2026-08-13T12:36:14+08:00
resource: include/mcp/JsonValue.hpp
---

# JsonValue

`std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object>`（[JsonValue.hpp](../../include/mcp/JsonValue.hpp)）。`Object = map<string, JsonValue, less<>>`，`Array = vector<JsonValue>`。无 uint64/float 类型——整数只有 `int64_t`。

## 要点

- 默认构造为 null；`int` 隐式提升 `int64_t`；`object_tag`/`array_tag` 构造空容器
- `Dump(int indent = -1)` 手写序列化（[JsonValue.cpp](../../src/core/JsonValue.cpp)）：控制字符 `\uXXXX` 转义；double 用 `max_digits10`，NaN/Inf 输出 `null`；无 `.eE` 时补 `.0`
- `Parse` 用 simdjson DOM 递归转换，失败抛 `McpError(ParseError)`；uint64 超 int64 范围抛 `DeserializeFailed`；`ParseJsonString` 使用 **`static thread_local` simdjson `dom::parser`**（跨调用复用、线程隔离，避免重复分配）
- 8 个类型检查 + 7 组 Get* 访问器（类型不匹配抛 `DeserializeFailed`）
- `operator[]`：非 const 缺失键**单次 `emplace(std::string(key), nullptr)` 插入 null**；const 缺失抛异常；`Find` 缺失返回 nullptr；`At` 缺失抛异常
- `Size()`：数组/对象/字符串返回各自长度；`Empty()`：null 也视为空
- 相等性 = variant 直接比较

## 相关页面

- [/modules/core.md](../modules/core.md) — 所属库
- [/concepts/meta-and-filters.md](../concepts/meta-and-filters.md) — _meta 中的 JsonValue 扩展
- [/AGENTS.md](../../AGENTS.md) — 公共头文件无外部 JSON 的约定
