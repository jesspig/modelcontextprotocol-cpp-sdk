# 多轮往返补参（MRTR）

工具跑到一半发现还缺输入时，不阻塞服务端、也不直发 `elicitation/create`，而是回一个 `input_required` 结果，让客户端答完再带着 `inputResponses` 重跑同一个 handler。本页给出结果构造、`requestState` 签发与校验、续轮读回，以及它和 [/zh/howto/collect-user-input](/zh/howto/collect-user-input) 的分工；字段表见 [/zh/reference/types](/zh/reference/types)，双时代差异见 [/zh/learn/protocol-versions](/zh/learn/protocol-versions)。

## 前置条件

- 头文件（写全子目录路径）：`mcp/server/McpServer.hpp`、`mcp/server/RequestContext.hpp`、`mcp/server/ServerOptions.hpp`、`mcp/server/RequestState.hpp`、`mcp/client/McpClient.hpp`、`mcp/client/ClientOptions.hpp`、`mcp/Methods.hpp`、`mcp/McpTypes.hpp`、`mcp/JsonValue.hpp`、`mcp/Implementation.hpp`。
- 结果类型在 `include/mcp/McpTypes.hpp`：`InputRequest{method, params}`（`:245-248`）、`InputRequests`（键 → 请求，`:250`）、`InputRequiredResult{input_requests, request_state}`（`:252-255`）、`CallToolResult::input_required`（`:272`）；资源与提示的同名字段在 `:296`、`:308`。
- 入参侧的回读字段：`CallToolRequestParams::input_responses` 与 `::request_state`（`:138-144`），由 `RequestContext::Params()` 给出。
- 装配开关：服务端 `ServerOptions::request_state_key`/`request_state_ttl`/`request_state_verifier`/`input_required_config`（`include/mcp/server/ServerOptions.hpp:39-49`）；客户端 `ClientOptions::input_required_config`（默认存在，`auto_fulfill=true`、`max_rounds=10`、`round_timeout=600s`，`include/mcp/client/ClientOptions.hpp:55-63`）。

## 第 1 步：先选路，再写代码

| 场景 | 选择 | 理由 |
|---|---|---|
| 单个输入、拿到就能收尾、可接受服务端阻塞 | `Elicit`/`ElicitUrl` 直发 | 代码最短，legacy 时代唯一可用形态 |
| 一轮里要多个输入 | MRTR 手工装配多个键 | `Elicit`/`ElicitUrl` 都固定写在键 `"elicitation"` 上（`src/server/McpServer.cpp:97-98`），第二次会覆盖第一次 |
| 需要跨轮记住已答内容或阶段 | MRTR + `requestState` | 直发没有承载状态的载体 |
| stateless 部署 | 只能 MRTR | `McpServer::IsMrtrSupported()` 在 stateless 下返回 `false`（`src/server/McpServer.cpp:3351-3355`），而现代时代的直发本来就走 MRTR |
| 需要长时间等用户 | MRTR | 服务端不挂线程等待，时限由客户端 `round_timeout`/`max_rounds` 控制 |

表单的 `requestedSchema` 是普通 JSON Schema，本片段用一个单字段构造器拼出来（构造器与下面的请求装配分属两段）：

<<< @/snippets/request-extra-input.cpp#form-schema

## 第 2 步：构造 `input_required` 结果

<<< @/snippets/request-extra-input.cpp#input-request
<<< @/snippets/request-extra-input.cpp#input-required-result

`input_requests` 的键由你自己命名并必须在续轮保持一致（`src/client/McpClient.cpp:1021-1031` 原样回填同名键），所以键名要稳定、可读、别用随机数。序列化后 `resultType` 被写成 `"input_required"`、`inputRequests` 与 `requestState` 同层输出，此时 `result` 语义上的 `content`/`isError` 仍会一起序列化（`src/core/McpTypesResults.cpp:179-196`），因此这个分支里别再去填有意义的内容。

`ReadResourceResult` 与 `GetPromptResult` 有同样的 `input_required` 字段，`HandleReadResource`/`HandleGetPrompt` 也走同一套签名逻辑（`src/server/McpServer.cpp:2829-2838`、`:2921-2930`）。

## 第 3 步：续轮读回 `inputResponses`

<<< @/snippets/request-extra-input.cpp#read-back

值是**裸结果**：elicitation 为 `{action, content}`（`src/core/McpTypesParams.cpp:277-282`），sampling 为 `{role, content, model, stopReason}`，roots 为 `{roots}`，都没有 `resultType`/`meta` 信封。handler 每轮从头重跑，所以「读回 → 判断 → 决定还缺什么」必须放在函数最前面，任何副作用（写文件、下扣款单）要么推到最后一轮，要么自己保证幂等。

