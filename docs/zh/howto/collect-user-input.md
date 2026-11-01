# 向用户收集输入

服务端在一次工具调用里向用户要结构化输入（`form`）或把人支到站外页面完成确认（`url`），只需 `McpServer::Elicit` 与 `McpServer::ElicitUrl` 两个入口；真正的难点是会话定位与两个协议时代的不同行为。签名与字段表在 [/zh/reference/server](/zh/reference/server) 与 [/zh/reference/types](/zh/reference/types) 维护，机制原理见 [/zh/learn/protocol-versions](/zh/learn/protocol-versions) 与 [/zh/howto/request-extra-input](/zh/howto/request-extra-input)。

## 前置条件

- 头文件（include 只给到 `-I include`，必须写全子目录路径）：`mcp/server/McpServer.hpp`、`mcp/server/RequestContext.hpp`、`mcp/server/ServerOptions.hpp`、`mcp/client/McpClient.hpp`、`mcp/client/ClientOptions.hpp`、`mcp/McpTypes.hpp`、`mcp/McpError.hpp`、`mcp/JsonValue.hpp`、`mcp/Implementation.hpp`、`mcp/Capabilities.hpp`。
- 两个入口的声明在 `include/mcp/server/McpServer.hpp:78-83`：

| 方法 | 签名 | 等待时限 |
|---|---|---|
| `Elicit` | `std::future<ElicitResult> Elicit(const ElicitRequestParams&, std::string_view session_id = {})` | 没有超时参数：取 `ServerOptions::input_required_config->round_timeout`，未配置时用内部默认 600s（`src/server/McpServer.cpp:1173-1177`、`McpServer.cpp:48`） |
| `ElicitUrl` | `std::future<ElicitResult> ElicitUrl(const std::string& url, const std::string& message, std::chrono::seconds timeout = std::chrono::seconds(600), std::string_view session_id = {})` | 显式 `timeout`，看门狗到期以 `RequestTimeout`（`-32001`）结算 future（`src/server/McpServer.cpp:1246-1279`） |

- 客户端必须能回答 `elicitation/create`：`ElicitationHandler` 与 `UrlElicitationHandler` 均返回 `ElicitResult`（`include/mcp/client/ClientOptions.hpp:26-30`），且必须写进 `ClientOptions` 后再 `McpClient::Create`——上报的能力在构造函数里由 `DeriveClientCapabilities` 推导（`src/client/McpClient.cpp:360-390`、`:620-622`）。
- 工具 handler 内必须能取到 `ctx.SessionId()`（`include/mcp/server/RequestContext.hpp:62`）。

## 第 1 步：把会话 id 一路传下去

`initialize` 上报的能力与协商版本，只有在会话 id 为空时才写进服务端全局成员，其余情况一律落到按 `mcp-session-id` 建立的会话表（`src/server/McpServer.cpp:3194-3198`）。`Elicit`/`ElicitUrl` 用 `EffectiveSessionId(session_id)` 定位会话（`McpServer.cpp:1060-1068`），省略 `session_id` 时的回退顺序是：显式会话 → 全局能力已设 → 恰好只有一个非空会话时用该会话 → 其余情况回落到全局（此时读不到按会话上报的能力）。

> [!warning]
> 在 stateful Streamable HTTP 的并发会话下省略 `session_id`，legacy 时代会直接抛 `McpError(-32021 MissingRequiredClientCapability)`（`src/server/McpServer.cpp:1151-1167`）。工具 handler 内一律写成 `ctx.Server().Elicit(params, ctx.SessionId())`——仓库内的真实写法见 `examples/conformance/server/InteractionTools.cpp:111`。

## 第 2 步：装配客户端 handler

<<< @/snippets/collect-user-input.cpp#client-handlers

两个 handler 按 `mode` 分工：`elicitation_handler` 收 `form`，`url_elicitation_handler` 收 `url`。三态必须自己判定——`ElicitResult.action` 只能是 `accept`/`decline`/`cancel`，其它值让客户端抛 `HandlerError`（`src/client/McpClient.cpp:285-294`）；`url` 模式没装 handler 时客户端不报错，而是合成 `action="decline"`（`:296-305`）；`form` 模式没装 handler 时直接回 `MethodNotFound`（`:737-740`）。

> [!tip]
> `SetLegacyUrlElicitationHandler`（`include/mcp/client/McpClient.hpp:119-121`）标记为 deprecated：它把回调返回值写死成 `accept` 且拿不到 `content`，只保留兼容用途，新代码直接用返回 `ElicitResult` 的 `url_elicitation_handler`。

## 第 3 步：form 模式的收集

