# 注册一个工具

用 `McpServer::RegisterTool` 把一段本地函数挂成 MCP 工具：注册完成后客户端能在 `tools/list` 看到它、用 `tools/call` 调用它，并按你声明的 `inputSchema` 校验入参。签名与字段表只在 [/zh/reference/server](/zh/reference/server) 与 [/zh/reference/types](/zh/reference/types) 维护，本页只讲怎么把它们串起来。

## 前置条件

- 头文件：`mcp/server/McpServer.hpp`、`mcp/server/RequestContext.hpp`、`mcp/server/ServerOptions.hpp`、`mcp/McpTypes.hpp`、`mcp/JsonValue.hpp`、`mcp/Content.hpp`、`mcp/Implementation.hpp`、`mcp/McpError.hpp`；include 只给到 `-I include`，必须写全子目录路径。
- 传输：任意 `std::shared_ptr<ITransport>` 交给 `McpServer::Create`；本页片段用 `InMemoryTransport::CreatePair()` 的 `.server` 侧，生产服务换成 stdio/HTTP 服务端传输（构造见 [/zh/reference/transports](/zh/reference/transports)）。
- 协议时代：注册与调用路径不随时代裁剪，5 个版本同构（见「时代差异」）。
- 开关：无需额外 CMake 开关；`ServerOptions::max_tool_input_elements`、`enable_file_input` 等均为可选收紧项。

## 注册一个最小工具

`RegisterTool(name, options, fn)` 的第三个形参是 `std::function<CallToolResult(const RequestContext<CallToolRequestParams>&)>`，直接传无捕获 lambda 即可。`ToolOptions{}` 什么都不给时，SDK 仍会补一个 `{"type":"object","properties":{}}` 的 `input_schema`（`McpServerTool.hpp`），所以零约束的工具也合法。

<<< @/snippets/register-a-tool.cpp#minimal

工具名会按 `[A-Za-z0-9._-]` 校验且长度不超过 128（`IsValidToolName`）；`Create()` 返回后即可注册，握手完成后再注册会自动补发 `tools/list_changed`。

## 加描述、标题与输入 schema

`ToolOptions` 只有 `Description`、`Title`、`InputSchema` 三个返回 `*this` 的链式 setter，其余字段一律成员赋值；`ToolOptions::name` 不会被消费，真实名字取 `RegisterTool` 的第一个实参。schema 用自研 `JsonValue` 现场构造（`JsonValue::Object` / `JsonValue::Array`），也可以 `JsonValue::Parse(R"({ ... })")` 从字符串读入。

<<< @/snippets/register-a-tool.cpp#schema

`Title` 落到 `Tool::title`，`Description` 落到 `Tool::description`，两者都会原样出现在 `tools/list` 里；把人类可读名写成 `Title` 而不是塞进描述，客户端界面才会用它。

## 声明行为注解与图标

行为提示是四个 `std::optional<bool>` 成员：`read_only_hint`、`destructive`、`idempotent`、`open_world_hint`。只要有一个有值，SDK 才会装配出 `ToolAnnotations`；全为 `nullopt` 时 `Tool::annotations` 保持缺省。图标与元数据走成员字段 `icons`（`std::vector<Icon>`）与 `meta`，没有对应 setter。

<<< @/snippets/register-a-tool.cpp#hints

## 在 handler 内读请求上下文

handler 只拿到一个 `RequestContext<CallToolRequestParams>`：`Params()` 取 `name`/`arguments`/`meta`，`SessionId()` 取当次请求的协议会话，`Log()` 发日志通知，`LogLevel()`/`IsCancellationRequested()` 读请求级日志门槛与取消标志，`Server()` 是回连客户端做反向请求的入口。

<<< @/snippets/register-a-tool.cpp#context

::: warning
给 `Elicit`/`ElicitUrl` 传会话尾参时只能用 `RequestContext::SessionId()`；`McpServer::SessionId()` 是实例级随机标识，二者语义无关。成员表见 [/zh/reference/server](/zh/reference/server)。
:::

## 返回结构化输出

声明 `ToolOptions::output_schema`（或置 `use_structured_content = true`，SDK 会补 `{"type":"object"}`），handler 里同时写 `content` 与 `structured_content`。回结果前 SDK 用与入参相同的校验器核对 `structured_content`。

<<< @/snippets/register-a-tool.cpp#structured

::: danger
结构化输出不匹配走的是**协议错误**而非 `is_error`：对外消息为 `tool output does not match outputSchema: <原因>`，错误码 `InvalidParams`（`McpServer.cpp`）。也就是说 schema 声明了就必须兑现，宁缺毋滥。
:::

## 捕获注册期错误

注册期错误全部是 `McpError(McpErrorCode::InvalidParams)`，且只在 `RegisterTool` 调用点抛出——不会延迟到第一次调用。启动阶段建议把注册收敛成一个函数，由上层决定是崩溃还是跳过。

