---
type: Transport
title: Stdio 传输
description: 服务端（stdin/stdout 管道）+ 客户端（子进程）双向传输，'\n' 分隔的 JSON-RPC 行。
tags: [transport, stdio, 管道, 子进程]
timestamp: 2026-08-13T12:36:14+08:00
resource: src/transport/StdioServerTransport.cpp
---

# Stdio 传输

服务端（[StdioServerTransport.cpp](../../src/transport/StdioServerTransport.cpp)）与客户端（[StdioClientTransport.cpp](../../src/transport/StdioClientTransport.cpp)）共享同一套读循环模板：`\n` 切行 + 缓冲超限防护 + `IsEof()` 判停。

## 服务端

- 无参构造；`Start()` 幂等（`running_.exchange(true)`），`OpenStandardInput/Output()` 打开管道，启动读线程
- `Close()`：`running_.exchange(false)` 块内关管道（解除读线程阻塞）→ **无条件** `JoinThreadSafely(read_thread_)`（读线程可能已自行退出，Close 兜底 join）→ 关通道 → `SetDisconnected`
- 发送：`SerializeMessage(message) + "\n"` 写 stdout；写字节数不符 → `NotifyError`
- 缓冲超限防护：`buffer.append` 后检查 > `detail::kMaxMessageSize`（8MB，[Limits.hpp](../../include/mcp/transport/detail/Limits.hpp)）→ 清空 + `NotifyError` + 退出读循环；单行超限同样 `NotifyError`（不退出，丢弃该行）
- 读到真 EOF / 读错误 / 超限自行退出时：**不置 `running_`**（留待 Close 完整收尾并 join），关通道 + `SetDisconnected`

## 客户端（工厂）

- 选项：`command / arguments / name / working_directory / inherit_environment_variables(true) / environment_variables`
- `Connect()`：选项转 `ProcessStartInfo` → `CreateProcess` → 构造匿名命名空间会话传输 → 启动即 `SetConnected()`
- `Close()`：`running_.exchange(false)` 非真则提前返回（客户端保留该行为）→ 关 stdin 管道 → `process_->Terminate(5000)` → 关 stdout 管道 → join → 关通道
- 读循环同样有 8MB 缓冲超限防护（清空 + `NotifyError` + break）；EOF 后不置 `running_`（既有行为）
- `Name()` 空时返回 `"stdio"`

## 陷阱

`PipeHandle::Read` 返回 0 **不一定是 EOF**——读循环必须用 `IsEof()` 区分"超时无数据"（继续轮询）与"真 EOF"（退出），否则空闲时被误判为断连。判停语义按平台（[win32_platform.cpp](../../src/transport/detail/win32_platform.cpp) / [posix_platform.cpp](../../src/transport/detail/posix_platform.cpp)，接口见 [PlatformIO.hpp](../../include/mcp/transport/detail/PlatformIO.hpp)）：

- **POSIX**：`poll` 100ms 超时无数据即返回 0；`read` 返回 0 置 `eof_`
- **Win32 已全面 Overlapped 化**：`Win32Pipe` 持有 read/write/cancel 三个事件 + `io_mutex_` + `io_in_flight_` 计数 + `closed_`（atomic）+ `eof_`
  - `Read`：`ReadOverlapped`（`ReadFile(OVERLAPPED)` → `WaitForMultipleObjects({io_event, cancel_event}, 100ms)` → 完成走 `GetOverlappedResult`；超时或取消走 `CancelIoEx` 并等待完成）或 `ReadSync`（`PeekNamedPipe` 轮询，100ms 与 POSIX poll 对齐）
  - `Write` 同构：`WriteOverlapped`（无限等待 + cancel 解除）或 `WriteSync`
  - `Close`：置 `closed_` + `SetEvent(cancel)` + 摘句柄 + `CancelIoEx` + **等 `io_in_flight_ == 0` 才 `CloseHandle`**（消除 CloseHandle 与阻塞 ReadFile 并发 UB）
  - `IsEof()` override：`closed_ || eof_`
- `OpenStandardInput/Output`（Win32）：优先 `CreateFileA("CONIN$"/"CONOUT$", FILE_FLAG_OVERLAPPED)`，失败 `DuplicateHandle` 复制（同步降级），再失败 `INVALID_HANDLE_VALUE`；stderr 直接包装。`CreateProcess` 的管道（`CreatePipe` 句柄）走同步路径（`overlapped_ = false`）

## 相关页面

- [/modules/transport.md](../modules/transport.md) — 所属库
- [/concepts/concurrency.md](../concepts/concurrency.md) — 线程与 Close 语义
- [/classes/message-channel.md](../classes/message-channel.md) — 消息载体
- [/examples/EchoServer](../../examples/EchoServer) — 使用示例（stdio 服务端）
