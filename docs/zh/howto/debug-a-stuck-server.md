# 排查卡住的服务端

请求发出去、石沉大海。这一页把「没有响应」拆成可判定的三类成因，并给出每种情况最短的定位手段：内部日志、日志汇、span 钩子、conformance 复现。协议语义本身见 [/zh/learn/protocol-versions](/zh/learn/protocol-versions)。

## 先分类：三种「没反应」

| 观察到的现象 | 属于哪类 | 直接跳 |
|---|---|---|
| 客户端一直等，超时后拿到 `RequestTimeout(-32001)` | 服务端把请求吞了（没兑现 promise，或异常落在 `std::async` 里） | 成因一 |
| 空闲一段时间后连接被判死 | 读循环把「无数据」当 EOF | 成因二 |
| 立刻回错误帧，错误码像 `-32021`/`-32601` | 能力没声明或时代不匹配 | 成因三 |

区分「连接死了」和「请求没结」：前者 `ClientOptions::on_transport_close` / `on_transport_error` 会响，后者不会。传输层回调见 [/zh/howto/connect-to-a-server](/zh/howto/connect-to-a-server)。

## 打开内部日志

日志默认**全关**。环境变量 `MCP_LOG_LEVEL` 取 0-5（`Off`/`Error`/`Warning`/`Info`/`Debug`/`Trace`），在日志子系统第一次被访问时读取一次；`>= 5` 一律按 `Trace`，非数字或 0 等价于关闭（`include/mcp/Log.hpp:51-64`）。运行时想改就 `mcp::SetLogLevel(LogLevel::Debug)`，它直接覆盖当前级别（`include/mcp/Log.hpp:99-101`）。

<<< @/snippets/debug-a-stuck-server.cpp#enable-internal-logs

默认汇把每条记录写到 **stderr**，形如 `[MCP][时:分:秒][tag] 消息`，时间戳是 **UTC**（`gmtime` + `%H:%M:%S`，`include/mcp/Log.hpp:66-78`、`:124-128`）。排查 stdio 服务端时这一点很关键：stderr 不会污染协议流，但如果子进程的 stderr 被丢弃，你就什么都看不见——用 `StdioClientTransportOptions::stderr_path` 或 `on_stderr` 收起来，见 [/zh/howto/manage-subprocesses](/zh/howto/manage-subprocesses)。

## 换成自己的日志汇

`SetLogHandler` 接管全部内部日志，记录里带 `level`、`tag`、`message`、`file`、`line`，以及可选的 `context`（`request_id`、`method`、`session_id`、`peer`，`include/mcp/Log.hpp:23-39`）。挂上自定义汇之后**默认 stderr 输出就没了**，传 `nullptr` 恢复。

<<< @/snippets/debug-a-stuck-server.cpp#custom-log-sink

::: info 日志汇抛异常不会拖垮协议
`LogWrite` 对 handler 的调用包在 `try { } catch (...) { }` 里（`include/mcp/Log.hpp:116-119`）——异常被吞掉，请求继续跑。反过来说：别指望日志汇里的异常能被上层捕获。
:::

## 用 span 钩子看帧的进出

`SpanEvent` 是结构化观测点：`kind` 分 `ServerRequest` / `TransportReceive` / `TransportSend`，`phase` 分 `Begin` / `End`，并带 `method`、`request_id`、`failed` 与 `traceparent`/`tracestate`/`baggage`（`include/mcp/SpanHooks.hpp:9-32`）。服务端挂在 `ServerOptions::span_handler`（`include/mcp/server/ServerOptions.hpp:64`），客户端挂在 `ClientOptions::span_handler`（`include/mcp/client/ClientOptions.hpp:69`）。

<<< @/snippets/debug-a-stuck-server.cpp#span-dump

判读法：一个请求 ID 只有 `ServerRequest/Begin` 没有 `ServerRequest/End`，就是 handler 没走完；两端都有但客户端没收到，看 `TransportSend` 是否发了、对端 `TransportReceive` 是否收到——问题就在传输或路由上。

## 成因一：handler 没兑现 promise

服务端把每个请求的应答通道以 `std::promise<JsonValue>` 交给 handler（`src/protocol/McpSessionHandler.cpp:479-489`）。**既不 set_value 也不 set_exception 就等于永久沉默**：SDK 不会替你补一个错误响应，客户端只能等到超时。异步 handler 常见写法是把 promise 存进队列稍后再兑现：

<<< @/snippets/debug-a-stuck-server.cpp#held-promise

这就是 `held.Held()` 这类计数器的用处——**给未兑现的 promise 数量做指标**，卡住时先看它涨没涨。仓库示例里就是这么教的：`examples/RequestCancellation/main.cpp:124-128`。

顺带一条同源陷阱：注册工具时 `input_schema` 里放**循环 `$ref`**，校验器抛异常会落在 `std::async` 的 future 内，而 `Close()` 只 `wait()` 不 `get()`（`src/server/McpServer.cpp:742-748`），异常于是被永久遗忘，表现也是「这个请求再也没回」。用 schema 时避免自引用环，或把 `$ref` 展开成有限结构。

## 成因二：把空闲当 EOF

自写传输或自己驱动 stdio 读写时会碰到：POSIX 侧 `Read` 用 100ms poll 轮询，**无数据也返回 0**，真 EOF 必须问 `IsEof()`（`include/mcp/transport/detail/PlatformIO.hpp:20-24`）。SDK 内部的读循环正是这么写的（`src/transport/StdioClientTransport.cpp:103-105`、`:130-132`）。少了这个判据，空闲 100ms 就会被误判成对端退出，症状是「跑一会儿就断」。

