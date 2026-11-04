# 管住子进程

用 stdio 传输连本地 MCP 服务端时，你其实启动了一个进程，就得负责它的输入输出、终止与回收。这一页讲两件事：**`mcp::Spawn` 能做什么**（与协议无关的通用子进程）、**stdio 传输自己做了什么**（以及它不听 `SpawnOptions` 的话）。传输选型见 [/zh/learn/transports](/zh/learn/transports)。

::: info 两条路径不是一条
stdio 传输走内部平台层创建进程，**不经过** `mcp::Spawn`；两者共用底层平台辅助，但入口与选项各自独立。别指望往 `SpawnOptions` 里填字段能影响 stdio 传输的行为，也别指望 stdio 传输帮你管非 MCP 的进程——那种活儿直接用 `Spawn`。
:::

## 前置条件

- 头文件：`mcp/process/Process.hpp`（公共 `Spawn`）、`mcp/transport/StdioClientTransport.hpp`、`mcp/Transport.hpp`、`mcp/client/McpClient.hpp`、`mcp/client/ClientOptions.hpp`、`mcp/Implementation.hpp`；include 必须写全子目录路径。
- 平台：Windows 用 Job Object 做树终止，POSIX 用 `posix_spawn` + 独立进程组 + 阻塞回收（无 shell 介入，参数边界原样保留）。
- 无需 CMake 开关；`Spawn` 在 `mcp-core`，stdio 传输在 `mcp-transport`。
- 目标可执行文件必须真实存在于 `working_directory` 解析得到的路径上——`command` 不展开 shell 变量，也不走 PATH 之外的搜索规则之外的魔法。

## 起一个与协议无关的子进程

`SpawnOptions` 八个字段 + `SpawnedProcess` 五个方法（`include/mcp/process/Process.hpp`）：`Pid()`、`IsRunning()`、`Wait(timeout_ms)`、`ExitCode()`、`Terminate(grace_ms)`。`command` 为空直接抛。

<<< @/snippets/manage-subprocesses.cpp#spawn-plain

`Wait` 返回 `false` 表示窗口内没退出，此时 `Terminate(grace_ms)` 先给它 `grace_ms` 温和退出、到点再强杀；`detached = true` 时子进程脱离在启动器退出后继续存活（行为由 `SpawnTest.DetachedChildSurvivesLauncherExit` 守护）。

## stdio 传输的完整选项

`StdioClientTransportOptions` 的实际字段（`include/mcp/transport/StdioClientTransport.hpp`）：

| 字段 | 类型 / 默认 | 作用 |
|---|---|---|
| `command` | `std::string` | 可执行文件 |
| `arguments` | `vector<string>` | 参数原样传递，不过 shell |
| `name` | `""` | 传输名；留空时 `Name()` 回 `"stdio"` |
| `working_directory` | `""` | 子进程工作目录 |
| `inherit_environment_variables` | `true` | 是否继承当前环境 |
| `environment_variables` | `map<string,string>` | 追加/覆盖的变量 |
| `stderr_path` | `""` | stderr 落盘路径 |
| `on_stderr` | `function<void(string_view)>` | stderr 回调 |
| `shutdown_timeout` | `2000ms` | 关闭时给子进程的退出宽限 |
| `terminate_process_tree` | `false` | 是否连带终止子孙进程 |
| `max_frame_bytes` | `kMaxStdioFrameBytes` | 单帧上限 |

<<< @/snippets/manage-subprocesses.cpp#stdio-options

超过 `max_frame_bytes` 的帧会被**丢弃并报错**（缓冲区直接清空后触发错误通知，`src/transport/StdioClientTransport.cpp`），而不是无限增长内存。

## 收 stderr：回调或落盘，二选一

::: danger 同时给会抛
`stderr_path` 与 `on_stderr` 互斥，两个都给时 `Connect()` 直接抛 `McpError(InvalidParams)`，消息是 `stderr_path and on_stderr are mutually exclusive`（`src/transport/StdioClientTransport.cpp`）。
:::

<<< @/snippets/manage-subprocesses.cpp#stderr-drain

