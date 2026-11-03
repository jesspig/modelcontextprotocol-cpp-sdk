# 校验工具入参

给工具加三层防线：先让 SDK 按 `input_schema` 把不合格参数拒在 handler 之外，再在 handler 内做语义与类型的自保，最后用 `ServerOptions::max_tool_input_elements` 设总量护栏。做完你得到的是「参数不合法时客户端立刻拿到可读的 `is_error`，而你的业务代码永远只见到合法形状」。字段与签名以 [/zh/reference/types](/zh/reference/types)、[/zh/reference/server](/zh/reference/server) 为准。

## 前置条件

- 头文件：`mcp/server/McpServer.hpp`、`mcp/server/RequestContext.hpp`、`mcp/server/ServerOptions.hpp`、`mcp/McpTypes.hpp`、`mcp/JsonValue.hpp`、`mcp/Content.hpp`、`mcp/McpError.hpp`、`mcp/transport/InMemoryTransport.hpp`（或生产传输）。
- 注册形态：`McpServer::RegisterTool` 的 lambda 或 `McpServerTool` 对象都可，校验发生在 SDK 侧，与形态无关。
- 协议时代：5 个版本一致生效；任务化路径同样先校验。
- 校验器（`src/detail/JsonSchemaValidator.hpp`）是 SDK 内部实现，不是公共 API：只能通过 `input_schema` 声明约束，不要 `#include` 它。

## 第一层：让 SDK 按 input_schema 拒参数

`HandleCallTool` 在把请求交给 handler 之前调 `ValidateToolCallArguments`，用 `Tool::input_schema` 校验 `arguments`（`McpServer.cpp`）；不通过就**不调用 handler**，直接回 `is_error` 结果，文本为 `invalid arguments: <原因>`。缺省 `arguments` 会以空对象参与校验，所以写了 `required` 就必须带字段；而 `McpServerTool` 补的默认 schema 含 `type: "object"`，非对象入参仍会被拒。

<<< @/snippets/validate-tool-input.cpp#builtin-validation

::: tip
`enum`、`minimum`、`maxLength`、`additionalProperties: false` 这类约束全部由 SDK 执行，成本为零；把它们写进 `description` 让模型「自觉遵守」是反模式。
:::

## 第二层：handler 内的类型防御

schema 管形状，不管语义：未知账户、跨字段关系、已被枚举但当前不可用的取值都要在 handler 里判。`CallToolRequestParams::arguments` 是 `std::optional<JsonValue>`，先判空再 `Find`；数值必须 `IsDouble()` 与 `IsInt()` 分开取。`examples/WeatherServer/main.cpp` 就是这个写法。

<<< @/snippets/validate-tool-input.cpp#handler-defense

::: warning
`JsonValue` 的取值函数一律抛异常（`McpError(McpErrorCode::DeserializeFailed, "JsonValue::GetString: expected string, got number")`），const 的 `operator[]` 遇到缺键抛 `JsonValue: key not found: '<key>'`——不会返回空值兜底。`Find()` 返回 `const JsonValue*`，未命中为 `nullptr`，判空顺序不能颠倒。
:::

## 第三层：限制参数元素数

`ServerOptions::max_tool_input_elements` 统计的是**一层**元素数：对象按键数、数组按长度、标量按 1 计（`McpServer.cpp`）。超限时回 `is_error` + `tool arguments exceed maximum input element count`，同样不进 handler。

<<< @/snippets/validate-tool-input.cpp#element-limit

代价与边界：它不是深度上限，也不是字节上限，嵌套很深的大对象照样放行；它只省掉一次解析后的遍历，别指望它替代 schema，两者要一起用。

## 校验器支持的 JSON Schema 子集

