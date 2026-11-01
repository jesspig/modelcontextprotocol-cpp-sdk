# 发送协议日志通知

把服务端执行途中产生的日志行，用 MCP 的 `notifications/message` 推给正在连接的客户端：声明 `logging` 能力、按级别过滤、在 handler 内外两种位置发帧。签名与字段表只在 [/zh/reference/server](/zh/reference/server)、[/zh/reference/client](/zh/reference/client) 与 [/zh/reference/types](/zh/reference/types) 维护，本页只讲操作步骤。

## 前置条件

- 头文件：`mcp/server/McpServer.hpp`、`mcp/server/ServerOptions.hpp`、`mcp/server/RequestContext.hpp`、`mcp/client/McpClient.hpp`、`mcp/client/ClientOptions.hpp`、`mcp/Log.hpp`（仅在需要区分内部日志时）、`mcp/McpTypes.hpp`、`mcp/JsonValue.hpp`、`mcp/Content.hpp`、`mcp/Implementation.hpp`、`mcp/transport/InMemoryTransport.hpp`；include 必须写全子目录路径。
- 级别枚举：`LoggingLevel{Debug, Info, Notice, Warning, Error, Critical, Alert, Emergency}`（`include/mcp/Meta.hpp:17-19`），线上是官方字符串 `debug`/`info`/`notice`/`warning`/`error`/`critical`/`alert`/`emergency`（`src/core/Content.cpp:328-330`），越界值直接抛 `InvalidParams`。
- 能力：协议日志要求服务端在 `initialize` 里声明 `logging` 能力，由 `ServerOptions::declare_logging` 控制，**默认 `false`**。
- 协议时代：级别协商方式在两时代完全不同，见「时代差异」。

## 声明 logging 能力

`declare_logging` 是唯一开关：置真后 `capabilities_.logging` 才被写入 `initialize` 响应（`src/server/McpServer.cpp:1959-1961`）。不声明也能发帧——SDK 不做发送前的能力检查——但客户端没有义务监听，且不符合协议对能力协商的要求。

<<< @/snippets/send-log-notifications.cpp#log-capability

## 在 handler 内发日志

首选 `RequestContext::Log(level, data)`。它按**当前请求**的日志级别过滤后投递，因此同一行日志只会回到发起该请求的客户端，不会误伤别的会话。

<<< @/snippets/send-log-notifications.cpp#log-in-handler

三条判定顺序（`include/mcp/server/RequestContext.hpp:64-70`）：modern 时代下请求若没带 `_meta` 日志级别，直接丢弃；带了就丢低于它的行；最后才交给 `McpServer::SendLoggingMessage(level, data, 请求级别)`。

## 在 handler 之外发日志

后台线程、定时器或另一处代码里想补一帧，用 `McpServer` 上的两个重载：

<<< @/snippets/send-log-notifications.cpp#log-outside-handler

两者的实际含义差别很大（`src/server/McpServer.cpp:961-982`）：

- 二参版 `SendLoggingMessage(level, data)` 在 **modern 时代直接 return，什么都不发**；legacy 时代才按 `logging/setLevel` 记录的级别过滤后发送。
- 三参版给了 `min_level` 就**只按 `min_level` 过滤并发送，可越过 `setLevel` 记录**，且不受时代限制；`min_level` 传 `std::nullopt` 时行为回落到二参版。

::: warning 别指望二参重载在 2026 时代出声
`McpServerTest.ModernSendLoggingMessageWithoutRequestIsSilent` 就是把「modern + 二参 = 静默」钉死为预期行为。要在 modern 下主动发帧，必须走三参版并显式给 `min_level`，或者干脆在 handler 里用 `ctx.Log()`。
:::

## 客户端接收与级别过滤

注册 `SetLoggingHandler` 收 `LoggingMessageNotificationParams{level, logger, data}`（`include/mcp/client/McpClient.hpp:126`、`include/mcp/McpTypes.hpp:344-348`）。`logger` 恒为字符串 `"mcp-server"`（`src/server/McpServer.cpp:107`、`988`），`data` 在线上是**一个 JSON 字符串**，不是对象——想传结构化内容就自己序列化后塞进去。

