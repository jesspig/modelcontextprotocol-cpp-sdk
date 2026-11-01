# 上报执行进度

在一次慢请求还没返回之前，把「已经做到哪一步」推给发起方：服务端读请求自带的 `progressToken`，用 `McpServer::SendProgress` 发 `notifications/progress`，客户端在 `RequestOptions::on_progress` 里收。签名与字段表只在 [/zh/reference/server](/zh/reference/server)、[/zh/reference/client](/zh/reference/client) 与 [/zh/reference/types](/zh/reference/types) 维护，本页只讲操作步骤。

::: info 与任务、日志的分工
进度是**同一请求内的临时心跳**：请求一旦返回，token 就失效。要让用户在断开之后还能回查，用 [/zh/howto/run-long-tasks](/zh/howto/run-long-tasks)；要送人类可读的日志行，用 [/zh/howto/send-log-notifications](/zh/howto/send-log-notifications)。
:::

## 前置条件

- 头文件：`mcp/server/McpServer.hpp`、`mcp/server/RequestContext.hpp`、`mcp/server/ServerOptions.hpp`、`mcp/client/McpClient.hpp`、`mcp/client/ClientOptions.hpp`、`mcp/McpTypes.hpp`、`mcp/JsonValue.hpp`、`mcp/Content.hpp`、`mcp/Implementation.hpp`、`mcp/transport/InMemoryTransport.hpp`；include 必须写全子目录路径。
- 令牌类型：`ProgressToken = std::variant<std::string, int64_t>`（`include/mcp/Meta.hpp:15`），线上只允许字符串或整数，别的类型会让整个请求被拒（`src/protocol/McpSessionHandler.cpp:52-53`）。
- 开关：进度不需要任何扩展开关，也不要求服务端声明能力——它是协议内建通知。
- 无需额外 CMake 选项。

## 取回 progressToken

令牌不属于 `CallToolRequestParams` 的业务字段，SDK 把请求的 `_meta` 归一化进 `params.meta`，所以统一从 `ctx.Params().meta->progress_token` 取，返回 `std::optional<ProgressToken>`。没带令牌时它是空的——这是常态而不是错误。

<<< @/snippets/report-progress.cpp#progress-server

判据在 `src/server/McpServer.cpp:2093-2108`：modern 从信封级 `_meta` 反序列化、legacy 从 `params._meta` 读，两时代最终都落到同一个 `params.meta->progress_token`，业务代码无需分支。

## 在服务端上报

`SendProgress(token, progress, total?, message?)` 是 `McpServer` 的公开方法，handler 里通过 `ctx.Server()` 拿到；也可以在 handler 之外（后台线程、定时器）对缓存下来的 token 调用。四个形参中只有 `token` 与 `progress` 必填。

- `progress` 是**单调递增的裸计数**，SDK 不做归一化、不检查是否超过 `total`，客户端自己算比例。
- `total` 可省；省略表示总量未知。
- 令牌**原样透传，SDK 不校验归属**：把 A 请求的 token 用于 B 请求的进度，帧就会被投到 A 的流里（`src/server/McpServer.cpp:1091-1109` 里没有任何归属判定）。跨请求复用 token 请自己保证。

<<< @/snippets/report-progress.cpp#progress-total-less

## 客户端接收进度

在 `RequestOptions` 上挂 `on_progress`，类型是 `std::function<void(const ProgressNotificationParams&)>`（`include/mcp/McpTypes.hpp:402`）。不显式给令牌时，客户端会自动分配一个自增整数令牌并写进 `_meta.progressToken`；显式给就用你给的（`src/client/McpClient.cpp:937-967`）。回调在请求结束时自动摘除，不需要手动清理。

<<< @/snippets/report-progress.cpp#progress-client

回调只在**该请求**的令牌匹配时被调用，因此把它做成无共享状态的 lambda 最省心；要更新 UI 就在回调里投递到主线程。

## 没有 token 时的行为

- **服务端侧**：`ctx.Params().meta->progress_token` 为空。此时唯一的正确做法是**跳过上报**，不要自造一个令牌——对方没有登记任何令牌，帧发过去无人认领。工具照常返回结果即可，如上面 `progress-server` 片段里的降级分支。
- **客户端侧**：`RequestOptions::on_progress` 未设时，客户端仍会登记一个自动令牌，但收到的进度帧被直接忽略（用例 `McpClientTest.CallToolWithoutProgressCallbackIgnoresProgress`）。
- 进度通知**不影响请求结果**：任何一帧丢了、发早了、发晚了，都不会改变响应体，也不会让请求失败。

::: tip 进度可给长请求续期
进度帧会重置对应请求的超时护栏：服务端收到客户端方向的 `notifications/progress` 后调 `ResetTimeoutByProgressToken`（`src/server/McpServer.cpp:1437-1455`），客户端方向的实现见 `SessionHandlerTest.ProgressNotificationExtendsDeadline`。所以持续上报本身就能给慢请求延长寿命。
:::

## 完整代码

服务端注册一个会上报进度的工具，客户端带回调发起调用并另起一次显式令牌的调用：

<<< @/snippets/report-progress.cpp#full

## 验证

片段只做语法校验（clang-cl 走 `/Zs`，GCC/Clang 走 `-fsyntax-only`），不需要起进程：

```bash
cmake -S . -B build/p2d -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p2d --target mcp-doc-snippet-report-progress
```

期望输出：configure 阶段打印 `[mcp-docs] syntax-checking <N> snippet(s) with '/Zs'`，构建阶段打印

