---
type: Concept
title: MRTR 多轮请求-响应
description: 服务端发起的输入请求（inputRequests 键→{method,params} 映射）：elicitation form/URL 双模式、URL 三态返回、requestState HMAC 签发/校验、客户端默认自动补全与超时预算。
tags: [协议, mrtr, elicitation, 多轮, hmac]
timestamp: 2026-10-29T22:02:08+08:00
resource: include/mcp/McpTypes.hpp
---

# MRTR 多轮请求-响应

（Multi-Round Request-Response）服务端发起的输入请求以 `InputRequiredResult` 内嵌（`resultType == "input_required"`），取代旧式的服务端→客户端独立请求（standalone `sampling/createMessage`）模式。

## 数据模型

- MRTR 三件套：`InputRequest`（`method` + `params` 两项）、`InputRequests`（`std::map<std::string, InputRequest, std::less<>>`，键由服务端自定义）、`InputRequiredResult`（必填 `input_requests` + 可选 `request_state`）（[McpTypes.hpp:219-228](../../include/mcp/McpTypes.hpp)）
- `inputRequests` 的线上结构为「服务端键 → `{method, params}`」，序列化与反序列化均逐项校验 `method` 为非空字符串、`params` 为对象，畸形项直接抛 `InvalidParams`（[McpTypesResults.cpp:357](../../src/core/McpTypesResults.cpp)）
- `inputResponses` 的值是**裸结果**：elicitation 为 `{action, content}`、sampling 为 `{role, content, model, stopReason}`、roots 为 `{roots}`，均不带 `resultType`/`meta` 信封（`MakeInputResponseFromElicitResult`/`MakeInputResponseFromCreateMessageResult`/`MakeInputResponseFromListRootsResult`，[McpTypesParams.cpp:282](../../src/core/McpTypesParams.cpp)）
- `ElicitResultTyped<T>` 模板：`action` 默认 `"cancel"`，`is_accepted()` 判 `"accept"`
- `ElicitRequestParams` 双模式：`mode` 默认 `"form"`（wire 不写新字段，向后兼容）；`mode=="url"`（SEP-1034）携带 `url`，legacy 另带 `elicitationId`，modern 由 MRTR 完成往返、客户端不再要求 `elicitationId`；`ElicitResult` wire 键 `action` + `content`（原 `values` 已修正，对齐规范与官方服务器）

## 客户端（[/classes/mcp-client.md](../classes/mcp-client.md)）

- `SendRequestWithMrtr` 循环处理 `input_required`：`input_required_config` 默认构造（`auto_fulfill=true`、`max_rounds=10`），默认自动补全；显式置 `std::nullopt` 或 `auto_fulfill=false`、以及任一请求项缺少对应 handler 时，不补全并把 `input_required` 结果原样返回调用方。补全时经 handler 填 `inputResponses` / `requestState`；遍历 `input_requests` 逐项按 `method` 分派——`elicitation/create` 是当前 MRTR 主路径（form 需 `ElicitationHandler`，url 未配置 handler 时默认 decline），`sampling/createMessage` 与 `roots/list` 仅保留 legacy/2025 兼容 handler（两 API 已因 SEP-2577 废弃，现代 2026 时代不应视为可用能力）；回发 `inputResponses` 的键为服务端原键；未知 `method` → `HandlerError`（分派逻辑见 [McpClient.cpp:936](../../src/client/McpClient.cpp)）
- 预算：`max_rounds`（默认 10）超限 → `InternalError`；每轮超时取 `round_timeout`（默认 600s）与调用方超时的较小值；`max_total_timeout`（默认 0 = 不设总预算）超限 → `RequestTimeout`。注意与 `ClientOptions::max_total_timeout`（会话引擎**每请求**总量封顶，见 [/classes/mcp-session-handler.md](../classes/mcp-session-handler.md)）是两个独立预算——后者接线自构造期 `SetMaxTotalTimeout`，MRTR 轮内每轮 `SendRequest` 同受其约束
- **state-only 退避**：`input_required` 无任何请求项（仅 `request_state`）时按 50ms 起每轮 ×2 增长、封顶 250ms 退避后重发（`kMrtrStateOnlyBackoffBase`/`kMrtrStateOnlyBackoffMax`，[McpClient.cpp:32](../../src/client/McpClient.cpp)，第 4 轮起不再增长），补全轮后计数清零
- **URL elicitation 三态与顺序**：`UrlElicitationHandler` 返回 `ElicitResult`，action 必须为 `accept`/`decline`/`cancel`，非法值抛 `HandlerError`，未配置 handler 默认 `decline`；旧 void 回调经 deprecated 的 `SetLegacyUrlElicitationHandler` 承接并固定 `accept`。legacy 时代处理 `mode=="url"` 请求时**先发 `notifications/elicitation/complete`、再 `p.set_value` 提交 elicit 响应**（通知发送异常仅记 Error 日志，不阻断响应提交）；modern 时代不发送 complete，改由 MRTR `inputResponses` 完成往返（[McpClient.cpp:666](../../src/client/McpClient.cpp)）

