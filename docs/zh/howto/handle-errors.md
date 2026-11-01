# 处理错误

同一个「出错」在 SDK 里有三条完全不同的出口：抛 `McpError`、回 `is_error` 的工具结果、以及 `RequestOutcome` 里的 `ErrorData`；再叠上协议时代的错误码重映射。这一页给你判定顺序与重试策略。取消与超时的细节在 [/zh/howto/cancel-a-request](/zh/howto/cancel-a-request)。

## 前置条件

- 头文件：`mcp/McpError.hpp`、`mcp/ErrorCodes.hpp`、`mcp/McpTypes.hpp`、`mcp/protocol/IncomingRequestMeta.hpp`、`mcp/client/McpClient.hpp`、`mcp/client/RequestHandle.hpp`、`mcp/server/RequestContext.hpp`；include 必须写全子目录路径。
- 判型要 `<variant>`：`RequestOutcome` 是 `std::variant<JsonValue, ErrorData>`（`include/mcp/protocol/IncomingRequestMeta.hpp:29`）。
- 按语义分组判错要 `<system_error>`（公共头已带）：`McpErrorCode` 注册为 `std::error_code` 枚举（`include/mcp/ErrorCodes.hpp:32-39`）。
- 需要先理解一层分工：工具调用的失败走 `is_error` 结果，协议方法的失败走错误帧——两者判定方式不同，本页逐条给出。

## 先定判定顺序

| 你在用哪层 API | 出错形态 | 判定代码 |
|---|---|---|
| 便捷方法（`CallTool`、`ListTools`、`ReadResource`、`GetPrompt`……） | **抛 `McpError`** | `catch (const McpError&)` + `Code()` |
| `SendRequestRaw`／四参 `SendRequest` | future 正常 resolve，值是序列化后的错误对象 | 查 `code` 是否为负数 |
| `SendRequest(method, params, RequestOptions)` | `RequestOutcome = std::variant<JsonValue, ErrorData>` | `std::get_if<ErrorData>` |
| 工具 handler（服务端） | `is_error = true` 的 `CallToolResult`，或抛异常后由 SDK 兜住 | 看 `is_error` 与 `content` |

`McpError` 继承 `std::runtime_error`，带 `Code()`、`ErrorMessage()`、`Data()`（`include/mcp/McpError.hpp:13-35`）；`Data()` 是 `std::optional<JsonValue>`，装协议错误的 `data` 字段。

## 错误码全表与它们的语义分组

值定义在 `include/mcp/ErrorCodes.hpp:8-30`（参考全表见 [/zh/reference/errors](/zh/reference/errors)）：

| 码 | 名称 | 典型来源 |
|---|---|---|
| `-32700` | `ParseError` | JSON 解析失败 |
| `-32600` | `InvalidRequest` | 非法请求帧 |
| `-32601` | `MethodNotFound` | 未注册方法或扩展开关关闭 |
| `-32602` | `InvalidParams` | 参数校验、注册期冲突、结构化输出不符 |
| `-32603` | `InternalError` | 未知异常兜底、MRTR 轮数用尽 |
| `-32020` | `HeaderMismatch` | `Mcp-Param-*` 与 body 不一致 |
| `-32021` | `MissingRequiredClientCapability` | 对端未声明所需能力 |
| `-32022` | `UnsupportedProtocolVersion` | 版本协商被拒，`data.supported` 带候选 |
| `-32042` | `UrlElicitationRequired` | 仅声明与透传白名单，**SDK 内无抛出点** |
| `-32000` | `ConnectionClosed` | 连接关闭结算在途请求、HTTP 401/403 body |
| `-32001` | `RequestTimeout` | 本地超时 |
| `-32002` | `ResourceNotFound` | **SDK 内无抛出点**，只可能来自对端 |
| `-32800` | `RequestCancelled` | 显式取消 |
| `-32003` / `-32004` | `ConnectionRefused` / `TlsHandshakeFailed` | 传输层建连失败 |
| `-32005` | `ProtocolViolation` | 时代不允许的调用（如 modern 下 `logging/setLevel`） |
| `-32006` | `TaskNotFound` | **SDK 内无抛出点** |
| `-32007` | `HandlerError` | handler 抛出的非 MCP 错误、未知 input request method |
| `-32008` | `DeserializeFailed` | 反序列化失败 |
| `-32009` | `SessionExpired` | 未知/过期会话 |