## 成因三：时代或能力不匹配

「立刻报错」类卡住通常是声明与实际不符：

- 服务端没 `declare_logging` 就没有 `logger` 能力，客户端 `SetLoggingLevel` 自然失败；`declare_logging` 与 `declare_completions` 默认都是 `false`（`include/mcp/server/ServerOptions.hpp:51`）。
- modern 时代没有 `logging/setLevel`：客户端调 `SetLoggingLevel` 直接抛 `ProtocolViolation`，提示改用请求级 `logLevel`（`src/client/McpClient.cpp:1913-1925`）。
- elicitation 需要**目标会话**声明了对应能力，否则 `-32021`；会话解析与回退顺序见 [/zh/howto/collect-user-input](/zh/howto/collect-user-input)。
- `tasks`、`skills` 扩展开关没开时，入口直接 `MethodNotFound`，见 [/zh/howto/run-long-tasks](/zh/howto/run-long-tasks)。

时代矩阵统一在 [/zh/learn/protocol-versions](/zh/learn/protocol-versions)；先用 `McpClient::GetNegotiatedProtocolVersion()` 确认双方到底谈成了哪个版本，再决定往哪边查。

## 用 conformance 复现

最小复现环境是仓库自带的 fixture，跑官方裁判：

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMCP_BUILD_CONFORMANCE=ON
cmake --build build/debug --target conformance-server
CONFORMANCE_SPEC_VERSION=2026-07-28 ./scripts/run-conformance.sh server
```

脚本要点（`scripts/run-conformance.sh`）：`MODE` 取 `server` 或 `client`；`PORT` 默认 3010，**端口已被占用会直接退出**（先杀残留进程或换端口）；`CONFORMANCE_SPEC_VERSION` 决定用哪份基线（`tests/conformance/baseline-2026-07-28.yaml` / `baseline-2025-11-25.yaml` / 默认 `baseline.yaml`）；`2026-07-28` 这一路会以 `--stateless` 启动服务端；裁判本身经 `npx -y @modelcontextprotocol/conformance@0.2.0-alpha.12` 拉取，需要网络。基线里的条目是「已知不达标」清单，**新增 unexpected failures 才是回归信号**。

## 验证

片段只做语法校验（clang-cl 走 `/Zs`，GCC/Clang 走 `-fsyntax-only`）：

```bash
cmake -S . -B build/p3a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3a --target mcp-doc-snippet-debug-a-stuck-server
```

期望：configure 打印 `[mcp-docs] syntax-checking 17 snippet(s) with '/Zs'`，构建打印一行 `Building CXX object ...debug-a-stuck-server.cpp.obj`，返回码 0 且无 `error:` 行。

`full` 片段本身可执行（进程内一客户端一「故意扣住 promise」的服务端）：它打印 `held promises: 1` 后返回，正是「请求被吞」的最小形态。要看取消后的终局码，跑 `RequestCancellation` 示例（见 [/zh/howto/cancel-a-request](/zh/howto/cancel-a-request) 的实测输出）。

## 速查表

| 症状 | 第一步 | 常见成因 |
|---|---|---|
| 请求永久无响应 | 数未兑现的 promise | handler 既不 set_value 也不 set_exception |
| 某类请求全无响应且无日志 | 查 schema 是否含循环 `$ref` | 异常落在 `std::async` future 内被遗忘 |
| 空闲后掉线 | 看读循环 | `Read()` 返回 0 未配 `IsEof()` |
| 报错码 `-32021` | 打 `GetNegotiatedProtocolVersion()` | 目标会话未声明能力 |
| `logging/setLevel` 报 `ProtocolViolation` | 看时代 | modern 改用请求级 `_meta.logLevel` |
| 什么都看不到（stdio） | 收 stderr | 日志写 stderr 而子进程 stderr 被丢弃 |
| 日志刷屏后反而更找不到 | 降级别 + 挂自己的汇 | `Trace` 含传输细节，噪声压过信号 |
| conformance 拒绝启动 | 换 `PORT` | 端口被上一轮残留进程占用 |

## 下一步

- 取消与超时语义：[/zh/howto/cancel-a-request](/zh/howto/cancel-a-request)
- 子进程与 stderr 收集：[/zh/howto/manage-subprocesses](/zh/howto/manage-subprocesses)
- 连接与传输回调：[/zh/howto/connect-to-a-server](/zh/howto/connect-to-a-server)
- 错误码全表：[/zh/reference/errors](/zh/reference/errors)

## 本页对应源码

- `include/mcp/Log.hpp`（`LogLevel`、`LogRecord`、`LogContext`、`SetLogLevel`、`SetLogHandler`、`LogWrite`）
- `include/mcp/SpanHooks.hpp`（`SpanKind`、`SpanPhase`、`SpanEvent`、`SpanHandler`）
- `include/mcp/server/ServerOptions.hpp`（`span_handler`、`declare_logging`）
- `include/mcp/client/ClientOptions.hpp`（`span_handler`、`on_transport_close`、`on_transport_error`）
- `include/mcp/transport/detail/PlatformIO.hpp`（`Read`/`IsEof` 语义）
- `include/mcp/protocol/McpSessionHandler.hpp`（请求处理器与 promise 交接）
- `src/protocol/McpSessionHandler.cpp`、`src/server/McpServer.cpp`、`src/transport/StdioClientTransport.cpp`
- `scripts/run-conformance.sh`、`tests/conformance/`
- `examples/RequestCancellation/main.cpp`
