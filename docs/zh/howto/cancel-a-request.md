# 取消请求与超时

让一个已经发出去、但不再需要的请求**尽快出局**：客户端调 `RequestHandle::Cancel` 或让超时到期，SDK 立刻把它本地结算成终局错误，并按时代与传输类型决定要不要通知对端；服务端只能协作式感知，不会被打断。签名与字段表见 [/zh/reference/client](/zh/reference/client) 与 [/zh/reference/transports](/zh/reference/transports)，本页只讲操作。

::: info 与关连接、任务的分工
取消针对**单个请求 ID**。要拆掉整条连接用 `McpClient::Close()`（在途请求统一结算为 `ConnectionClosed`，见 [/zh/howto/connect-to-a-server](/zh/howto/connect-to-a-server)）；要让活儿在断开后仍能回查与续跑，改用任务，见 [/zh/howto/run-long-tasks](/zh/howto/run-long-tasks)。
:::

## 前置条件

- 头文件：`mcp/client/McpClient.hpp`、`mcp/client/RequestHandle.hpp`、`mcp/CancelToken.hpp`、`mcp/McpTypes.hpp`、`mcp/protocol/IncomingRequestMeta.hpp`、`mcp/Content.hpp`、`mcp/JsonValue.hpp`、`mcp/server/McpServer.hpp`、`mcp/server/RequestContext.hpp`、`mcp/transport/InMemoryTransport.hpp`；include 必须写全子目录路径。
- 只有走 `RequestOptions` 的 `SendRequest` 才返回带取消能力的 `RequestHandle`（`include/mcp/client/McpClient.hpp`）；三参重载里它是必填，**没有默认实参**。
- 不需要任何扩展开关，也不看服务端能力——取消是协议内建的 `notifications/cancelled`。
- `RequestHandle` 只可移动、不可拷贝（`include/mcp/client/RequestHandle.hpp`），别把它塞进容器再拷贝。

## 取消一个在途请求

`RequestHandle::Cancel(reason)` 的 `reason` 可省。SDK 的结算顺序是固定的三步（`src/protocol/McpSessionHandler.cpp`）：先从 pending 表里**取走**该 ID（取不到就直接返回，所以重复取消是 no-op，取消一个已完成的请求也是 no-op），再按时代与传输决定怎么通知对端，最后本地投递 `RequestCancelled(-32800)`。

<<< @/snippets/cancel-a-request.cpp#typed-handle

`reason` 非空时会被塞进 `ErrorData::data` 的 `{"reason": ...}`，同时在出站通知里带上。`RequestOutcome` 是 `std::variant<JsonValue, ErrorData>`（`include/mcp/protocol/IncomingRequestMeta.hpp`），用 `std::get_if<JsonValue>` 分流成功与失败。

::: warning 取消即本地终局
一旦 pending 被取走，服务端后面真的把结果发回来也**无人认领**——响应查不到对应条目就被丢弃。不要把「取消后可能仍会完成」当成可以慢慢等的理由。
:::

## 超时、续期与是否顺发取消

`RequestOptions::timeout` 是这一条请求的截止线；`ClientOptions::max_total_timeout` 另给整个客户端设一条绝对截止线，默认 `0` 即不启用（`include/mcp/client/ClientOptions.hpp`），启用后每条出站请求都会登记绝对截止（`src/client/McpClient.cpp`、`src/protocol/McpSessionHandler.cpp`），与单次截止**先到者触发**。到期时：

- `cancel_on_timeout` 为真（**默认值**，`include/mcp/McpTypes.hpp`）会顺带走一遍传输级取消，`reason` 固定写成 `"timeout"`；
- 本地结算码**恒为 `RequestTimeout(-32001)`**，不是 `-32800`——想区分「我取消的」和「到点的」，看码别看 reason。

<<< @/snippets/cancel-a-request.cpp#timeout-only

带 `progressToken` 的请求收到进度帧时，剩余时间会被夹到不少于续期步长（`ResetTimeoutByProgressToken`），所以持续上报本身就在续命，见 [/zh/howto/report-progress](/zh/howto/report-progress)。

## 一个令牌取消整批请求