<<< @/snippets/manage-subprocesses.cpp#stderr-file

排空跑在**专用线程**上（线程名 `mcp-stderr`），两条实现细节值得知道：

- 读到 0 字节不一定结束：POSIX 侧 `Read` 用 100ms poll 轮询，无数据就返回 0，是否真 EOF 由 `IsEof()` 判定（`include/mcp/transport/detail/PlatformIO.hpp`）。空闲期不会误判断连。
- **回调里的异常不会穿透**：`std::exception` 与未知异常都被捕获并记 Error 日志。你的回调炸了只会丢那一段输出，不会带走读线程。

回调是排空线程直接调用的，因此必须**快**：把内容塞进你自己的队列，别在里面做阻塞 IO 或调 `Close()`。

## 进程树终止与关闭宽限

`terminate_process_tree = true` 时：

- Windows：子进程被挂进 Job Object 并设 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`，终止走 `TerminateJobObject`（`src/process/detail/WindowsProcessSupport.cpp`），因此孙进程也一起没。
- POSIX：子进程自领一个进程组，终止按进程组发信号并阻塞回收，不留僵尸（`src/transport/detail/posix_platform.cpp` 与 `src/process/ProcessPosix.cpp`，行为由 `SpawnTest.PosixChildLeadsOwnProcessGroupAndIsReaped`、`TransportTest.StdioProcessReapedWithoutZombie` 守护）。

`shutdown_timeout` 就是那个「温和退出窗口」：关闭路径直接把它交给进程终止接口（`src/transport/StdioClientTransport.cpp`）。**POSIX 的宽限计时用真实时钟截止**，不是循环次数——macOS 上 10ms 级 sleep 实测约为标称 5 倍，按次数算会把 300ms 拖成约 1.7s（`src/transport/detail/posix_platform.cpp`）。

## 关闭顺序与线程陷阱

<<< @/snippets/manage-subprocesses.cpp#shutdown-order

- 先 `client->Close()` 再释放传输对象；`StdioClientTransport` 对象必须比它 `Connect()` 出来的会话与客户端活得久，`Pid()` 也是从它上面取的（`include/mcp/transport/StdioClientTransport.hpp`）。
- **不要在传输回调里调 `Close()`**：stdio 的读线程直接执行用户回调，回调里 `Close()` 会 join 自己所在的线程。所有 `Close()` 都应发生在别的线程；SDK 内部用 `detail::JoinThreadSafely` 兜底（`src/transport/StdioClientTransport.cpp`）。

## 健康检查与重启

<<< @/snippets/manage-subprocesses.cpp#watchdog

`Pid()` 只在 `Connect()` 之后有值（`std::optional<int64_t>`）。它是从传输对象上取的，所以会话建立与 PID 读取的先后顺序不能颠倒：

<<< @/snippets/manage-subprocesses.cpp#connect-and-pid

它只回答「有没有起过进程」，**不等于进程还活着**——要判活请自己发 `Ping()`（legacy 时代可用；modern 时代已废弃，见 [/zh/howto/handle-server-initiated-requests](/zh/howto/handle-server-initiated-requests)）或观察传输错误回调 `ClientOptions::on_transport_error`。空闲误判的坑在上一节，`on_transport_close` 与 `on_transport_error` 的语义见 [/zh/howto/connect-to-a-server](/zh/howto/connect-to-a-server)。

## 完整代码

<<< @/snippets/manage-subprocesses.cpp#full

`StartEchoServer` 的输出参数是刻意的：调用方必须持有 `shared_ptr<StdioClientTransport>` 直到客户端关闭完毕，否则传输先析构，会话就成悬垂。

## 验证

片段只做语法校验（clang-cl 走 `/Zs`，GCC/Clang 走 `-fsyntax-only`）：

```bash
cmake -S . -B build/p3a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3a --target mcp-doc-snippet-manage-subprocesses
```

期望：configure 打印 `[mcp-docs] syntax-checking 16 snippet(s) with '/Zs'`，构建打印一行 `Building CXX object ...manage-subprocesses.cpp.obj`，返回码 0 且无 `error:` 行。

行为侧由既有用例守护，需先 `git submodule update --init third_party/googletest`，再 `cmake --preset debug && cmake --build --preset debug`，然后：

```bash
ctest --preset debug -R 'SpawnTest|TransportTest.Stdio'
```

覆盖点：参数边界与不经过 shell、环境继承与覆盖、工作目录、双流重定向与截断、`Wait` 超时后取退出码、非零退出码、树终止、detached 存活、空命令被拒、多线程并发启动、POSIX 进程组与回收、stderr 捕获与回调、`Pid()` 可用、并发连接、关闭宽限、无僵尸回收。

## 时代差异

进程生命周期是传输层以下的事，与协议时代无关：

| 项目 | legacy（≤ 2025-11-25） | modern（2026-07-28） |
|---|---|---|
| stdio 建进程 / 终止 / 回收 | 完全一致 | 完全一致 |
| `stderr_path` 与 `on_stderr` 互斥 | 一致 | 一致 |
| 取消通知的传输形态 | stdio 走 `notifications/cancelled` | stdio 仍走通知（只有实现 `IRequestCanceller` 的传输才关流），见 [/zh/howto/cancel-a-request](/zh/howto/cancel-a-request) |

## 常见错误

| 对外表现 | 成因 | 处置 |
|---|---|---|
| `Connect()` 抛 `InvalidParams`，提示两者互斥 | 同时设了 `stderr_path` 与 `on_stderr` | 二选一：要落盘就别给回调 |
| 空闲一段时间后被判定「子进程退出」 | 自己写的读循环把 `Read()` 返回 0 当 EOF | 必须配合 `IsEof()` 判定 |
| 子进程还在但连接已不可用 | 只盯 `Pid()` 判活 | 用协议层探活或看传输错误回调 |
| 关掉客户端后进程树里还留着孙进程 | `terminate_process_tree` 默认 `false` | 需要树终止就显式打开 |
| 回调抛异常导致输出丢了一半 | 回调异常被捕获并记日志，不重放 | 回调里别做可失败的重活，异常自己吃掉 |
| `Close()` 在自己的 stderr/读回调里卡死 | IO 线程 join 自身 | 把关闭动作投递到别的线程 |
| 想给 stdio 传 `SpawnOptions` | 两者不是同一条路径 | stdio 用 `StdioClientTransportOptions`；通用进程用 `Spawn` |
| 启动慢：macOS 上关闭要 1.7s | 旧实现按循环次数估宽限 | 已改为真实时钟截止；把 `shutdown_timeout` 设小即可 |

## 下一步

- 传输选型与 stateful/stateless：[/zh/learn/transports](/zh/learn/transports)
- 连上去之后的收尾与关闭时机：[/zh/howto/connect-to-a-server](/zh/howto/connect-to-a-server)
- 服务端侧的 stdio 入口：[/zh/howto/run-a-streamable-http-server](/zh/howto/run-a-streamable-http-server) 的同类装配章节
- 字段全表：[/zh/reference/transports](/zh/reference/transports)、[/zh/reference/process-and-storage](/zh/reference/process-and-storage)

## 本页对应源码

- `include/mcp/process/Process.hpp`（`SpawnOptions`、`SpawnedProcess`、`Spawn`）
- `include/mcp/transport/StdioClientTransport.hpp`（`StdioClientTransportOptions`、`Pid()`）
- `include/mcp/transport/detail/PlatformIO.hpp`（内部进程/管道抽象与 `IsEof()`）
- `src/transport/StdioClientTransport.cpp`（互斥校验、stderr 排空线程、帧上限、关闭顺序）
- `src/process/detail/WindowsProcessSupport.cpp`（Job Object 树终止）
- `src/process/ProcessPosix.cpp`（`posix_spawn`、进程组、回收）
- `src/transport/detail/posix_platform.cpp`（关闭宽限的真实时钟截止）
- `tests/unit/ProcessTests.cpp`、`tests/unit/TransportTests.cpp`
