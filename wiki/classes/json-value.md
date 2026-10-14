---
type: Class
title: JsonValue
description: 基于 std::variant 的 JSON 值类型，手写序列化 Dump()、显式栈迭代解析与迭代式析构/深拷贝。
tags: [json, variant, 序列化]
timestamp: 2026-10-14T15:08:03+08:00
resource: include/mcp/JsonValue.hpp
---

# JsonValue

`std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object>`（[JsonValue.hpp](../../include/mcp/JsonValue.hpp)）。`Object = map<string, JsonValue, less<>>`，`Array = vector<JsonValue>`。无 uint64/float 类型——整数只有 `int64_t`。

## 要点

- 默认构造为 null；`int` 隐式提升 `int64_t`；`object_tag`/`array_tag` 构造空容器
- `Dump(int indent = -1)` 手写序列化（[JsonValue.cpp](../../src/core/JsonValue.cpp)）：控制字符 `\uXXXX` 转义；double 经 `std::to_chars` 输出最短往返表示（**locale 无关**，MSVC/glibc≥11 或 `__cpp_lib_to_chars` 启用，否则回退 `snprintf` 的 `max_digits10`（17 位）格式），NaN/Inf 输出 `null`；无 `.eE` 时补 `.0`
- `Parse` 用自研解析器（[JsonParser.cpp](../../src/core/JsonParser.cpp)，`vector<Frame>` 显式栈迭代实现），失败抛 `McpError(ParseError)`（消息含 offset）；uint64 超 int64 范围抛 `DeserializeFailed`；1000 层嵌套数组期望成功（`JsonParserTest.DeepNesting1000Succeeds`）
- 迭代式析构与深拷贝：`JsonValue` 是递归值类型（variant 内含 `vector<JsonValue>` 与 `map<string, JsonValue>`），隐式析构/拷贝按层递归，深层嵌套在小栈加 Debug 大帧下溢出（CI `windows-2022-debug` 上 `DeepNesting1000Succeeds` SEGFAULT）。修复为显式工作栈的迭代式析构与深拷贝、显式默认移动（实现见 [JsonValue.hpp](../../include/mcp/JsonValue.hpp) 与 [JsonValue.cpp](../../src/core/JsonValue.cpp)），保持 1000 层成功语义
- 范围外：`DumpValue` 与 `operator==` 仍是递归实现但单层帧小、无深层用例触达，暂不在范围
- 8 个类型检查 + 6 组 Get* 访问器（GetArray/GetObject 各有 const 与非 const 重载，类型不匹配抛 `DeserializeFailed`）
- **Windows `GetObject` 宏隔离**（[JsonValue.hpp](../../include/mcp/JsonValue.hpp)）：文件顶部 `#ifdef GetObject` 内 `push_macro` + `undef`（记 `MCP_POP_GETOBJECT_MACRO`），文件尾部按该标记条件 `pop_macro` 恢复——`windows.h`（`wingdi.h`）的 `GetObject` 宏不再破坏 `JsonValue::GetObject` 成员声明；`McpParamAnnotations.hpp`（[McpParamAnnotations.hpp](../../include/mcp/detail/McpParamAnnotations.hpp)，标记 `MCP_POP_GETOBJECT_MACRO_ANNOT`）同理
- `operator[]`：非 const 缺失键**单次 `emplace(std::string(key), nullptr)` 插入 null**；const 缺失抛异常；`Find` 缺失返回 nullptr；`At` 缺失抛异常
- `Size()`：数组/对象/字符串返回各自长度；`Empty()`：null 也视为空
- 相等性 = variant 直接比较

## 相关页面

- [/modules/core.md](../modules/core.md) — 所属库
- [/concepts/meta-and-filters.md](../concepts/meta-and-filters.md) — _meta 中的 JsonValue 扩展
- [/AGENTS.md](../../AGENTS.md) — 公共头文件无外部 JSON 的约定