## 第 4 步：签发与校验 `requestState`

<<< @/snippets/request-extra-input.cpp#state-payload

handler 填的 `request_state` 只是**未签名 payload**（一段 JSON 文本）：配置了 `request_state_key` 时，服务端把这段 payload 解出来、补上 `iat`、再 HMAC-SHA256 签名后发给客户端（`src/server/McpServer.cpp:2403-2410`、`src/server/McpServer.cpp:410-421`）——所以「handler 自己签名」是错的，签名一次由服务端完成。工具里用 `detail::DecodeRequestStatePayload` 读回（`include/mcp/server/RequestState.hpp:83-98`），它在有无签名的两种情况下都能取回 payload。

<<< @/snippets/request-extra-input.cpp#server-signing
<<< @/snippets/request-extra-input.cpp#custom-verifier

`request_state_ttl` 控制 `iat` 的有效期，`0` 表示不检查过期（`include/mcp/server/RequestState.hpp:74-79`）；`request_state_verifier` 一旦提供就取代内置 HMAC 校验（`src/server/McpServer.cpp:686-695`）。校验发生在进入 handler 之前：不通过直接回 `InvalidParams`，`data.reason = "invalid_request_state"`（`src/protocol/McpSessionHandler.cpp:466-475`），`requestState` 非法时不会有任何工具副作用。

> [!danger]
> 既不配 `request_state_key` 也不配 `request_state_verifier` 时，`requestState` 全程不校验——它是客户端回传的自由字符串。凡是用它决定阶段、金额、权限的 handler，必须至少配一项。

## 第 5 步：注册工具，客户端续轮

<<< @/snippets/request-extra-input.cpp#full-tool
<<< @/snippets/request-extra-input.cpp#tool-registration

<<< @/snippets/request-extra-input.cpp#client-auto

默认配置就够：`auto_fulfill=true` 时客户端识别到 `resultType=="input_required"` 后逐键调用对应 handler 补全，再带着 `inputResponses` + `requestState` 重发 `tools/call`，最多 `max_rounds` 轮（`src/client/McpClient.cpp:1113-1131`）。缺对应 handler 时不补全，`input_required` 中间结果原样返回调用方（`:1000-1003`，仅 `form` 模式这一项参与存在性检查，`url` 模式合成 `decline`）。

<<< @/snippets/request-extra-input.cpp#manual-round

手动续轮（`input_required_config = std::nullopt` 或 `auto_fulfill=false`）时结果停在 `CallToolResult::input_required`，此时**不能用 `CallTool()` 发第二轮**：该重载只取 `name`/`arguments`/`meta`/进度回调，`RequestOptions::input_responses` 与 `::request_state` 不会被写进请求（`src/client/McpClient.cpp:1312-1356`）。可用路径是自己组装 `CallToolRequestParams`（含 `input_responses`/`request_state`）后走 `SendRequest(methods::kCallTool, SerializeCallToolRequestParams(params))`（`src/core/McpTypesParams.cpp:45-53`）。

## 完整代码

`docs/snippets/request-extra-input.cpp` 是可编译全文，区域依次为 `form-schema`、`input-request`、`read-back`、`state-payload`、`input-required-result`、`full-tool`、`server-signing`、`custom-verifier`、`tool-registration`、`client-auto`、`manual-round`。仓库内同主题的参考实现是 `examples/conformance/server/MrtrTools.cpp:123-295`（单键、多键、签名状态、多轮四种形态各一个工具）。

## 验证

片段只做语法校验（MSVC 前端 `/Zs`，其它 `-fsyntax-only`）：

```bash
cmake -S . -B build/p2c -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p2c --target mcp-doc-snippet-request-extra-input
```

实测输出（clang-cl 前端）：configure 末尾 `[mcp-docs] syntax-checking 10 snippet(s) with '/Zs'`，build 为 `[1/2] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-request-extra-input.dir\__\__\docs\snippets\request-extra-input.cpp.obj`，返回码 0、无 `error:` 行；`argument unused during compilation: '-c'` 是 `/Zs` 组合下的固有告警。

行为级验证需要测试目标：

```bash
ctest --preset debug -R 'McpServerTest.(MrtrToolInputRequiredElicitationRoundTrip|MrtrMintedRequestStateRoundTrip|MrtrForgedRequestStateRejectedWithReason)|McpClientTest.MrtrDefaultWithoutHandlersReturnsInputRequired'
```