<<< @/snippets/collect-user-input.cpp#form-schema
<<< @/snippets/collect-user-input.cpp#form-tool-handler

`requested_schema` 是标准 JSON Schema 对象，`properties` 的每个键就是最终 `content` 的键；`form` 模式下客户端会把 schema 里的 `default` 回填进 `content`，但仅当 `accept`、且客户端能力声明里 `elicitation.form.applyDefaults == true`（`src/client/McpClient.cpp:239-258`）。

handler 的骨架是「先回读、再发问」：现代时代 `Elicit` 不直发请求，而是抛出内部信号 `ElicitationInputRequired`（`src/server/McpServer.cpp:1142-1148`），由 `HandleCallTool` 的三条路径转成 `input_required` 结果（`:2369`、`:2520`、`:2630`），客户端答完后**从头重跑同一个 handler**，答案在 `ctx.Params().input_responses` 里。把 `Elicit(...).get()` 当成"总能拿到答案"的老写法，在现代时代会每轮重新问一次，直到客户端 `max_rounds` 耗尽。

<<< @/snippets/collect-user-input.cpp#read-back

回读逻辑对两个时代同构：legacy 时代 `input_responses` 不存在，走发问分支；现代时代第二轮起命中回读分支，`input_responses["elicitation"]` 的值是裸结果 `{action, content}`（`src/core/McpTypesParams.cpp:277-282`）。

## 第 4 步：url 模式的收集

<<< @/snippets/collect-user-input.cpp#url-tool-handler

`ElicitUrl(url, message, timeout, session_id)` 只告诉用户去哪儿，不返回表单内容。三个细节：

- legacy 时代服务端自动生成 `elicitation_id`（`src/server/McpServer.cpp:1213-1220`），并在 `elicitationId` 字段随请求下发；现代时代该字段根本不序列化（`src/core/McpTypesParams.cpp:158-170`）。
- legacy 时代服务端的 future 由 `notifications/elicitation/complete` 结算，结算值固定是 `action="accept"` 且 `content` 为空（`src/server/McpServer.cpp:1464-1487`）——客户端 handler 返回的 `decline`/`cancel` 不会传到服务端。要拿到真实三态只能走现代时代 MRTR（`src/client/McpClient.cpp:1024-1027`）。
- 超时（`-32001`）与连接关闭都让 future 以异常结算，后者走 `AbandonPendingUrlElicitation`（`src/server/McpServer.cpp:1293-1304`），因此 `.get()` 必须放在 `try` 里。

## 第 5 步：注册工具并跑起来

<<< @/snippets/collect-user-input.cpp#server-options
<<< @/snippets/collect-user-input.cpp#server-registration

`input_required_config` 在服务端只有一个实际作用点：给 legacy 时代的 `Elicit` 提供等待时限；`max_rounds` 是客户端循环上界（`src/client/McpClient.cpp:1076-1134`），服务端不消费它（`src/server/McpServer.cpp:1173-1177`）。

<<< @/snippets/collect-user-input.cpp#end-to-end

## 完整代码

服务端标识与选项、两个工具的注册、客户端两个 handler、一次端到端调用，全部在上面五个步骤里按区域给出；可编译的完整文件是 `docs/snippets/collect-user-input.cpp`，各区域名依次为 `form-schema`、`read-back`、`outcome`、`form-tool-handler`、`url-tool-handler`、`server-options`、`server-registration`、`client-handlers`、`end-to-end`。

## 验证

片段只做语法校验（MSVC 前端 `/Zs`，其它 `-fsyntax-only`），在工作树根执行：

```bash
cmake -S . -B build/p2c -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p2c --target mcp-doc-snippet-collect-user-input
```

实测输出（clang-cl 前端，Windows）：configure 末尾 `[mcp-docs] syntax-checking 10 snippet(s) with '/Zs'`，build 输出

```text
[0/2] Re-checking globbed directories...
[1/2] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-collect-user-input.dir\__\__\docs\snippets\collect-user-input.cpp.obj
```

返回码 0、无 `error:` 行即通过；`clang-cl: warning: argument unused during compilation: '-c'` 是 `/Zs` 与 Ninja 组合下的固有告警，与本页代码无关。

行为级验证需要测试目标（`cmake --preset debug && cmake --build --preset debug`）：

```bash
ctest --preset debug -R 'McpServerTest.(ElicitationMissingCapability32021|ModernElicitRoutesThroughMrtrInputRoundTrip|ElicitUrlTimesOutWhenNoCompleteArrives|LegacyElicitResolvesCapabilitiesForSingleStatefulSession)'
```

