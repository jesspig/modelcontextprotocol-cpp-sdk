---
type: Module
title: mcp-transport 传输库
description: 传输抽象层：ITransport/TransportBase 三态状态机、IClientTransport 连接工厂、各传输实现与 PlatformIO。
tags: [transport, 状态机, 管道, 线程]
timestamp: 2026-08-13T16:30:00+08:00
resource: include/mcp/Transport.hpp
---

# mcp-transport 传输库

依赖 `mcp-core`，网络 I/O 用自研栈（[detail/net/](../../src/transport/detail/net/)：TcpSocket/TlsSocket/HttpClient/WebSocketClient/Sha1）。显式关闭 Unity 构建（匿名命名空间实现类符号冲突）。

## 抽象

- `ITransport` 纯虚接口 5 方法：`SessionId / GetMessageChannel / SendMessageAsync / Close / IsStateless`（默认 false）
- `TransportBase`：三态状态机 `Initial → Connected → Disconnected`（[Transport.hpp](../../include/mcp/Transport.hpp)）
  - `SetConnected` 仅允许 `Initial→Connected`（CAS），已 Disconnected 后调用被忽略并记 Warning
  - `SetDisconnected` 幂等：仅首次触发 `NotifyClose()`
  - `SessionId()` 仅 `StreamableHttpServerTransport` 设置，其余为空串
- `IClientTransport` 工厂：`Name()` + `Connect()`（每次返回新会话传输）

## 传输实现

| 类 | 方向 | 载体 | 页 |
|----|------|------|----|
| StdioServerTransport | 服务端 | stdin/stdout 管道 | [/transports/stdio.md](../transports/stdio.md) |
| StdioClientTransport | 客户端工厂 | 子进程管道 | 同上 |
| InMemoryTransport::CreatePair | 双向 | MessageChannel 对 | [/transports/in-memory.md](../transports/in-memory.md) |
| SseClientTransport | 客户端工厂 | 自研 HttpClient | [/transports/sse.md](../transports/sse.md) |
| WebSocketClientTransport | 客户端工厂 | 自研 WebSocketClient | [/transports/websocket.md](../transports/websocket.md) |
| StreamableHttpServer/ClientTransport | 双向 | 自研 HttpClient / WinHTTP | [/transports/streamable-http.md](../transports/streamable-http.md) |

## PlatformIO（合并 Win32+POSIX）

[PlatformIO.hpp](../../include/mcp/transport/detail/PlatformIO.hpp)：`ProcessHandle / PipeHandle / ProcessStartInfo / CreatedProcess` + 工厂函数 `CreateProcess / OpenStandardInput / SetThreadName`。

关键语义（[posix_platform.cpp](../../src/transport/detail/posix_platform.cpp)）：

- `PipeHandle::Read` 返回 0 **不一定是 EOF**——POSIX 实现 poll 轮询（100ms 超时）无数据即返回 0，必须用 `IsEof()` 区分
- POSIX 子进程：fork + 手动 PATH 搜索 + execve（execvpe 在 macOS/BSD 不可用），stderr 继承父进程；析构 SIGKILL + WNOHANG reap
- Win32：`ArgvToCommandLine` 引号转义、`CREATE_NO_WINDOW`、stderr = 父进程标准错误
- 线程命名：POSIX 限 16 字节；macOS 用单参 `pthread_setname_np`；Win32 动态加载 `SetThreadDescription`

## 默认限制

[Limits.hpp](../../include/mcp/transport/detail/Limits.hpp)：`kMaxMessageSize = 8MB`、`kReadBufferSize = 4096`、`kChannelCapacity = 64`。

## 相关页面

- [/transports/stdio.md](../transports/stdio.md) 等 5 个传输页
- [/classes/message-channel.md](../classes/message-channel.md) — 通道实现
- [/concepts/concurrency.md](../concepts/concurrency.md) — 线程与生命周期
- [/modules/protocol.md](protocol.md) — 上层依赖方