legacy 时代用 `SetLoggingLevel` 做一次性全局协商；modern 时代改为**每次请求**在 `_meta` 里带级别：

<<< @/snippets/send-log-notifications.cpp#log-client-receive

modern 下 `SetLoggingLevel` 会直接抛 `ProtocolViolation("logging/setLevel is not available …; use a per-request logLevel instead")`（`src/client/McpClient.cpp:1913-1919`）。`_meta` 里的键固定是 `io.modelcontextprotocol/logLevel`，且**必须是字符串**（`src/protocol/McpSessionHandler.cpp:58-59`），写整数会让整个请求被拒。

## 与 SDK 内部日志的分工

这是两套互不相干的机制，别混：

| 维度 | 协议日志（本页） | SDK 内部日志 |
|---|---|---|
| 目的 | 给 MCP 客户端看的业务事件 | 给宿主进程排障看的框架事件 |
| API | `ctx.Log`、`McpServer::SendLoggingMessage` | `MCP_LOG`/`MCP_LOG_CTX` 宏、`mcp::SetLogLevel`、`mcp::SetLogHandler` |
| 级别 | `mcp::LoggingLevel`（8 档，协议语义） | `mcp::LogLevel{Off,Error,Warning,Info,Debug,Trace}`（6 档，实现语义） |
| 开关 | `ServerOptions::declare_logging` + 对端级别 | `MCP_LOG_LEVEL` 环境变量或 `mcp::SetLogLevel`，**默认全关** |
| 去处 | `notifications/message` 帧，走传输发给客户端 | stderr，或自定义 handler，不出进程 |

<<< @/snippets/send-log-notifications.cpp#log-vs-internal-log

## 完整代码

服务端声明能力并注册一个会发协议日志的工具，客户端装上 handler 并按请求级别发起一次调用：

<<< @/snippets/send-log-notifications.cpp#full

## 验证

片段只做语法校验（clang-cl 走 `/Zs`，GCC/Clang 走 `-fsyntax-only`），不需要起进程：

```bash
cmake -S . -B build/p2d -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p2d --target mcp-doc-snippet-send-log-notifications
```

期望输出：configure 阶段打印 `[mcp-docs] syntax-checking <N> snippet(s) with '/Zs'`，构建阶段打印

```text
[1/4] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-send-log-notifications.dir\__\__\docs\snippets\send-log-notifications.cpp.obj
```

返回码 0 且无 `error:` 行（`clang-cl: warning: argument unused during compilation: '-c'` 属该模式固有提示，可忽略）。

行为侧由这些既有用例守护，需先 `git submodule update --init third_party/googletest` 再 `ctest --preset debug -R '<用例名>'`：`McpServerTest.InitializeDeclaresExplicitLoggingAndCompletions`、`McpServerTest.SendLoggingMessageEncodesLevelAsString`、`McpServerTest.ModernLoggingRequiresRequestLogLevel`、`McpServerTest.ModernLoggingHonorsRequestLogLevel`、`McpServerTest.ModernSendLoggingMessageWithoutRequestIsSilent`、`McpClientTest.SetLoggingLevelLegacySendsRequest`、`McpClientTest.SetLoggingLevelModernRejected`、`McpTypesTest.LoggingLevelRoundTrip`、`LogTest.LevelFilterBelowNotTriggered`。

## 时代差异

| 项目 | legacy（≤ 2025-06-18） | modern（2026-07-28） |
|---|---|---|
| 级别协商方式 | 客户端发 `logging/setLevel`，服务端记住一个全局级别 | 无 `logging/setLevel`；级别随每个请求的 `_meta.io.modelcontextprotocol/logLevel` 走 |
| 服务端过滤依据 | `setLevel` 记录的级别 | 该请求自带的级别；没带就**一条都不发** |
| `SendLoggingMessage(level, data)` | 按记录级别过滤后发送 | **直接 return，静默不发** |
| 投递通道 | `handler_->SendNotification` 广播 | 先试 `BufferRequestStreamNotification` 写进 hold-open 请求流 |
| 缓冲失败后 | 不适用 | 传输要求请求级通知时**丢弃该帧**（与进度不同，进度会回落成全局通知） |
| 客户端 `SetLoggingLevel` | 正常发请求 | 抛 `ProtocolViolation` |