应看到：4 条用例逐个 `Passed`，末行为 `100% tests passed out of 4`（存在失败时才会出现 `0 tests failed` 这一段）。这四条分别覆盖 `-32021` 门控、现代时代走 MRTR、URL 看门狗超时、单会话隐式回退（用例名可在 `tests/unit/McpServerTests.cpp` 里按名检索）。

## 时代差异

判据是协商版本字典序 `>= "2026-07-28"`（`IsModernProtocolVersion`），会话级生效。

| 维度 | legacy（早于 2026-07-28） | modern（2026-07-28 及以后） |
|---|---|---|
| 出站形态 | 服务端直发 `elicitation/create` 请求并阻塞等响应 | 不直发：抛 `ElicitationInputRequired`，`HandleCallTool` 转成 `resultType="input_required"`，由客户端经 MRTR 续轮履行 |
| 能力门控 | 缺 `elicitation.form`/`elicitation.url` 抛 `-32021`，`data.requiredCapabilities.elicitation` 指明缺哪个 | 完全不做能力门控，也不查会话能力 |
| `elicitationId` | 出现，`ElicitUrl` 自动生成 `elicit-<n>` | 不序列化，恒不出现 |
| URL 模式三态 | 服务端恒得 `accept`（无 `content`），`decline`/`cancel` 丢失 | 客户端 handler 的 action 原样进 `input_responses` |
| handler 生命周期 | 一次进入、一次返回，原地阻塞 | 同一 handler 每轮从头重跑，必须回读 `input_responses` |
| 等待时限 | `input_required_config->round_timeout`，未配置 600s | 由客户端 `round_timeout`/`max_rounds` 决定，服务端不等待 |

## 常见错误

| 对外表现 | 成因 | 处置 |
|---|---|---|
| legacy 会话上报 `-32021`，`data.requiredCapabilities.elicitation` 含 `form` | 客户端没注册 `elicitation_handler`，能力没被推导出来；或 `SetElicitationHandler()` 在 `Create` 之后调用 | 一律在 `ClientOptions` 里装配 handler（能力在构造时定稿，`src/client/McpClient.cpp:620-622`） |
| stateful HTTP 下同样的代码单机能跑、并发就报 `-32021` | 省略 `session_id` 且会话数不为 1，`EffectiveSessionId` 回落全局能力（未写入） | 传 `ctx.SessionId()`，见第 1 步 |
| 现代时代客户端一直重复问同一个问题，最后 `MRTR: exceeded max_rounds` | handler 没先回读 `input_responses` 就调 `Elicit`，每轮都重新发起 | 按第 3 步骨架先回读再发问 |
| 一次调用里问第二次，第一次的答案被覆盖 | `Elicit` 与 `ElicitUrl` 都用固定键 `"elicitation"`（`src/server/McpServer.cpp:97-98`） | 多输入改用 MRTR 手工装配不同键，见 [/zh/howto/request-extra-input](/zh/howto/request-extra-input) |
| 以为 catch 住了错误，现代时代却拿不到用户输入 | 用 `catch (const std::exception&)` 包 `.get()`，把 MRTR 信号当普通错误吞掉 | 只捕获 `McpError`；`examples/conformance/server/main.cpp:159-161` 把有状态 conformance 服务钉在 legacy 时代，那里 `InteractionTools.cpp` 的泛捕获写法因此成立，通用代码不能照抄 |
| `url` 模式下拿不到 `content` | 该模式只回 action，legacy 由 complete 通知固定为 `accept` | 需要内容就改用 `form`；URL 只承担「去站外完成一件事」 |
| 服务端把 `ElicitUrl` 当成永久等待 | 看门狗到期（默认 600s）以 `-32001` 结算异常 | 显式传更短的 `timeout` 并处理 `McpError` |

## 下一步

- 需要多个输入、跨轮携带状态：[/zh/howto/request-extra-input](/zh/howto/request-extra-input)
- 入参校验与工具注册：[/zh/howto/validate-tool-input](/zh/howto/validate-tool-input)、[/zh/howto/register-a-tool](/zh/howto/register-a-tool)
- 长时间任务的续跑与进度：[/zh/howto/run-long-tasks](/zh/howto/run-long-tasks)、[/zh/howto/report-progress](/zh/howto/report-progress)
- 错误码全表：[/zh/reference/errors](/zh/reference/errors)

## 本页对应源码

- `include/mcp/server/McpServer.hpp`
- `src/server/McpServer.cpp`
- `include/mcp/server/ServerOptions.hpp`
- `include/mcp/server/RequestContext.hpp`
- `include/mcp/client/ClientOptions.hpp`
- `include/mcp/client/McpClient.hpp`
- `src/client/McpClient.cpp`
- `src/core/McpTypesParams.cpp`
- `docs/snippets/collect-user-input.cpp`