| 类别 | 支持的关键字 | 备注 |
|---|---|---|
| 类型与常量 | `type`、`const`、`enum` | `type` 可为字符串或数组；`integer` 接受小数部分为 0 的 number |
| 逻辑组合 | `allOf`、`anyOf`、`oneOf`、`not`、`if`/`then`/`else` | `oneOf` 要求恰好匹配一个 |
| 对象 | `required`、`minProperties`、`maxProperties`、`properties`、`patternProperties`、`additionalProperties`、`propertyNames`、`dependentRequired`、`dependentSchemas`、`dependencies` | 错误文本会带属性路径，如 `property 'city': type mismatch` |
| 数组 | `items`（单 schema 或数组形式配 `additionalItems`）、`prefixItems`、`contains` 配 `minContains`/`maxContains`、`uniqueItems`、`minItems`、`maxItems` | 错误文本形如 `item 0: ...` |
| 字符串 | `minLength`、`maxLength`、`pattern` | 长度按 UTF-8 码点计数；`pattern` 走 `std::regex` 的 ECMAScript 方言且是**非锚定搜索** |
| 数值 | `minimum`、`maximum`、`exclusiveMinimum`、`exclusiveMaximum`、`multipleOf` | 兼容 draft-04 的布尔形态 `exclusiveMinimum` |
| 引用 | `$ref`、`$dynamicRef`、`$recursiveRef` | 只接受本地 `#`，可用 JSON Pointer 或 `$anchor`；`$defs` 本身只是被指针指到的容器 |

未被识别的关键字一律**静默忽略**（fail-open）：`format`、`contentEncoding`/`contentMediaType`、`unevaluatedProperties`/`unevaluatedItems`、`deprecated` 都不参与判定。布尔 schema 合法：`true` 全通过，`false` 报 `boolean schema false`。两条硬限制：递归深度上限 32（`kMaxJsonSchemaDepth`），超出报 `schema depth exceeded`；非 `#` 开头的引用直接判失败，报 `remote $ref not dereferenced '<uri>'`。递归复用请写本地引用：

<<< @/snippets/validate-tool-input.cpp#local-ref

::: danger
三个坑：`pattern` 非锚定，写 `^[a-z]+` 会放过尾随垃圾，要自己写全 `^...$`；正则编译失败时该值被直接放行，别用查表式正则兜安全；循环 `$ref` 不回 `is_error` 而是抛 `McpError(InvalidParams)`，此时响应根本不会发出，只能等客户端超时（`src/detail/JsonSchemaValidator.hpp`）。
:::

## 区分 is_error 与协议错误

| 触发者 | 对外形态 | 客户端可见信息 |
|---|---|---|
| SDK 内置 schema 校验 | 工具结果 `is_error` | `invalid arguments: <原因>` |
| 元素数超 `max_tool_input_elements` | 工具结果 `is_error` | `tool arguments exceed maximum input element count` |
| handler 主动返回失败 | 工具结果 `is_error` | 你写的 `content` 文本 |
| handler 抛 `McpError` | 工具结果 `is_error` | 仅 `ErrorMessage()`，错误码与 `data` 被丢弃（`McpServer.cpp`） |
| handler 抛其它异常 | 工具结果 `is_error` | 泛化为 `Error executing tool <name>`，原始 `what()` 只进内部日志 |
| 工具名未注册 | JSON-RPC 协议错误 `InvalidParams` | `tool not found: <name>` |
| 任务化路径的 handler 异常 | 任务失败而非 `is_error` | `McpError` 保留 code/message/data，其它异常归 `InternalError` |

<<< @/snippets/validate-tool-input.cpp#failure-modes

::: tip
可预期的业务失败走 `is_error`（模型能读到文本并重试）；入站请求本身不合法或属于编程错误才让它抛。别指望错误码能传到对端。
:::

## 完整代码

schema 约束 + handler 自保 + 元素数上限的组合：

<<< @/snippets/validate-tool-input.cpp#full

## 验证

```bash
cmake -S . -B build/p2a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p2a --target mcp-doc-snippet-validate-tool-input
```

