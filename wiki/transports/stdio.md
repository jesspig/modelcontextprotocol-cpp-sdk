---
type: Transport
title: Stdio 传输
description: 服务端（stdin/stdout 管道）+ 客户端（子进程）双向传输，'\n' 分隔的 JSON-RPC 行。
tags: [transport, stdio, 管道, 子进程]
timestamp: 2026-10-31T20:00:06+08:00
resource: src/transport/StdioServerTransport.cpp
---

# Stdio 传输

服务端（[StdioServerTransport.cpp](../../src/transport/StdioServerTransport.cpp)）与客户端（[StdioClientTransport.cpp](../../src/transport/StdioClientTransport.cpp)）共享同一套读循环模板：`\n` 切行 + 缓冲超限防护 + `IsEof()` 判停。

## 服务端

- 无参构造；`Start()` 幂等（`running_.exchange(true)`），`OpenStandardInput/Output()` 打开管道，启动读线程——**由 `McpServer` 构造自动调用**（`ITransport::Start` 契约，[McpServer.cpp:242](../../src/server/McpServer.cpp)），服务端用户无需手动 Start
- `Close()`：`running_.exchange(false)` 块内关管道（解除读线程阻塞）→ **无条件** `JoinThreadSafely(read_thread_)`（读线程可能已自行退出，Close 兜底 join）→ 关通道 → `SetDisconnected`
- 发送：`SerializeMessage(message) + "\n"` 写 stdout；写字节数不符 → `NotifyError`
- 缓冲超限防护：`buffer.append` 后检查 > `detail::kMaxMessageSize`（8MB，[Limits.hpp](../../include/mcp/transport/detail/Limits.hpp)）→ 清空 + `NotifyError` + 退出读循环；单行超限同样 `NotifyError`（不退出，丢弃该行）
- 读到真 EOF / 读错误 / 超限自行退出时：**不置 `running_`**（留待 Close 完整收尾并 join），关通道 + `SetDisconnected`
- `ReadLoop` 入口 `shared_from_this()` **自持有保活**（[StdioServerTransport.cpp:55](../../src/transport/StdioServerTransport.cpp)）：IO 线程回调内 `Close()` 触发 `JoinThreadSafely` detach 后线程仍存活，成员访问不 UAF

## 客户端（工厂）

- 选项：`command / arguments / name / working_directory / inherit_environment_variables(true) / environment_variables / stderr_path / on_stderr / shutdown_timeout(2000ms) / terminate_process_tree(false) / max_frame_bytes`
- stderr：`stderr_path` 重定向到文件；`on_stderr` 回调（UTF-8 字节块，专用线程持续排空，回调异常只记日志不穿透）；两者互斥，同时设置抛 `McpError(InvalidParams)`；都未设置时继承父进程 stderr
- `terminate_process_tree`：Windows 为子进程建 kill-on-close Job Object（创建失败时告警并退化为直接 `TerminateProcess`），POSIX 在 `fork` 后 `setpgid(0,0)` 并以进程组收发信号；默认关闭，不影响父进程组
- `Pid()`：`Connect()` 成功后返回子进程 PID，未启动返回 `nullopt`；退出后仍保留最后 PID
- `Connect()`：选项转 `ProcessStartInfo` → `CreateProcess` → 构造匿名命名空间会话传输 → 启动读线程与 stderr 线程 → `SetConnected()`；stdio 管道语义不变，与公共 `Spawn` 共用 Windows 命令线/环境块/重定向文件/Job 辅助与 POSIX 重定向文件/进程组信号/回收辅助（见 [/concepts/process-spawn.md](../concepts/process-spawn.md)）
- `Close()`：`running_.exchange(false)` 非真则提前返回（客户端保留该行为）→ 关 stdin 管道 → `process_->Terminate(shutdown_timeout)` → 关 stdout/stderr 管道 → join 读线程与 stderr 线程 → 关通道
- **POSIX 关闭宽限按真实时钟**：`PosixProcess::Terminate` 先 `SIGTERM`，以 `steady_clock` 算出 `shutdown_timeout` 截止点、每 10ms 一次 `waitpid(WNOHANG)` 轮询，到点才升级 `SIGKILL` 并阻塞回收（[posix_platform.cpp](../../src/transport/detail/posix_platform.cpp)）。此前按「迭代次数 × 10ms」估算经过时间，而 macOS 上 10ms 级 `sleep_for` 实测可达标称 5 倍，300ms 宽限被放大到 ~1.7s；公共 `Spawn` 侧 `SpawnedProcess::Wait` 本就用 deadline 判定，无此缺陷
- 读循环同样有 8MB 缓冲超限防护（清空 + `NotifyError` + break）；EOF 后不置 `running_`（既有行为）
- `Name()` 空时返回 `"stdio"`