`McpErrorCode` 还是 `std::error_code` 枚举（`include/mcp/ErrorCodes.hpp:32-39`），因此可以按语义分组判，而不是逐个码硬对（`include/mcp/ErrorCodes.hpp:71-91`）：

<<< @/snippets/handle-errors.cpp#classify-outcome

`connection_aborted` 归 `ConnectionClosed`/`RequestTimeout`/`ConnectionRefused`/`TlsHandshakeFailed`/`SessionExpired`；`io_error` 归 `InternalError`/`DeserializeFailed`/`ProtocolViolation`/`HandlerError`；`TaskNotFound` 是 `no_such_process`，`ResourceNotFound` 是 `no_such_file_or_directory`。

## 时代重映射只发生在出站

`-32001`、`-32003`、`-32004` 在 modern 时代经 wire 编码时被降级为 `-32603`（`src/protocol/WireCodec.cpp:271-283`），legacy 原样透传（`:158-160`）。由此得两条必然后果：

- 客户端**本地**产生的超时仍是 `-32001`，与时代无关；对端发来的同类错误在 modern 下只会以 `-32603` 出现。
- 想在 modern 下区分「内部错误」与「超时/拒绝连接/TLS 失败」，只能靠 `message` 文本或 `data`，不能靠码。

## 工具层的三种失败，结果完全不同

<<< @/snippets/handle-errors.cpp#tool-error-surface

1. **返回 `is_error = true`**：协议层成功，模型侧看到失败文本，可继续对话。入参不符合 `input_schema` 时 SDK 就是这么做的，消息以 `invalid arguments: ` 开头（`src/server/McpServer.cpp:553-559`、`:2359`、`:2507`、`:2616`）。
2. **抛 `McpError`**：`Code()` **被丢弃**，只有 `message` 进入工具结果的错误文本（`src/server/McpServer.cpp:2633-2643`）——想在工具里传码是传不出去的。
3. **抛其它异常**：文本被泛化成 `Error executing tool <name>`，你的异常信息不回给对端，只在服务端日志里（同上）。

::: warning 结构化输出不符不是 `is_error`
`structured_content` 不合 `output_schema` 走协议错误 `InvalidParams`，对外消息 `tool output does not match outputSchema: <原因>`（`src/server/McpServer.cpp:2673-2681`）。声明了 schema 就必须兑现，宁缺毋滥。
:::

协议方法层（资源、提示词、补全等）则**保留错误码**：`McpError` 的 `Code()` 与 `message` 原样进错误帧（`src/protocol/McpSessionHandler.cpp:576-586`），非 `McpError` 的异常一律 `InternalError` + `"internal error"`，真实原因只进服务端日志（`:587-614`）。

## 注册期错误：启动就炸，别等第一次调用

所有注册期校验都在 `RegisterTool`/`RegisterResource`/`RegisterPrompt` 调用点抛 `McpError(InvalidParams)`：重名、非法工具名、非法资源模板、空 handler 都属此类（`src/server/McpServer.cpp:93`、`:287`、`:329`）。把注册收敛成一个函数、由上层决定崩溃还是跳过：

<<< @/snippets/handle-errors.cpp#registration-errors

## 读 `data` 字段

`-32022` 的候选版本列表与请求态校验失败都靠 `data` 传结构化原因：

<<< @/snippets/handle-errors.cpp#version-rejection

`data.supported` 的键名固定为 `"supported"`（`src/protocol/McpSessionHandler.cpp:35`）；`requestState` 校验失败回 `InvalidParams` + `data.reason = "invalid_request_state"`。typed 通道里同样能取到：`ErrorData::data`（`include/mcp/protocol/IncomingRequestMeta.hpp`）。

## 重试策略：按码分派，不按 message

<<< @/snippets/handle-errors.cpp#retry-policy

- `SessionExpired` → 重建会话再试一次；客户端在 MRTR 流程里也自带一次「重新 initialize 再试」（`src/client/McpClient.cpp:1057-1066`），别在它之上叠无限循环。
- `RequestTimeout` → **只有幂等操作**才自动重试；写型工具交给调用方决定。
- `UnsupportedProtocolVersion` → 不要重试，读 `data.supported` 决定是否降级；stdio 类传输的自动回退规则见 [/zh/learn/protocol-versions](/zh/learn/protocol-versions)。
- `ConnectionClosed` / `ConnectionRefused` / `TlsHandshakeFailed` → 重建传输对象，重试同一客户端实例上的请求没有意义。
- `HandlerError` / `InternalError` / `DeserializeFailed` → 视为对端缺陷，记日志并停止自动重试。