`CancelToken` 是可拷贝的共享状态句柄：把它放进 `RequestOptions::cancel_token`，多个请求就挂在同一次取消上，一次 `token.Cancel(reason)` 全部出局（`src/client/McpClient.cpp`）。回调在请求结束时自动摘除（订阅登记与摘除见 `src/client/McpClient.cpp`），不需要你清理。

<<< @/snippets/cancel-a-request.cpp#cancel-token-fanout

`CancelToken` 还提供 `IsCancelled()` / `Reason()` / `AddCancelHandler()`，可以先判后再发新请求，避免把注定被取消的活儿发出去。

## 只有请求 ID 时

拿不到句柄（例如你自己实现了出站队列）时走 `McpClient::CancelRequest(id, reason)`，语义与 `RequestHandle::Cancel` 完全一致——两者都只是转调 `McpSessionHandler::CancelRequest`（`src/client/McpClient.cpp`）。

<<< @/snippets/cancel-a-request.cpp#cancel-by-id

## 三条失败通道，别混用

同一个「失败」在三条 API 上长相完全不同，这是最常见的踩坑点：

| 调用形态 | 失败时的表现 | 源码位置 |
|---|---|---|
| 便捷方法（`CallTool`、`ListTools`、`ReadResource`……） | **抛 `McpError`**，码与消息可用 `Code()`/`what()` 读 | `src/client/McpClient.cpp` |
| `SendRequestRaw`（以及四参 `SendRequest`，它只是转调前者） | future **正常 resolve**，值是序列化后的错误对象 `{"code": <负数>, "message": ...}`，不抛 | `src/protocol/McpSessionHandler.cpp`、`src/client/McpClient.cpp` |
| `SendRequest(method, params, RequestOptions)` | 返回 `RequestOutcome` variant，`std::get_if<ErrorData>` 取错 | `include/mcp/client/RequestHandle.hpp` |

<<< @/snippets/cancel-a-request.cpp#three-channels

便捷方法之所以能抛，是因为它把 raw 通道的值再判一次：`code` 存在且为负即抛。于是 `code` 为正数的正常结果不受影响，但**你在 raw 通道上必须自己判**——忘了判就会把错误对象当结果解析。

## 服务端只能协作式感知

对端的取消到达时，SDK 只做一件事：把该 (会话, 请求 ID) 的原子标志置真（`src/protocol/McpSessionHandler.cpp`）。**它不会打断你的 handler**，也不会替你回错误——`RequestContext::IsCancellationRequested()` 只是读那个标志（`include/mcp/server/RequestContext.hpp`），要早点收摊必须自己轮询。

<<< @/snippets/cancel-a-request.cpp#server-side-poll

标志在请求结束时擦除、连接关闭时清空（`src/protocol/McpSessionHandler.cpp`），所以轮询只在该请求还活着期间有意义。长循环里还建议顺带上报进度，见 [/zh/howto/report-progress](/zh/howto/report-progress)。

::: warning 命中标志需要同一会话
入站取消用 `SessionRequestKey(session_id, request_id)` 定位标志，而 pending 表用裸请求 ID 定位。stateful HTTP 下若通知的 `sessionId` 与请求不属于同一会话，标志就命中不了。
:::

## 完整代码

一个进程内起两端：注册一个会轮询取消标志的工具，客户端发起调用后立刻取消，并打印终局。

<<< @/snippets/cancel-a-request.cpp#full

## 验证

片段只做语法校验（clang-cl 走 `/Zs`，GCC/Clang 走 `-fsyntax-only`），不需要起进程：

```bash
cmake -S . -B build/p3f -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3f --target mcp-doc-snippet-cancel-a-request
```

期望输出：configure 阶段打印 `[mcp-docs] syntax-checking 13 snippet(s) with '/Zs'`，构建阶段打印

```text
[1/2] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-cancel-a-request.dir\__\__\docs\snippets\cancel-a-request.cpp.obj
```

返回码 0 且无 `error:` 行（`clang-cl: warning: argument unused during compilation: '-c'` 属该模式固有提示）。

要看真实终局码，跑仓库示例（需 `-DMCP_BUILD_EXAMPLES=ON`）：

```bash
cmake --build build/p3f --target RequestCancellation
build/p3f/examples/RequestCancellation/RequestCancellation.exe
```

实测输出四行、返回码 0：

```text
timeout handle id: 2
error: request timed out (code -32001)
cancel handle id: 3
error: request cancelled (code -32800)
```

