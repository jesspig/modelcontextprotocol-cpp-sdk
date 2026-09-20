---
type: Concept
title: URI 模板匹配
description: 基于 RFC 6570 子集的资源模板解析、展开与实例 URI 匹配，并接入服务端资源注册和读取路由。
tags: [协议, uri, rfc6570, 资源]
timestamp: 2026-09-20T03:14:18+08:00
resource: src/detail/UriTemplate.hpp
---

# URI 模板匹配

`mcp::detail::UriTemplate` 是 header-only 的 RFC 6570 子集实现，提供 `Parse`、`Expand`、`Match` 和 `Pattern`。`Parse` 返回 `std::optional<UriTemplate>`，失败原因通过 `std::string& error` 返回，不直接抛异常。

## 语法与限制

- 支持 simple、reserved、fragment、label、path、path-style、form、form-cont 八类算子；变量名使用实现允许的字符集。
- 模板长度上限为 2048 字节，变量数上限为 32，待匹配 URI 长度上限为 8192 字节。
- 不支持 `{var:N}` 前缀修饰符；相邻且没有字面量分隔的变量、多个多段变量会在解析时拒绝。
- `Match` 返回变量名到 pct-decoded 值的映射；多段变量按贪婪方向匹配。`Expand` 未定义变量时遵循当前算子的省略语义。
- `{?x,y}` / `{;x,y}` 中部分变量缺失时，`Expand` 可以省略该变量，但当前匹配部分仍要求固定键名前缀，因此这类展开结果可能无法被 `Match` 反向匹配。

## 服务端接入

`McpServer::RegisterResourceTemplate` 注册前解析模板；非法模板抛 `McpError(InvalidParams)`，消息包含模板和解析原因。`resources/read` 先做静态资源精确匹配，再按注册顺序匹配资源模板，静态资源优先；模板命中后调用 `template_handler(uri, variables)`，变量值已经 pct 解码。

模板和静态资源都未命中时仍返回 `InvalidParams`。资源读取命中后继续应用 `resources/read` 的 cache hint。

## 验证

`mcp-uri-template-tests` 当前注册 21 个用例、133 个断言，覆盖解析拒绝、算子展开、pct 解码、贪婪匹配、长度限制和 `Expand`/`Match` 往返。

## 相关页面

- [/classes/mcp-server.md](../classes/mcp-server.md) — 资源模板注册与路由
- [/modules/server.md](../modules/server.md) — 服务端注册 API
- [/modules/core.md](../modules/core.md) — 内部 detail 头
- [/tests.md](../tests.md) — URI 模板测试目标
