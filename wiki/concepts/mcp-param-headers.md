---
type: Concept
title: x-mcp-header 参数头注解
description: Streamable HTTP 按工具 inputSchema 的 x-mcp-header 注解镜像并校验 Mcp-Param-* 请求头与 tools/call 参数。
tags: [http, streamable, sep2243, headers, tools]
timestamp: 2026-09-20T03:14:18+08:00
resource: include/mcp/detail/McpParamAnnotations.hpp
---

# x-mcp-header 参数头注解

Streamable HTTP 的参数头功能由 `McpParamAnnotations.hpp` 与 `McpParamHeaders.hpp` 共同实现。它只处理工具 `inputSchema` 中明确声明的参数，不再无差别镜像 `params` 顶层标量。

## 注解约束

- 注解必须位于从 schema 根通过 `properties` 静态可达的属性上；`$ref`、`items`、组合 schema 等路径上的注解会被拒绝。
- 注解值必须是非空、大小写不重复的合法 HTTP token；属性类型只允许 `string`、`integer` 或 `boolean`，`number`、数组和对象不允许。
- 客户端在 `tools/list` 响应中解析注解并建立工具名到属性路径的缓存；非法注解的工具从该响应的工具数组中剔除并记 Warning。

## 客户端镜像

发出 `tools/call` 时，客户端根据工具名和 `arguments` 中的属性路径生成 `Mcp-Param-<注解名>`。缺少或为 null 的参数不生成头；字符串、整数和布尔值分别按原值、十进制文本、`true`/`false` 编码。

首尾空白、不可见或非 ASCII 可打印字符，以及本身会形成 `=?base64?...?=` 哨兵的字符串，使用带 padding 的 RFC 4648 标准 Base64 包装；该编码不同于 PKCE/requestState 使用的无 padding base64url。未注解参数不会进入请求头。

服务端返回的结果 `_meta.x-mcp-header` 对象会被镜像为 `Mcp-Param-*` 响应头。

## 服务端校验

`StreamableHttpServerOptions::resolve_param_annotations` 可为当前 `tools/call` 解析注解。启用后，服务端检查每个有值参数对应的头是否存在、可解码且与请求体值一致；参数缺失但头存在、缺头、编码非法或值不一致都返回 `HeaderMismatch`（-32020）。未配置解析器或非 `tools/call` 请求不执行逐参数校验。

## 相关页面

- [/transports/streamable-http.md](../transports/streamable-http.md) — HTTP 双端行为
- [/modules/http.md](../modules/http.md) — mcp-http 组成
- [/classes/mcp-server.md](../classes/mcp-server.md) — 服务端工具 schema 解析
- [/tests.md](../tests.md) — 参数头契约与校验测试