```text
[1/4] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-report-progress.dir\__\__\docs\snippets\report-progress.cpp.obj
```

返回码 0 且无 `error:` 行（`clang-cl: warning: argument unused during compilation: '-c'` 属该模式固有提示，可忽略）。

行为侧由这些既有用例守护，需先 `git submodule update --init third_party/googletest` 再 `ctest --preset debug -R '<用例名>'`：`McpServerTest.SendProgressDeliversNotificationWithStringToken`、`McpServerTest.SendProgressDeliversNotificationWithInt64Token`、`McpClientTest.CallToolReceivesProgressNotifications`、`McpClientTest.CallToolWithoutProgressCallbackIgnoresProgress`、`McpClientTest.ProgressCallbackRemovedAfterRequestCompletes`、`SessionHandlerTest.LegacyEraRequestCarriesProgressTokenInParams`、`SessionHandlerTest.ModernEraRequestCarriesProgressTokenInParamsMetaOnWire`、`SessionHandlerTest.IntProgressTokenExtendsDeadline`、`StreamableHttpTransportTest.RequestStreamHoldOpenDeliversProgressBeforeResult`。

## 时代差异

| 项目 | legacy（≤ 2025-06-18） | modern（2026-07-28） |
|---|---|---|
| `_meta.progressToken` 的落位 | 留在 `params._meta`，`JsonRpcRequest::meta` 为空 | 提到信封级 `_meta`，序列化时再写回 `params._meta` |
| 业务代码读取方式 | `ctx.Params().meta->progress_token` | 同左，SDK 已归一化，无需分支 |
| 通知投递通道 | `SendNotification` 直接广播到传输 | 先试 `BufferRequestStreamNotification` 写进所属请求的流 |
| 路由依据 | 无（不区分请求） | 按 `progress_token_routes_` 找回发起该令牌的 hold-open 请求 |
| 缓冲失败后 | 不适用 | 回落到独立的全局通知（与协议日志不同，进度**不**被 `RequiresRequestScopedNotifications` 吞掉） |
| stateless HTTP | 不适用 | 进度帧与最终响应同流到达，先帧后响应 |

通知体本身两时代一致：`{"progressToken":…,"progress":…,"total":…,"message":…}`，`total` 与 `message` 可缺省。

## 常见错误

| 对外表现 | 成因 | 处置 |
|---|---|---|
| 整个请求被拒，报 `_meta.progressToken` 非法 | 令牌传了布尔、浮点或对象 | 只用字符串或整数 |
| 客户端 `on_progress` 一次都没进 | 服务端没读到令牌就自己造了一个，或上报用了另一个请求的令牌 | 一律回读 `ctx.Params().meta->progress_token`；令牌为空就跳过上报 |
| 服务端明明发了，客户端收不到 | 回调挂在别的调用上，或该调用早已返回、回调被摘除 | 令牌生命周期等于请求生命周期，别跨请求复用 |
| 进度到 100 之后客户端算出的比例超过 1 | `progress` 是裸计数，SDK 不封顶也不归一化 | 上报前自己夹取，或让 `total` 与实际步数一致 |
| 不传 `total` 后客户端显示「未知比例」 | 省略 `total` 表示总量未知，属预期 | 需要比例就带上 `total` |
| 慢请求在跑完前超时 | 中途没有可续期的帧，或上报间隔大于超时窗口 | 按里程碑持续上报（见上方 tip） |
| 长任务断开后无从回查 | 进度不是持久状态 | 改用任务，见 [/zh/howto/run-long-tasks](/zh/howto/run-long-tasks) |

## 下一步

- 把慢工具改成可轮询的任务：[/zh/howto/run-long-tasks](/zh/howto/run-long-tasks)
- 送结构化的人类日志行：[/zh/howto/send-log-notifications](/zh/howto/send-log-notifications)
- 任务里反问用户：[/zh/howto/request-extra-input](/zh/howto/request-extra-input)、[/zh/howto/collect-user-input](/zh/howto/collect-user-input)
- 双时代语义全表：[/zh/learn/protocol-versions](/zh/learn/protocol-versions)
- 签名与字段表：[/zh/reference/server](/zh/reference/server)、[/zh/reference/client](/zh/reference/client)、[/zh/reference/types](/zh/reference/types)

## 本页对应源码

- `include/mcp/Meta.hpp`（`ProgressToken`、`RequestMeta::progress_token`）
- `include/mcp/McpTypes.hpp`（`CallToolRequestParams::meta`、`ProgressNotificationParams`、`RequestOptions::on_progress`）
- `include/mcp/server/McpServer.hpp`（`SendProgress`）
- `include/mcp/server/RequestContext.hpp`（`Params()`、`Server()`）
- `include/mcp/server/ServerOptions.hpp`
- `include/mcp/client/McpClient.hpp`、`include/mcp/client/ClientOptions.hpp`
- `include/mcp/JsonValue.hpp`、`include/mcp/Content.hpp`、`include/mcp/Implementation.hpp`
- `include/mcp/transport/InMemoryTransport.hpp`
- `src/server/McpServer.cpp`（令牌归一化、上报实现、入站进度续期）
- `src/client/McpClient.cpp`（令牌分配与回调登记/摘除）
- `src/protocol/McpSessionHandler.cpp`（`_meta` 字段合法性校验）
- `src/http/StreamableHttpServerTransport.cpp`（按令牌路由到所属请求流）