## 陷阱

`PipeHandle::Read` 返回 0 **不一定是 EOF**——读循环必须用 `IsEof()` 区分"超时无数据"（继续轮询）与"真 EOF"（退出），否则空闲时被误判为断连。判停语义按平台（[win32_platform.cpp](../../src/transport/detail/win32_platform.cpp) / [posix_platform.cpp](../../src/transport/detail/posix_platform.cpp)，接口见 [PlatformIO.hpp](../../include/mcp/transport/detail/PlatformIO.hpp)）：

- **POSIX**：`poll` 100ms 超时无数据即返回 0；`read` 返回 0 置 `eof_`；fork 安全——argv/envp 构造与 PATH 搜索（自定义环境分支）在 `fork()` 前完成（fork 后仅 async-signal-safe 调用），管道 fd 与标准流 dup 设 `FD_CLOEXEC`（[posix_platform.cpp](../../src/transport/detail/posix_platform.cpp)）
- **Win32 已全面 Overlapped 化**：`Win32Pipe` 持有 read/write/cancel 三个事件 + `io_mutex_` + `io_in_flight_` 计数 + `closed_`（atomic）+ `eof_`
  - `Read`：`ReadOverlapped`（`ReadFile(OVERLAPPED)` → `WaitForMultipleObjects({io_event, cancel_event}, 100ms)` → 完成走 `GetOverlappedResult`；超时或取消走 `CancelIoEx` 并等待完成）或 `ReadSync`（`PeekNamedPipe` 轮询，100ms 与 POSIX poll 对齐）
  - `Write` 同构：`WriteOverlapped`（无限等待 + cancel 解除）或 `WriteSync`
  - `Close`：置 `closed_` + `SetEvent(cancel)` + 摘句柄 + `CancelIoEx` + **等 `io_in_flight_ == 0` 才 `CloseHandle`**（消除 CloseHandle 与阻塞 ReadFile 并发 UB）
  - `IsEof()` override：`closed_ || eof_`
- `OpenStandardInput/Output`（Win32）：优先 `CreateFileA("CONIN$"/"CONOUT$", FILE_FLAG_OVERLAPPED)`，失败 `DuplicateHandle` 复制（同步降级），再失败 `INVALID_HANDLE_VALUE`；stderr 直接包装。`CreateProcess` 的管道改 `CreateNamedPipeW` + `CreateFileW`（**父端 `FILE_FLAG_OVERLAPPED`**，64KB 缓冲），`WriteSync` 锁外化——`io_mutex_` 只在循环内短暂持锁检查 `closed_`，`WriteFile` 在锁外（原锁内阻塞写满管道会让 `Close()` 死锁等锁）

## 相关页面

- [/modules/transport.md](../modules/transport.md) — 所属库
- [/concepts/process-spawn.md](../concepts/process-spawn.md) — 公共子进程 Spawn 与平台进程语义
- [/concepts/concurrency.md](../concepts/concurrency.md) — 线程与 Close 语义
- [/classes/message-channel.md](../classes/message-channel.md) — 消息载体
- [/examples/EchoServer](../../examples/EchoServer) — 使用示例（stdio 服务端）
