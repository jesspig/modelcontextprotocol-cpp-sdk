---
type: Concept
title: MRTR 多轮请求-响应
description: 服务端发起的输入请求（inputRequests 键→{method,params} 映射）：elicitation form/URL 双模式、requestState HMAC 签发/校验、客户端按 method 分派自动补全与超时预算。
tags: [协议, mrtr, elicitation, 多轮, hmac]
timestamp: 2026-09-19T23:27:28+08:00
resource: include/mcp/McpTypes.hpp
---

# MRTR 多轮请求-响应

（Multi-Round Request-Response）服务端发起的输入请求以 `InputRequiredResult` 内嵌（`resultType == "input_required"`），取代旧式的服务端→客户端独立请求（standalone `sampling/createMessage`）模式。

## 数据模型

- MRTR 三件套：`InputRequest`（`method` + `params` 两项）、`InputRequests`（`std::map<std::string, InputRequest, std::less<>>`，键由服务端自定义）、`InputRequiredResult`（必填 `input_requests` + 可选 `request_state`）（[McpTypes.hpp:219-228](../../include/mcp/McpTypes.hpp)）
- `inputRequests` 的线上结构为「服务端键 → `{method, params}`」，序列化与反序列化均逐项校验 `method` 为非空字符串、`params` 为对象，畸形项直接抛 `InvalidParams`（[McpTypesResults.cpp:357](../../src/core/McpTypesResults.cpp)）
- `inputResponses` 的值是**裸结果**：elicitation 为 `{action, content}`、sampling 为 `{role, content, model, stopReason}`、roots 为 `{roots}`，均不带 `resultType`/`meta` 信封（`MakeInputResponseFromElicitResult`/`MakeInputResponseFromCreateMessageResult`/`MakeInputResponseFromListRootsResult`，[McpTypesParams.cpp:282](../../src/core/McpTypesParams.cpp)）
- `ElicitResultTyped<T>` 模板：`action` 默认 `"cancel"`，`is_accepted()` 判 `"accept"`
- `ElicitRequestParams` 双模式：`mode` 默认 `"form"`（wire 不写新字段，向后兼容）；`mode=="url"`（SEP-1034）额外携带 `url` + `elicitationId`；`ElicitResult` wire 键 `action` + `content`（原 `values` 已修正，对齐规范与官方服务器）

## 客户端（[/classes/mcp-client.md](../classes/mcp-client.md)）

- `SendRequestWithMrtr` 循环处理 `input_required`：显式配置 `input_required_config` 时 `auto_fulfill` 默认开（未配置则自动补全关闭），经 handler 填 `inputResponses` / `requestState`；遍历 `input_requests` 逐项按 `method` 分派（`elicitation/create` → `ElicitationHandler`、`sampling/createMessage` → `SamplingHandler`、`roots/list` → `RootsHandler`），回发 `inputResponses` 的键为服务端原键；对应 handler 未注册或 `method` 未知 → `MethodNotFound`（分派逻辑见 [McpClient.cpp:792](../../src/client/McpClient.cpp)）
- 预算：`max_rounds`（默认 10）超限 → `InternalError`；`max_total_timeout`（默认 0 = 不设总预算，只按轮限时 `round_timeout` 默认 600s）超限 → `RequestTimeout`。注意与 `ClientOptions::max_total_timeout`（会话引擎**每请求**总量封顶，见 [/classes/mcp-session-handler.md](../classes/mcp-session-handler.md)）是两个独立预算——后者接线自构造期 `SetMaxTotalTimeout`，MRTR 轮内每轮 `SendRequest` 同受其约束
- **state-only 退避**：`input_required` 无任何请求项（仅 `request_state`）时按 50ms 起每轮 ×2 增长、封顶 250ms 退避后重发（`kMrtrStateOnlyBackoffBase`/`kMrtrStateOnlyBackoffMax`，[McpClient.cpp:32](../../src/client/McpClient.cpp)，第 4 轮起不再增长），补全轮后计数清零
- **URL elicitation 顺序保证**（[McpClient.cpp:545-557](../../src/client/McpClient.cpp)）：处理 `mode=="url"` 请求时**先发 `notifications/elicitation/complete`、再 `p.set_value` 提交 elicit 响应**；通知发送异常被捕获仅记 Error 日志，不阻断响应提交

## 服务端（[/classes/mcp-server.md](../classes/mcp-server.md)）

- `IsMrtrSupported()`：非 stateless 且客户端 capabilities 含 `elicitation`
- `Elicit`：无 config 时超时 600s；结果 `code` 为负抛 McpError
- `ElicitUrl(url, message, timeout=600s)`：URL 模式 elicitation——自增 `elicitationId`，**先注册 `pending_url_elicitations_` 再发 `mode="url"` 请求**（complete 通知可能先于注册到达，旧顺序下会静默丢弃、promise 永不完成）；`SendRequest` 抛异常时清理条目并 rethrow；随后挂起等待客户端 `notifications/elicitation/complete` 唤醒，以 `action="accept"` 完成；两条超时消息区分 `(no response)`（请求无应答）与 `(no complete notification)`（响应已到但 complete 未到）；未知 `elicitationId` 的 complete 通知丢弃并记 Warning。客户端经 `SetUrlElicitationHandler` 消费（SDK 自动回 complete 通知）
- `ServerOptions::InputRequiredConfig`：`max_rounds{10}`、`round_timeout{600s}`、`legacy_shim{true}`
- **`CallToolResult::input_required`**（`std::optional<InputRequiredResult>`）：工具 handler 返回内嵌 MRTR 结果时，wire 写 `resultType: "input_required"` + `inputRequests` + `requestState`（[McpTypesResults.cpp](../../src/core/McpTypesResults.cpp)——该分支独有，普通结果仍写自身 `resultType`）
- **requestState HMAC 签发/校验**（[RequestState.hpp](../../include/mcp/server/RequestState.hpp)）：`ServerOptions::request_state_key` 配置后，`HandleCallTool` 对 `input_required` 结果自动 mint——payload（bare JSON，可空）注入 `iat` 经 `MintRequestState` 签名（`base64url(payload).hex(HMAC-SHA256)`）；下一轮请求携带的 requestState 在 handler 运行前经 `McpSessionHandler::SetRequestStateVerifier` 校验，失败回 `InvalidParams` + `data.reason="invalid_request_state"`；`request_state_ttl`（默认 0 不校验过期）超期拒绝；显式 `request_state_verifier` 优先于内置 HMAC 校验器

## 相关页面

- [/modules/protocol.md](../modules/protocol.md) — elicit 请求处理器注册
- [/classes/mcp-client.md](../classes/mcp-client.md) — MRTR 循环实现
- [/classes/mcp-server.md](../classes/mcp-server.md) — 服务端 elicitation
- [/docs/en/advanced/mrtr.md](../../docs/en/advanced/mrtr.md) — 在线文档
