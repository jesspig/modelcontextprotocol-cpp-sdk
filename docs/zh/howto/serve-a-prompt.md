# 提供提示

提示是一段可复用的消息模板，客户端按名字取回、再填参数。本篇覆盖参数声明、消息构造、缺失参数的报错形态，以及把补全接到提示参数上。

## 前置条件

- 已能让一个服务端实例跑起来，见 [快速上手](/zh/start/quickstart)。
- 手上有一个 `std::shared_ptr<ITransport>`；进程内自测用 `InMemoryTransport::CreatePair()` 取成对的 `.client` 与 `.server`。
- 签名与字段表只在参考层维护，本篇不重复：`RegisterPrompt`、`SetCompletionHandler` 见 [服务端门面参考](/zh/reference/server)，`PromptOptions`、`PromptArgument`、`GetPromptResult`、`CompleteRequestParams`、`CompleteResult` 见 [类型层参考](/zh/reference/types)。

## 第一步：声明提示与参数

<<< @/snippets/serve-a-prompt.cpp#declare-arguments

把 `prompt_options` 交给 `McpServer::RegisterPrompt("code-review", prompt_options, handler)` 即完成注册。

- 注册名只取 `RegisterPrompt` 的 `name` 参数；`PromptOptions` 里的 `name` 成员没有任何消费点，写了也不生效。
- `Arguments()` 给的列表原样进 `prompts/list` 的 `arguments`；`required` 是 `std::optional<bool>`，留空则该字段不上线（`src/core/McpTypesTools.cpp`）。
- 重名注册抛 `McpError`（`InvalidParams`，`prompt already registered: <name>`），空 handler 同样拒收（`src/server/McpServer.cpp`）。
- 注册成功即声明 `prompts` 能力并把 `list_changed` 置真；实例已在 `Run()` 时会自动补发一次 `list_changed`（`src/server/McpServer.cpp`）。

## 第二步：在 handler 里构造消息

<<< @/snippets/serve-a-prompt.cpp#read-argument
<<< @/snippets/serve-a-prompt.cpp#build-message

- handler 签名是 `(const std::string& name, const std::optional<JsonValue>& args)`：`name` 是命中的注册名，`args` 是客户端 `arguments` 对象的原样反序列化结果。客户端不给 `arguments` 时 `args` 为 `nullopt`。
- 上面那段 `ReadArgument` 是完整代码里的取参函数：`args` 为空、键不存在或值不是字符串都回空串——`args->Find(key)` 返回 `nullptr` 而不是抛错，判空必须由取参代码负责。
- `role` 直接写线、不做校验，按协议只能填 `user` 或 `assistant`。
- `content` 是 `ContentVariant`，五种变体都能用：文本、图片、音频、内嵌资源、资源链接。内嵌资源那条见 [提供资源](/zh/howto/serve-a-resource)。
- `messages` 永远会被序列化，空 vector 也照样发出 `"messages": []`——SDK 不拦，条数下限要你自己保证。
- 提示列表同样按 100 条一页分页，超出要带 `cursor` 翻页。

## 第三步：自己校验缺失参数

`prompts/get` 这条路径上 SDK **不做任何参数校验**：既不查 `required`，也不查类型，`args` 直接进 handler。这与 `tools/call` 会按 `input_schema` 走 JSON Schema 校验不同。缺参必须自己抛，否则对端只看到一条内容空泛的消息。

<<< @/snippets/serve-a-prompt.cpp#reject-missing-argument

抛出的 `McpError` 会原样编码成 JSON-RPC 错误响应，码与消息都保留；抛其它异常收敛为 `InternalError` + 固定文本 `internal error`（见 [错误码与错误模型](/zh/reference/errors)）。

::: tip
声明与校验要成对写：`required = true` 只是给客户端看的元数据，真正的拒绝只能发生在 handler 里。
:::

## 第四步：把补全接到提示参数上

补全能力由 `ServerOptions::declare_completions` 决定，默认 `false` 且不随注册派生；但 `completion/complete` 的请求处理器是无条件挂载的，所以没声明能力时方法仍然可达，只是对端不会去问。

<<< @/snippets/serve-a-prompt.cpp#declare-completions
<<< @/snippets/serve-a-prompt.cpp#completion-handler