## 服务端（[/classes/mcp-server.md](../classes/mcp-server.md)）

- `IsMrtrSupported()`：非 stateless 且客户端 capabilities 含 `elicitation`
- `Elicit`：无 config 时超时 600s；结果 `code` 为负抛 McpError
- `ElicitUrl(url, message, timeout=600s)`：modern 时代直接抛 `ElicitationInputRequired`（`inputRequests["elicitation"]` + 可选 `requestState`），由 MRTR 往返完成并忽略 `timeout`；legacy 时代为 URL 模式 elicitation——自增 `elicitationId`，**先注册 `pending_url_elicitations_` 再发 `mode="url"` 请求**（complete 通知可能先于注册到达，旧顺序下会静默丢弃、promise 永不完成）；`SendRequest` 抛异常时清理条目并 rethrow；随后挂起等待客户端 `notifications/elicitation/complete` 唤醒，以客户端返回的 action 完成；两条超时消息区分 `(no response)`（请求无应答）与 `(no complete notification)`（响应已到但 complete 未到）；未知 `elicitationId` 的 complete 通知丢弃并记 Warning。客户端经 `SetUrlElicitationHandler` 消费（legacy SDK 自动回 complete 通知）
- `ServerOptions::InputRequiredConfig`：`max_rounds{10}`、`round_timeout{600s}`（无额外兼容垫片字段）
- **`CallToolResult::input_required`**（`std::optional<InputRequiredResult>`）：工具 handler 返回内嵌 MRTR 结果时，wire 写 `resultType: "input_required"` + `inputRequests` + `requestState`（[McpTypesResults.cpp](../../src/core/McpTypesResults.cpp)——该分支独有，普通结果仍写自身 `resultType`）
- **requestState HMAC 签发/校验**（[RequestState.hpp](../../include/mcp/server/RequestState.hpp)）：`ServerOptions::request_state_key` 配置后，`HandleCallTool` 对 `input_required` 结果自动 mint——payload（bare JSON，可空）注入 `iat` 经 `MintRequestState` 签名（`base64url(payload).hex(HMAC-SHA256)`）；下一轮请求携带的 requestState 在 handler 运行前经 `McpSessionHandler::SetRequestStateVerifier` 校验，失败回 `InvalidParams` + `data.reason="invalid_request_state"`；`request_state_ttl`（默认 0 不校验过期）超期拒绝；显式 `request_state_verifier` 优先于内置 HMAC 校验器

## 相关页面

- [/modules/protocol.md](../modules/protocol.md) — elicit 请求处理器注册
- [/classes/mcp-client.md](../classes/mcp-client.md) — MRTR 循环实现
- [/classes/mcp-server.md](../classes/mcp-server.md) — 服务端 elicitation
- [/docs/en/advanced/mrtr.md](../../docs/en/advanced/mrtr.md) — 在线文档