## raw 通道最容易漏判

<<< @/snippets/handle-errors.cpp#raw-channel

raw 通道不抛异常：错误是**值**。判据是序列化后对象里 `code` 为负数（`src/client/McpClient.cpp:62-72`），漏判就会把错误对象当业务结果解析。便捷方法之所以能抛，正是因为它们在内部做了这一步判定。

## 完整代码

<<< @/snippets/handle-errors.cpp#full

`full` 片段可执行：进程内两端，客户端用 120ms 超时打一个不结算是的工具，打印分类结果对应的枚举值（`TimedOut` 为 `2`）。

## 验证

片段只做语法校验（clang-cl 走 `/Zs`，GCC/Clang 走 `-fsyntax-only`）：

```bash
cmake -S . -B build/p3a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3a --target mcp-doc-snippet-handle-errors
```

期望：configure 打印 `[mcp-docs] syntax-checking 19 snippet(s) with '/Zs'`，构建打印一行 `Building CXX object ...handle-errors.cpp.obj`，返回码 0 且无 `error:` 行。

取消/超时的实测终局码见 [/zh/howto/cancel-a-request](/zh/howto/cancel-a-request) 的验证小节（`-32001` 与 `-32800` 两行原样输出）。

## 时代差异

| 项目 | legacy（≤ 2025-11-25） | modern（2026-07-28） |
|---|---|---|
| `-32001`/`-32003`/`-32004` 出站 | 原样 | 降级为 `-32603` |
| 本地取消/超时码 | `-32800` / `-32001` | 同左（不过重映射） |
| `ProtocolViolation` 触发点 | `logging/setLevel` 可用 | `logging/setLevel` 直接抛 |
| 工具失败主通道 | `is_error` 结果 | 同左，外加 MRTR 的 `input_required` 交回 |

## 常见错误

| 对外表现 | 成因 | 处置 |
|---|---|---|
| catch 到 `McpError` 但 `Code()` 与你抛的不一致 | 工具 handler 抛的码被丢弃 | 工具层只传 message；要传语义就回 `is_error` + 结构化 content |
| 对端报 `-32603`，本地日志写着 timeout | modern 出站重映射 | 别按码分类跨时代行为，分类只看本地 |
| 错误对象被当结果解析 | 用了 raw 通道却没判 `code` | 判负数 `code`，或改用 typed 通道 |
| 一直重试同一客户端 | 连接已关闭 | 重建传输与会话 |
| 服务端把内部异常文本暴露给对端 | 担心 message 泄漏 | 不会：非 `McpError` 一律回 `"internal error"`，细节只在日志 |
| 结构化输出偶发失败导致整调用失败 | schema 声明过头 | 收窄 `output_schema`，或改回非结构化 content |
| 启动即崩溃找不到原因 | 注册期 `InvalidParams` | 把注册收敛成函数并在启动阶段汇总报告 |

## 下一步

- 取消与超时：[/zh/howto/cancel-a-request](/zh/howto/cancel-a-request)
- 工具入参校验：[/zh/howto/validate-tool-input](/zh/howto/validate-tool-input)
- MRTR 续轮与 requestState：[/zh/howto/request-extra-input](/zh/howto/request-extra-input)
- 全表与抛出点：[/zh/reference/errors](/zh/reference/errors)

## 本页对应源码

- `include/mcp/ErrorCodes.hpp`（`McpErrorCode`、类别映射、`make_error_code`）
- `include/mcp/McpError.hpp`（`Code()`、`ErrorMessage()`、`Data()`）
- `include/mcp/protocol/IncomingRequestMeta.hpp`（`RequestOutcome`、`ErrorData`）
- `include/mcp/client/McpClient.hpp`（三通道签名）
- `src/client/McpClient.cpp`（错误判定、异常抛出、会话过期重来）
- `src/protocol/McpSessionHandler.cpp`（出站错误帧与码重映射调用点）
- `src/protocol/WireCodec.cpp`（时代 `EncodeErrorCode`）
- `src/server/McpServer.cpp`（入参预校验、handler 兜底、结构化输出校验、注册期校验）