- 一个实例只有一个全局补全回调，靠 `params.ref` 分流：`ref` 是未经解析的原始 `JsonValue`，形如 `{"type":"ref/prompt","name":"code-review"}`，`type`／`name` 都得自己读。
- `params.argument_name` 是正在输入的参数的键名，`params.argument_value` 是已输入的片段；`params.context_arguments` 带上同一请求里其它已确定的参数值（值必须是字符串，否则整条请求按 `InvalidParams` 拒收，`src/core/McpTypesParams.cpp`）。
- `CompleteResult::completion` 也是裸 `JsonValue`，SDK 不校验形状；惯例是 `values` / `total` / `hasMore` 三项，`total` 要在 `values` 被移走之前算好。
- 没调 `SetCompletionHandler` 时 `completion/complete` 回 `MethodNotFound`，消息 `No completion handler registered`（`src/server/McpServer.cpp`）；传空回调则注册期就抛 `InvalidParams`。

## 完整代码

<<< @/snippets/serve-a-prompt.cpp#full

## 验证

```bash
ctest --preset debug -R 'McpServerTest\.(RegisterPrompt|ListPrompts|Prompts|Complete|InitializeDeclares|ExplicitCapability)'
```

6 个用例应全部 `Passed`，末行 `100% tests passed, 0 tests failed out of 6`。关键断言：

| 用例 | 断言值 |
|---|---|
| `McpServerTest.ListPromptsIncludesArguments` | `prompts[0].arguments[0].name` 为 `city`，`required` 为 `true` |
| `McpServerTest.ExplicitCapabilityDeclarationDefaultsOff` | `completions` 能力无值 |
| `McpServerTest.InitializeDeclaresExplicitLoggingAndCompletions` | 置 `declare_completions` 后 `completions` 有值 |
| `McpServerTest.CompleteRequestCarriesContextArguments` | `context_arguments` 原样送达回调 |

## 时代差异

`prompts/list`、`prompts/get`、`completion/complete` 与 `prompts/list_changed` 通知双时代都可用；旧时代未握手的会话请求这三条会回 `InvalidRequest`（`Server not initialized`），现代时代直接放行。详见 [协议版本](/zh/learn/protocol-versions)。

## 常见错误

| 症状 | 成因 | 处置 |
|---|---|---|
| 客户端填了缺省参数仍拿到一条空消息 | SDK 不校验 `required`，缺失静默成空串 | 在 handler 开头判空并抛 `McpError(InvalidParams, ...)` |
| `prompts/list` 里没有 `arguments` | 没调 `PromptOptions::Arguments()`，或 `required` 留了空 `optional` | 显式给列表；需要 `required` 就赋 `true`／`false` |
| 改了 `PromptOptions::name` 却没效果 | 该成员无消费点 | 改名只改 `RegisterPrompt` 的第一个参数 |
| 对端问补全却得到 `-32601` | 没调 `SetCompletionHandler` | 注册全局回调并按 `params.ref` 分流 |
| 补全能力没出现在 `initialize` 结果里 | `declare_completions` 默认 `false` 且不派生 | 在 `ServerOptions` 里显式置真 |
| 请求未知提示名得到 `prompt not found: <name>` | 名字不在注册表 | 核对 `prompts/list` 的实际名字，注意连字符与下划线 |

## 下一步

- 提示里内嵌资源与资源链接：[提供资源](/zh/howto/serve-a-resource)
- 取不到参数时改问用户：[请求额外输入](/zh/howto/request-extra-input)
- 参数的声明与拒绝：[校验工具入参](/zh/howto/validate-tool-input)

## 本页对应源码

- `include/mcp/server/McpServer.hpp`
- `include/mcp/server/ServerOptions.hpp`（`declare_completions`）
- `include/mcp/McpTypes.hpp`（`PromptOptions`、`PromptArgument`、`Prompt`、`PromptMessage`、`GetPromptResult`、`CompleteRequestParams`、`CompleteResult`）
- `include/mcp/Content.hpp`（`ContentVariant` 的五种变体）
- `include/mcp/transport/InMemoryTransport.hpp`
- `src/server/McpServer.cpp`（`RegisterPrompt`、`HandleListPrompts`、`HandleGetPrompt`、`SetCompletionHandler`、`HandleComplete`、能力推导）
- `src/core/McpTypesTools.cpp`（`SerializePromptArgument`、`SerializePromptMessage`）
- `src/core/McpTypesParams.cpp`（`DeserializeCompleteRequestParams`）