期望输出：configure 阶段 `[mcp-docs] syntax-checking <N> snippet(s) with '/Zs'`（clang-cl）或 `'-fsyntax-only'`（Clang/GCC），构建阶段 `[N/N] Building CXX object ...` 且退出码 0、无 `error:` 行（clang-cl 另附一条与片段内容无关的 `argument unused during compilation: '-c'` 警告）。行为层面，校验器本身有直接用例：`ctest --preset debug -R 'McpTypesTest.JsonSchemaValidator'`；注册期错误用 `ctest --preset debug -R 'McpServerTest.RegisterToolRejectsInvalidName'` 与 `ctest --preset debug -R 'McpServerTest.DuplicateRegistrationThrowsInvalidParams'`。注意 `invalid arguments:` 与元素数超限这两条分支目前只有实现路径，仓库里没有断言其文本的用例，改动 `HandleCallTool` 时需自行验证。

## 时代差异

入参校验位置与 `is_error` 语义在两个协议时代完全一致，唯一分岔是任务化与 MRTR 结果的承载方式（异常在任务路径保留错误码）——详见 [/zh/learn/protocol-versions](/zh/learn/protocol-versions)。

## 常见错误

| 现象 | 成因 | 处置 |
|---|---|---|
| 每次调用都 `invalid arguments: missing required property 'x'` | 客户端没传该字段，而 schema 写了 `required` | 改 schema 把字段设为可选，或在 `description`/`title` 里说明必填 |
| `invalid arguments: property 'x': boolean schema false` | `additionalProperties: false` 拒掉了多余键 | 客户端侧收敛参数，或放宽为 `additionalProperties: { "type": "string" }` |
| 明明写了 `format: date` 却什么也没校验 | 校验器不支持 `format` | 用 `pattern` 加全锚定正则表达同样的约束 |
| `invalid arguments: unresolvable $ref` | 本地 JSON Pointer 指向 schema 里不存在的路径（例如写 `#/definitions/x` 而 schema 只有 `$defs`） | 公共子 schema 放 `$defs`，指针写 `#/$defs/<name>`；`$ref` 只接受 `#` 开头的本地引用 |
| 调用完全没有响应，最后超时 | schema 内存在循环 `$ref` | 拆成 `items`/`prefixItems` 的有限展开，别自引用 |
| 调用回 `is_error`，文本是 `JsonValue: key not found: 'x'` 或 `JsonValue::GetString: expected string, got number` | 直接用 `operator[]` 加 `GetString()`，没先判存在与类型 | 按「第二层」改成 `Find()` + `IsXxx()` 再取值；异常虽也会被转成 `is_error`，但文本不受你控制 |
| 参数体积没被限住 | 只设了 `max_tool_input_elements`，而它只数一层 | 在 schema 里对具体数组写 `maxItems`、字符串写 `maxLength` |
| 想给客户端回 `-32602` 却只拿到文本 | handler 抛 `McpError` 后错误码被丢弃 | 把细节写进 message，或返回带结构化文本的 `is_error` 结果 |

## 下一步

- 工具注册全流程：[/zh/howto/register-a-tool](/zh/howto/register-a-tool)
- 需要向用户补问参数：[/zh/howto/collect-user-input](/zh/howto/collect-user-input)、[/zh/howto/request-extra-input](/zh/howto/request-extra-input)
- 长任务与失败上报：[/zh/howto/run-long-tasks](/zh/howto/run-long-tasks)
- 错误码全表：[/zh/reference/errors](/zh/reference/errors)；类型字段表：[/zh/reference/types](/zh/reference/types)

## 本页对应源码

- `include/mcp/server/McpServer.hpp`
- `include/mcp/server/RequestContext.hpp`
- `include/mcp/server/ServerOptions.hpp`
- `include/mcp/server/McpServerTool.hpp`
- `include/mcp/McpTypes.hpp`
- `include/mcp/JsonValue.hpp`
- `include/mcp/McpError.hpp`
- `src/detail/JsonSchemaValidator.hpp`（内部校验器，能力边界的唯一事实来源）
- `src/server/McpServer.cpp`（`ValidateToolCallArguments` 与 `tools/call` 分支）
- `examples/WeatherServer/main.cpp`（handler 内类型防御的样板）