两时代的通知体形状一致：`{"level":"<官方字符串>","data":"<字符串>","logger":"mcp-server"}`。modern 额外在信封 `_meta` 带 `protocolVersion`。

## 常见错误

| 对外表现 | 成因 | 处置 |
|---|---|---|
| 客户端一条日志都收不到（modern） | 请求没带 `_meta` 日志级别，`ctx.Log` 按设计全部丢弃 | 客户端在 `RequestOptions::meta` 里带上 `io.modelcontextprotocol/logLevel` |
| 请求直接被拒，报 `io.modelcontextprotocol/logLevel` 非法 | 该字段写成了整数而非官方字符串 | 只写 `"debug"`…`"emergency"` 八个字符串之一 |
| modern 下 `SetLoggingLevel` 抛 `ProtocolViolation` | 该方法在 2026 时代已从协议移除 | 改用每请求 `_meta` 级别 |
| `initialize` 里没有 `logging` 能力 | `declare_logging` 默认 `false` | 显式置真 |
| 二参 `SendLoggingMessage` 在 modern 毫无输出 | 该重载在 modern 首行即 return | 用三参版显式给 `min_level`，或改在 handler 里 `ctx.Log` |
| 想「强制发一条」却仍被过滤掉 | 三参版传了 `std::nullopt`，行为回落到二参版 | 要越过滤镜必须传真数值 |
| `data` 收到的是带引号的字符串而非对象 | 协议日志把 `data` 当字符串写出 | 自己 `Dump()`/`JSON.stringify` 一侧约定好 |
| 多会话服务端里日志串了时代 | `EmitLoggingMessage`/`SendProgress` 判时代用的是**全局**协商版本（`NegotiatedProtocolVersion()` 无参会话） | 多会话混代时改为显式三参重载并按会话取版本 |
| 打开 `MCP_LOG_LEVEL=4` 却看不到 `notifications/message` | 那是 SDK 内部日志开关，与协议日志无关 | 见「与 SDK 内部日志的分工」 |

## 下一步

- 上报执行进度（比例、里程碑）：[/zh/howto/report-progress](/zh/howto/report-progress)
- 把慢工具改成可轮询的任务：[/zh/howto/run-long-tasks](/zh/howto/run-long-tasks)
- 资源变更推送：[/zh/howto/serve-a-resource](/zh/howto/serve-a-resource)
- 双时代语义全表：[/zh/learn/protocol-versions](/zh/learn/protocol-versions)
- 签名与字段表：[/zh/reference/server](/zh/reference/server)、[/zh/reference/client](/zh/reference/client)、[/zh/reference/types](/zh/reference/types)

## 本页对应源码

- `include/mcp/Meta.hpp`（`LoggingLevel`、`RequestMeta::log_level`）
- `include/mcp/McpTypes.hpp`（`LoggingMessageNotificationParams`、`SetLevelRequestParams`、`RequestOptions::meta`）
- `include/mcp/server/ServerOptions.hpp`（`declare_logging`）
- `include/mcp/server/McpServer.hpp`（`SendLoggingMessage` 两个重载）
- `include/mcp/server/RequestContext.hpp`（`Log`、`LogLevel`）
- `include/mcp/client/McpClient.hpp`（`SetLoggingHandler`、`SetLoggingLevel`）
- `include/mcp/Log.hpp`（`LogLevel`、`SetLogLevel`、`SetLogHandler`、`MCP_LOG_LEVEL`）
- `include/mcp/JsonValue.hpp`、`include/mcp/Content.hpp`、`include/mcp/Implementation.hpp`
- `include/mcp/transport/InMemoryTransport.hpp`
- `src/server/McpServer.cpp`（能力声明、两个重载与 `EmitLoggingMessage`、请求级级别解析）
- `src/client/McpClient.cpp`（`setLevel` 的时代门控与 handler 注册）
- `src/protocol/McpSessionHandler.cpp`（`_meta` 字段合法性校验）
- `src/core/Content.cpp`（级别官方字符串映射）
- `src/http/StreamableHttpServerTransport.cpp`（`notifications/message` 广播进 hold-open 流）
