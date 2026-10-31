---
type: Concept
title: 子进程 Spawn API
description: 公共子进程管理接口：无 shell 参数传递、环境继承与覆盖、工作目录、stdout/stderr 重定向、Wait/ExitCode/Terminate、Windows Job Object 树终止与 POSIX 进程组/回收。
tags: [process, spawn, 子进程, job-object, posix-spawn, 进程组]
timestamp: 2026-10-31T03:49:04+08:00
resource: include/mcp/process/Process.hpp
---

# 子进程 Spawn API

## 公共接口

[Process.hpp](../../include/mcp/process/Process.hpp) 定义 `SpawnOptions`、`SpawnedProcess` 与工厂 `Spawn`，实现随 `mcp-transport` 静态库安装导出（头文件位于 `include/mcp/process/`）。

| 字段 | 语义 |
|------|------|
| `command` | 可执行文件路径或名称；空串抛 `McpError(InvalidParams)`。不经过 shell |
| `arguments` | 独立参数列表；Windows 按 CRT 规则加引号，POSIX 直接进 `argv` |
| `working_directory` | 子进程工作目录；空串继承父进程 |
| `environment` | 环境变量覆盖表；键不得为空或含 `=` |
| `inherit_environment` | `true` 继承后覆盖（同名键大小写不敏感仅 Windows），`false` 仅使用 `environment` |
| `stdout_path` / `stderr_path` | 非空则以 `CREATE_ALWAYS` / `O_TRUNC` 创建并重定向；空则继承父进程标准流 |
| `detached` | Windows 脱离控制台并尝试脱离 Job；POSIX 新 session（可用时）或新进程组 |

`SpawnedProcess` 成员语义：

- `Pid()`：创建后始终可读，退出后仍保留原 PID。
- `IsRunning()`：轮询回收；已退出返回 `false`。
- `Wait(timeout_ms)`：`true` = 已退出并完成回收，`false` = 超时；负值表示无限等待，`0` 为单次轮询。
- `ExitCode()`：运行中为 `nullopt`；退出后为退出码，POSIX 被信号终止时为 `128 + signo`。
- `Terminate(grace_ms)`：先请求优雅退出并等待 `grace_ms`，超时后强杀并等待回收；`grace_ms <= 0` 直接强杀。
- 析构：非 detached 且仍在运行时强杀整棵进程树并回收；detached 保持运行（POSIX 启动后台回收线程避免僵尸，Windows 无僵尸概念）。

## Windows