<<< @/snippets/register-a-tool.cpp#registration-errors

::: tip
需要自定义执行体（异步结算、按 `ProtocolTool()` 读元信息）时改走 `McpServerTool::Create` 生成 `shared_ptr` 再注册；对象路径没有空指针检查，传空 `shared_ptr` 会直接解引用崩溃。异步形态见 [/zh/reference/server](/zh/reference/server)。
:::

## 完整代码

服务端标识、指令、元素数上限、schema、注解、上下文与结构化输出一次组合：

<<< @/snippets/register-a-tool.cpp#full

## 验证

片段只做语法校验（`/Zs` 或 `-fsyntax-only`），不需要运行进程。在工作树根执行：

```bash
cmake -S . -B build/p2a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p2a --target mcp-doc-snippet-register-a-tool
```

期望输出：configure 阶段打印 `[mcp-docs] syntax-checking <N> snippet(s) with '/Zs'`（clang-cl）或 `'-fsyntax-only'`（Clang/GCC），构建阶段停在 `[N/N] Building CXX object ...` 且返回码为 0，无 `error:` 行；clang-cl 下每个片段还会附带一条与片段内容无关的 `argument unused during compilation: '-c'` 警告，可忽略。全部 snippet 一起校验用聚合目标 `--target mcp-docs-snippets`（CI 即跑这个）。

运行时自查：`tools/list` 应返回刚注册的名字与 `description`/`title`；未注册成功时先确认 `Create()` 用的传输与客户端连的是同一对管道。

## 时代差异

注册 API、`inputSchema`/`outputSchema`/`annotations` 的序列化不做时代门控，差异只在 `tools/list_changed` 的路由（modern 走订阅者、legacy 广播）与 `RequestContext::Log()` 需要请求自带 `_meta` 日志级别——详见 [/zh/learn/protocol-versions](/zh/learn/protocol-versions)。

## 常见错误

| 对外表现 | 成因 | 处置 |
|---|---|---|
| 注册抛 `invalid tool name: <name>` | 名字为空、超 128 或含 `[A-Za-z0-9._-]` 之外字符 | 改成 `snake_case`/`dotted.name` 形式后重试 |
| 注册抛 `tool already registered: <name>` | 同名重复注册，SDK 不支持覆盖 | 换名，或把可变内容做成同一工具的参数 |
| 注册抛 `x-mcp-header ...` | `input_schema` 里的 `x-mcp-header` 注解不在 `properties` 静态可达路径上、不唯一，或挂在非 string/integer/boolean 属性上 | 只给标量字符串/整数/布尔属性加该注解 |
| 调用回协议错误 `tool not found: <name>`（`InvalidParams`） | 客户端名字与注册名不一致（大小写、分隔符） | 以 `tools/list` 返回的 `name` 为准 |
| 调用回 `is_error` + `invalid arguments: ...` | 入参不合 `input_schema`，handler 根本没跑 | 见 [/zh/howto/validate-tool-input](/zh/howto/validate-tool-input) |
| `is_error` 文本里没有你抛的错误码 | handler 抛 `McpError` 时错误码与 `data` 被丢弃，只保留 `ErrorMessage()` | 需要传递细节就把它写进消息文本或返回 `is_error` 结果 |
| `is_error` 文本是泛化的 `Error executing tool <name>` | handler 抛了非 `McpError` 的异常，原始 `what()` 只进内部日志（默认日志全关） | 预期失败路径显式返回 `is_error` 结果 |
| 客户端拿到 `structuredContent` 但整次调用报错 | `structured_content` 不合 `output_schema` | 按「返回结构化输出」的 danger 提示收紧返回值构造 |

## 下一步

- 入参约束与三层防线：[/zh/howto/validate-tool-input](/zh/howto/validate-tool-input)
- 资源与提示的注册：[/zh/howto/serve-a-resource](/zh/howto/serve-a-resource)、[/zh/howto/serve-a-prompt](/zh/howto/serve-a-prompt)
- 长任务与进度：[/zh/howto/run-long-tasks](/zh/howto/run-long-tasks)、[/zh/howto/report-progress](/zh/howto/report-progress)
- 完整签名/字段表：[/zh/reference/server](/zh/reference/server)、[/zh/reference/types](/zh/reference/types)

## 本页对应源码

- `include/mcp/server/McpServer.hpp`
- `include/mcp/server/McpServerTool.hpp`
- `include/mcp/server/RequestContext.hpp`
- `include/mcp/server/ServerOptions.hpp`
- `include/mcp/McpTypes.hpp`
- `include/mcp/McpError.hpp`
- `include/mcp/Content.hpp`
- `include/mcp/JsonValue.hpp`
- `include/mcp/Implementation.hpp`
- `include/mcp/transport/InMemoryTransport.hpp`
- `src/server/McpServer.cpp`（注册校验与 `tools/call` 分支）