应看到 4 条用例逐个 `Passed`，末行 `100% tests passed, 0 tests failed out of 4`：分别覆盖裸结果回读、服务端签名往返、伪造状态被拒（`data.reason`）、缺 handler 时原样返回。

## 时代差异

| 维度 | legacy（早于 2026-07-28） | modern（2026-07-28 及以后） |
|---|---|---|
| 手写 `input_required` | 可用：序列化与回读不做时代裁剪（`src/core/McpTypesResults.cpp:179-196`） | 可用，且是唯一能拿到用户输入的形态 |
| `Elicit`/`ElicitUrl` | 直发 `elicitation/create` 并受客户端能力门控 | 不直发，内部抛 `ElicitationInputRequired` 转成 `input_required`，键固定 `"elicitation"` |
| `elicitationId` | 出现在 URL 模式请求里 | 不序列化 |
| 服务端等待 | 阻塞在 future 上，时限 `round_timeout`（未配置 600s） | 不等待，轮次与时限都由客户端决定 |
| 状态承载 | 无（只能靠会话内阻塞续跑） | `requestState`，签名与有效期由服务端选项控制 |
| 重试闭环 | 不适用 | `tools/call` 重发时同时带 `inputResponses` 与 `requestState`，与任务续跑同构（`tasks/update` 里两者不得同带，`src/server/McpServer.cpp:1586-1591`） |

## 常见错误

| 对外表现 | 成因 | 处置 |
|---|---|---|
| 客户端每轮都问同一个问题，最终 `MRTR: exceeded max_rounds` | handler 没先读 `input_responses`/`request_state` 就重新发问 | 按第 3 步把读回放在最前面，用状态决定当前轮 |
| 续轮直接回 `-32602` 且 `data.reason="invalid_request_state"`，handler 没进 | 客户端回传的 `requestState` 被改动、过期（`request_state_ttl`），或你自己在 handler 里改了签名串 | 只把服务端发来的原串回传；需要放宽就调 `ttl` 或换 `request_state_verifier` |
| 配了 key 后签名字段变成一串 `base64url.hex`，`Find("round")` 取不到值 | 签名后的 `requestState` 不是裸 JSON | 用 `detail::DecodeRequestStatePayload` 读取，别直接 `JsonValue::Parse` |
| 未配 `request_state_key` 时状态被随意伪造 | 服务端不校验不签名 | 见第 4 步的 danger，二者至少配一个 |
| `auto_fulfill=false` 后手动发第二轮拿不到答案 | `CallTool()` 不透传 `RequestOptions::input_responses`/`request_state` | 用 `SendRequest` + `SerializeCallToolRequestParams`，见第 5 步 |
| 第二个输入请求把第一个的答案覆盖 | 多个 `InputRequest` 用了同一个键，或都走 `Elicit`（固定键） | 每个输入用独立键；`Elicit` 只适合单输入 |
| 想读 `content` 却拿到空数组 | `input_required` 分支仍序列化了 `content`/`isError`，但语义上此轮没有终局结果 | 该分支只填 `input_required`，内容留到最后一轮 |
| 现代时代以为会抛 `-32021` 而写了兜底 | 现代路径完全不做能力门控，能力缺失表现为续轮没人答 | 需要预判时用 `McpServer::IsMrtrSupported()`，并注意它按全局/单会话定位 |

## 下一步

- 单个输入的直发写法：[/zh/howto/collect-user-input](/zh/howto/collect-user-input)
- 工具注册与入参校验：[/zh/howto/register-a-tool](/zh/howto/register-a-tool)、[/zh/howto/validate-tool-input](/zh/howto/validate-tool-input)
- 任务型长流程与进度：[/zh/howto/run-long-tasks](/zh/howto/run-long-tasks)、[/zh/howto/report-progress](/zh/howto/report-progress)
- 错误码全表：[/zh/reference/errors](/zh/reference/errors)

## 本页对应源码

- `include/mcp/McpTypes.hpp`
- `include/mcp/server/RequestContext.hpp`
- `include/mcp/server/RequestState.hpp`
- `include/mcp/server/ServerOptions.hpp`
- `src/server/McpServer.cpp`
- `src/protocol/McpSessionHandler.cpp`
- `src/core/McpTypesParams.cpp`
- `src/core/McpTypesResults.cpp`
- `include/mcp/client/ClientOptions.hpp`
- `src/client/McpClient.cpp`
- `examples/conformance/server/MrtrTools.cpp`
- `docs/snippets/request-extra-input.cpp`
