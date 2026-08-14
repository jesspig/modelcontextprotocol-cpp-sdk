---
type: Concept
title: MRTR 多轮请求-响应
description: 服务端发起的 elicitation：InputRequiredResult 内嵌、客户端自动补全循环与超时预算。
tags: [协议, mrtr, elicitation, 多轮]
timestamp: 2026-08-14T00:57:41+08:00
resource: include/mcp/McpTypes.hpp
---

# MRTR 多轮请求-响应

（Multi-Round Request-Response）服务端发起的 elicitation 以 `InputRequiredResult` 内嵌（`resultType == "input_required"`），而非旧式的 `sampling/createMessage`。

## 数据模型

- MRTR 三件套：`InputRequestElicit`（必填 `message` + 可选 `requested_schema`）、`InputRequests`（可选 `confirm`/`elicit`）、`InputRequiredResult`（必填 `input_requests` + 可选 `request_state`）（[McpTypes.hpp:269-282](../../include/mcp/McpTypes.hpp)）
- `ElicitResultTyped<T>` 模板：`action` 默认 `"cancel"`，`is_accepted()` 判 `"accept"`

## 客户端（[/classes/mcp-client.md](../classes/mcp-client.md)）

- `SendRequestWithMrtr` 循环处理 `input_required`：显式配置 `input_required_config` 时 `auto_fulfill` 默认开（未配置则自动补全关闭），经 `elicitation_handler` 填 `inputResponses` / `requestState`
- 预算：`max_rounds`（默认 8）超限 → `InternalError`；`max_total_timeout`（默认 0 = 不设总预算，只按轮限时 `round_timeout` 默认 600s）超限 → `RequestTimeout`

## 服务端（[/classes/mcp-server.md](../classes/mcp-server.md)）

- `IsMrtrSupported()`：非 stateless 且客户端 capabilities 含 `elicitation`
- `Elicit`：无 config 时超时 600s；结果 `code` 为负抛 McpError
- `ServerOptions::InputRequiredConfig`：`max_rounds{8}`、`round_timeout{600s}`、`legacy_shim{true}`

## 相关页面

- [/modules/protocol.md](../modules/protocol.md) — elicit 请求处理器注册
- [/classes/mcp-client.md](../classes/mcp-client.md) — MRTR 循环实现
- [/classes/mcp-server.md](../classes/mcp-server.md) — 服务端 elicitation
- [/docs/en/advanced/mrtr.md](../../docs/en/advanced/mrtr.md) — 在线文档