即「超时结算为 `-32001`、显式取消结算为 `-32800`」——两条都来自本地，与对端是否真的停止无关。

## 时代差异

| 项目 | legacy（≤ 2025-11-25） | modern（2026-07-28） |
|---|---|---|
| 通知方式 | 一律发 `notifications/cancelled` | 传输实现 `IRequestCanceller` 时**只关该请求的 SSE 流**，不发通知；未实现则回落到通知 |
| 判定位置 | — | `src/protocol/McpSessionHandler.cpp`（`dynamic_cast` 探测） |
| 接口 | — | `include/mcp/transport/IRequestCanceller.hpp` |
| 本地终局码 | `-32800` / `-32001` / `-32000` | 同上，本地结算**不过** `EncodeErrorCode`，码不变 |
| 对端传来的这些码 | 原样 | `-32001`/`-32003`/`-32004` 被降级为 `-32603`（`src/protocol/WireCodec.cpp`） |

::: tip 关流不是万能
现代 HTTP 的「关掉请求流」只到 handler/canceller 一层：`InMemoryTransport` 与 stdio 传输不实现 `IRequestCanceller`，即使在 modern 时代也仍走 `notifications/cancelled`（`src/protocol/McpSessionHandler.cpp` 的回落分支）。
:::

## 常见错误

| 对外表现 | 成因 | 处置 |
|---|---|---|
| `Cancel()` 调了但 future 迟迟不返回 | 该请求早已被结算（pending 取不到即静默 no-op） | 每个 ID 只取消一次；`future` 一定要 `get()` 或 `wait_for` |
| 想区分主动取消与超时，结果分不出来 | 把两者当同一码 | 主动取消 `-32800`，超时 `-32001`；`reason` 只是文本 |
| 超时了但服务端还在干活 | `cancel_on_timeout = false`，或传输/时代组合只发了通知没落地的效果 | 默认保持 `true`，并让服务端轮询标志 |
| 服务端收到取消却没停 | SDK 只置标志，不打断 handler | handler 内在每个工作单元前判 `ctx.IsCancellationRequested()` |
| 取消后服务端的结果把状态改坏了 | 结果被 SDK 丢弃，但服务端**仍在写副作用** | 副作用要在轮询到取消后立刻收敛，或走任务 |
| 本地超时在 modern 下期望看到 `-32603` | 重映射只发生在**出站**编码 | 客户端本地码恒为 `-32001`；`-32603` 只会来自对端 |
| `RequestHandle` 编译报拷贝删除 | 句柄 move-only | 用 `std::move` 转移所有权，别拷贝 |
| 取消通知命中不了在途请求 | stateful HTTP 下 `sessionId` 与请求不同 | 在同一会话里取消，见 [/zh/howto/run-a-streamable-http-server](/zh/howto/run-a-streamable-http-server) |

## 下一步

- 连接与关闭时机：[/zh/howto/connect-to-a-server](/zh/howto/connect-to-a-server)
- 让长活儿可回查：[/zh/howto/run-long-tasks](/zh/howto/run-long-tasks)
- 用进度给慢请求续命：[/zh/howto/report-progress](/zh/howto/report-progress)
- 错误码全表与重映射：[/zh/reference/errors](/zh/reference/errors)
- 双时代语义全表：[/zh/learn/protocol-versions](/zh/learn/protocol-versions)

## 本页对应源码

- `include/mcp/client/RequestHandle.hpp`（`Cancel`、`future`、move-only）
- `include/mcp/client/McpClient.hpp`（`SendRequest(RequestOptions)`、`SendRequestRaw`、`CancelRequest`）
- `include/mcp/CancelToken.hpp`、`include/mcp/McpTypes.hpp`（`RequestOptions`）
- `include/mcp/protocol/IncomingRequestMeta.hpp`（`RequestOutcome`）
- `include/mcp/transport/IRequestCanceller.hpp`
- `include/mcp/server/RequestContext.hpp`（`IsCancellationRequested`）
- `include/mcp/ErrorCodes.hpp`
- `src/protocol/McpSessionHandler.cpp`（取消结算、超时扫描、入站标志）
- `src/client/McpClient.cpp`（句柄装配、令牌订阅、错误分型）
- `src/protocol/WireCodec.cpp`（时代错误码重映射）
- `examples/RequestCancellation/main.cpp`