- **参数引号**：`BuildWindowsCommandLine` 按 MSVC CRT 解析规则处理空格、制表符、引号与反斜杠（引号前反斜杠翻倍、参数末尾反斜杠翻倍）；该函数与 stdio 的 `CreateProcess` 共用，见 [WindowsProcessSupport.cpp](../../src/process/detail/WindowsProcessSupport.cpp)。
- **环境块**：`inherit_environment=true` 时读取 `GetEnvironmentStringsW()`，按大小写不敏感名称过滤被覆盖项后追加覆盖项；保留 `=C:` 形式的驱动器当前目录项；`false` 时构造仅含映射项的环境块（空映射即空环境块）。
- **文件与标准句柄**：重定向文件以可继承句柄打开；未重定向的标准句柄先 `DuplicateHandle(..., TRUE, ...)` 复制为可继承句柄，再以 `STARTF_USESTDHANDLES` 传入。
- **Job Object 树终止**：子进程以 `CREATE_SUSPENDED` 创建，先 `AssignProcessToJobObject` 再 `ResumeThread`，避免子进程在入 Job 前抢先派生后代。非 detached Job 设 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`，`Terminate` 用 `TerminateJobObject` 杀整棵树，析构关闭 Job 也会清理残留后代。所有自建 Job 均设 `JOB_OBJECT_LIMIT_BREAKAWAY_OK`，供 detached 子进程按需脱离。
- **detached**：创建标志为 `DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB`；外部 Job 拒绝 breakaway（`ERROR_ACCESS_DENIED`）时回退为普通创建。detached 自建 Job 不设 kill-on-close，父进程句柄关闭后子进程继续运行。
- **优雅终止**：`GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid)`（子进程为独立进程组）后等待宽限，再 `TerminateJobObject` / `TerminateProcess`。无控制台或子进程不响应 Ctrl-Break 时直接进入强杀路径。
- **限制**：若调用方自身处于禁止 breakaway 且带 kill-on-close 的外部 Job（常见于 CI/容器），操作系统会在该 Job 关闭时连带终止其嵌套的 detached 子进程，用户态无法绕过。

## POSIX

- **启动**：优先 `posix_spawn`（[ProcessPosix.cpp](../../src/process/ProcessPosix.cpp)），不在多线程父进程中 `fork`；工作目录经 `posix_spawn_file_actions_addchdir_np` 设置（glibc ≥ 2.29、macOS），其他 libc 在 `working_directory` 非空时抛 `McpError(InternalError)`。
- **PATH 解析**：命令不含 `/` 时按子进程有效 PATH 逐目录 `access(X_OK)` 解析——`environment["PATH"]` 优先，其次继承的父 PATH，最后 `confstr(_CS_PATH)`；`inherit_environment=false` 时绝不借用父 PATH。
- **环境**：继承模式下按名称精确匹配过滤被覆盖项；`false` 时构造仅含映射项的 `envp`（空映射为空环境）。
- **标准流**：重定向文件在父进程以 `O_WRONLY | O_CREAT | O_TRUNC` 打开，经 `posix_spawn_file_actions_adddup2` 复制到 fd 1/2 并在子进程关闭临时 fd；未重定向的标准流继承父进程。
- **进程组与信号**：非 detached 子进程始终 `POSIX_SPAWN_SETPGROUP(0)` 成为新进程组组长；detached 优先 `POSIX_SPAWN_SETSID`（新 session，glibc/macOS 可用），否则退化为新进程组。子进程 SIGPIPE 复位为默认处置，避免继承 SDK 的忽略设置。
- **回收**：`Wait`/`IsRunning`/`ExitCode` 均用 `waitpid(pid, WNOHANG)` 轮询并缓存退出码；`ECHILD` 视为已退出（退出码未知）。`Terminate` 向进程组发 `SIGTERM`，宽限结束仍存活则 `SIGKILL`；根进程优雅退出后若进程组仍有成员，再补 `SIGKILL` 清理树。
- **析构**：非 detached 向进程组 `SIGKILL` 后阻塞 `waitpid`，不产生僵尸；detached 启动 detached 回收线程 `waitpid(pid)`，父进程退出后由 init 接管回收。

## 复用与 stdio 的关系

Windows 的 UTF-8 转换、命令线引号与环境块构造已抽到 [WindowsProcessSupport.hpp](../../src/process/detail/WindowsProcessSupport.hpp)/[.cpp](../../src/process/detail/WindowsProcessSupport.cpp)，由 `PlatformIO` 的 `CreateProcess` 与公共 `Spawn` 共用；[win32_platform.cpp](../../src/transport/detail/win32_platform.cpp) 行为不变（环境覆盖从“追加重复键”改为“过滤后覆盖”，更符合 Windows 语义）。`StdioClientTransport` 仍走 `ProcessStartInfo`/`CreateProcess` 管道路径，未接入本 API。

## 验证

[ProcessTests.cpp](../../tests/unit/ProcessTests.cpp) 共 14 用例 / 57 断言：参数边界与引号、环境继承/覆盖/隔离、工作目录、stdout/stderr 重定向与截断、退出码、Wait 超时、树终止、detached 存活、多线程并发 Spawn；POSIX 另有进程组、回收与 session 断言。Windows（clang-cl 22 + Debug）与 WSL Ubuntu（glibc 2.43 + clang 21）均实测通过。macOS 未在本机运行验证，代码走同一 POSIX 分支。
