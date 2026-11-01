# 进程与存储参考

本页是公共进程 API、任务/事件/会话存储与 HTTP 服务端底座符号的**唯一权威字段表**，字段、类型、默认值逐条对照头文件与平台实现；stdio 传输的选项字段见 [传输层参考](/zh/reference/transports)，门面签名见 [服务端参考](/zh/reference/server) 与 [客户端参考](/zh/reference/client)，OAuth 与 token 缓存细节见 [授权与扩展参考](/zh/reference/auth-and-extensions)。本页不含操作步骤。

## 公共进程 API

`include/mcp/process/Process.hpp` 只声明三个符号：结构体 `SpawnOptions`、抽象类 `SpawnedProcess`、自由函数 `Spawn`。它是唯一带 `MCP_API` 标记的进程头（全静态库下该宏为空，`include/mcp/Export.hpp:15-16`），实现按平台编入 `mcp-transport`（`CMakeLists.txt:145-156`）。

```cpp
struct MCP_API SpawnOptions { /* 见下表 8 个字段 */ };
class MCP_API SpawnedProcess { /* 纯虚接口，5 个成员函数 */ };
MCP_API std::unique_ptr<SpawnedProcess> Spawn(const SpawnOptions& options);
```

### `SpawnOptions` 逐字段

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `SpawnOptions` | `struct MCP_API SpawnOptions` | — | 平台无关的启动描述，无构造函数、全部公有字段 | `include/mcp/process/Process.hpp:14-23` |
| `command` | `std::string` | 空（必填） | 空则 `Spawn` 抛 `McpErrorCode::InvalidParams`；POSIX 不含 `/` 时先按 PATH 逐项 `access(X_OK)` 解析，Windows 原样交给 `CreateProcessW` 自行搜索 | `Process.hpp:15`；`src/process/ProcessPosix.cpp:41-42`、`77-93` |
| `arguments` | `std::vector<std::string>` | `{}` | Windows 按引号/反斜杠规则拼成单条命令行，POSIX 组装 `argv`，两平台 `argv[0]`/命令位均为 `command` | `Process.hpp:16`；`src/process/detail/WindowsProcessSupport.cpp:24-54`；`ProcessPosix.cpp:235-242` |
| `working_directory` | `std::string` | 空（继承父进程） | Windows 转宽字符传 `lpCurrentDirectory`；POSIX 依赖 `posix_spawn_file_actions_addchdir_np`（macOS 或 glibc ≥ 2.29），不支持时非空值直接抛 `InternalError` | `Process.hpp:17`；`src/process/ProcessWin32.cpp:162-164`；`ProcessPosix.cpp:26-32`、`317-327` |
| `environment` | `std::map<std::string, std::string>` | `{}` | 键为空或含 `=` 抛 `InvalidParams`；POSIX 按精确键名覆盖，Windows 覆盖匹配**不区分大小写** | `Process.hpp:18`；`ProcessWin32.cpp:19-23`；`ProcessPosix.cpp:43-47`；`WindowsProcessSupport.cpp:56-92` |
| `inherit_environment` | `bool` | `true` | 为真时并入父进程环境；POSIX 无任何覆盖时直接把继承的 `environ` 交给 `posix_spawn` | `Process.hpp:19`；`ProcessPosix.cpp:95-109`、`246-253`；`ProcessWin32.cpp:166-170` |
| `stdout_path` | `std::string` | 空（继承父 stdout） | 非空则**截断重建**（POSIX `O_TRUNC` + `0644`，Windows `CREATE_ALWAYS` 只写），仅重定向 stdout | `Process.hpp:20`；`ProcessWin32.cpp:180-185`；`ProcessPosix.cpp:260`；`src/process/detail/PosixProcessSupport.cpp:14-24` |
| `stderr_path` | `std::string` | 空（继承父 stderr） | 同上，仅重定向 stderr；无 `stdout_path` 的“追加”变体 | `Process.hpp:21`；`ProcessWin32.cpp:186-191`；`ProcessPosix.cpp:261` |
| `detached` | `bool` | `false` | Windows 追加 `DETACHED_PROCESS` 与 `CREATE_BREAKAWAY_FROM_JOB`（后者被拒时去掉重试），且作业对象不带 `KILL_ON_JOB_CLOSE`；POSIX 有 `POSIX_SPAWN_SETSID` 时新建 session，否则仍新建进程组 | `Process.hpp:22`；`ProcessWin32.cpp:206-220`、`231`；`ProcessPosix.cpp:335-345`；`WindowsProcessSupport.cpp:108-122` |

### `SpawnedProcess` 成员

`SpawnedProcess` 是抽象类，实际对象由平台内部实现类构造（Windows `WindowsSpawnedProcess`、POSIX `PosixSpawnedProcess`，均在匿名 namespace，不属于用户可见类型）。

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `~SpawnedProcess` | `virtual ~SpawnedProcess() = default` | — | 虚析构；`Spawn` 返回 `std::unique_ptr<SpawnedProcess>`，析构行为按平台不同（见下两节） | `Process.hpp:27`、`36` |
| `Pid` | `virtual int64_t Pid() const = 0` | 构造时定格 | 子进程 PID，恒定返回、不判定退出 | `Process.hpp:29`；`ProcessWin32.cpp:67`；`ProcessPosix.cpp:152` |
| `IsRunning` | `virtual bool IsRunning() const = 0` | — | Windows 用 `WaitForSingleObject(0)` 顺带记录退出码，返回值非 `WAIT_TIMEOUT`/`WAIT_OBJECT_0` 时抛 `InternalError`；POSIX 用非阻塞 `waitpid(WNOHANG)` 回收 | `Process.hpp:30`；`ProcessWin32.cpp:69-81`；`ProcessPosix.cpp:154-159` |
| `Wait` | `virtual bool Wait(int timeout_ms) = 0` | — | `timeout_ms < 0` 为无限等待（Windows `INFINITE`，POSIX 10 ms 轮询）；已退出立即 `true`，超时返回 `false` | `Process.hpp:31`；`ProcessWin32.cpp:83-102`；`ProcessPosix.cpp:38`、`161-180` |
| `ExitCode` | `virtual std::optional<int> ExitCode() const = 0` | `std::nullopt`（未退出） | 未退出时顺带做一次零等待探测；POSIX 下被信号杀死记 `128 + signum`，`ECHILD` 视为已退出但无码 | `Process.hpp:32`；`ProcessWin32.cpp:104-115`；`ProcessPosix.cpp:182-186`；`PosixProcessSupport.cpp:41-61` |
| `Terminate` | `virtual bool Terminate(int grace_ms) = 0` | 恒 `true`（除非抛出） | 先宽限后强杀的平台序列见下两节；`grace_ms <= 0` 跳过宽限直接强杀 | `Process.hpp:33`；`ProcessWin32.cpp:117-130`；`ProcessPosix.cpp:188-208` |

### `Spawn` 与错误语义

| 项 | 结论 | 证据 |
|---|---|---|
| 签名 | `std::unique_ptr<SpawnedProcess> Spawn(const SpawnOptions& options)`，`options` 常量引用，返回所有权 | `include/mcp/process/Process.hpp:36` |
| 参数校验 | 两平台同一条 `ValidateOptions`：`command` 为空 → `McpError(McpErrorCode::InvalidParams, "spawn command must not be empty")`；环境键空或含 `=` → 同码 `"spawn environment key must be non-empty and must not contain '='"` | `ProcessWin32.cpp:16-24`；`ProcessPosix.cpp:40-48` |
| 平台失败 | 一律 `McpError(McpErrorCode::InternalError, ...)`：Windows 文案带 `Windows error <N>`，POSIX 带 `std::strerror` 文本 | `ProcessWin32.cpp:226-229`、`249-251`；`ProcessPosix.cpp:273-275`、`296-301`、`351-354` |
| 重定向打开失败 | 底层抛 `std::runtime_error`，`Spawn` 收敛为 `McpError(InternalError, what())`，并关闭已打开的描述符/句柄 | `ProcessWin32.cpp:26-32`、`192-197`；`ProcessPosix.cpp:111-117`、`262-266` |
| 生产调用者 | **无**。全仓 `Spawn` 只出现在 `include/mcp/process/Process.hpp`、`src/process/ProcessWin32.cpp`、`src/process/ProcessPosix.cpp` 与 `tests/unit/ProcessTests.cpp`、`tests/unit/ProcessTestHelper.cpp` | `include/mcp/process/Process.hpp:36`；`tests/unit/ProcessTests.cpp:1` |

## 平台实现差异

### Windows：`ProcessWin32.cpp`

| 行为点 | 实现与语义 | 证据 |
|---|---|---|
| 创建入口 | `CreateProcessW`，`bInheritHandles = TRUE`，`dwFlags` 恒含 `CREATE_UNICODE_ENVIRONMENT`、`CREATE_SUSPENDED`、`CREATE_NEW_PROCESS_GROUP` | `src/process/ProcessWin32.cpp:206-212` |
| 标准句柄 | 父进程三个标准句柄经 `DuplicateHandle(..., DUPLICATE_SAME_ACCESS)` 复制并置继承位，填 `STARTF_USESTDHANDLES`；`stdout_path`/`stderr_path` 非空时改为文件句柄 | `ProcessWin32.cpp:34-45`、`172-204` |
| 挂起—入作业—恢复 | 子进程先以挂起态创建，随后建作业对象并 `AssignProcessToJobObject`，最后 `ResumeThread`；恢复失败即 `TerminateProcess` + 无限等待 + 关闭全部句柄并抛 `InternalError` | `ProcessWin32.cpp:231-253` |
| 作业对象 | 恒尝试创建：`kill_on_close = !detached`、`breakaway_ok = true`；创建或赋值失败只记 Warning 并退化为无作业对象（不抛） | `ProcessWin32.cpp:231-240`；`src/process/detail/WindowsProcessSupport.cpp:108-126` |
| 环境块 | UTF-8 → UTF-16 转换使用 `MB_ERR_INVALID_CHARS`（非法 UTF-8 抛错）；块以两个 `L'\0'` 收尾并配 `CREATE_UNICODE_ENVIRONMENT` | `WindowsProcessSupport.cpp:11-22`、`56-92`；`ProcessWin32.cpp:166-170`、`206` |
| `Terminate` 序列 | `grace_ms > 0` 时先 `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid)` 再 `Wait(grace_ms)`；宽限内退出则补一次 `TerminateJobObject(job, 1)` 收尾；否则 `TerminateWindowsJobOrProcess`（有作业对象 `TerminateJobObject`，无则 `TerminateProcess`）后 `Wait(-1)` | `ProcessWin32.cpp:117-130`；`WindowsProcessSupport.cpp:128-131` |
| 析构 | 非 `detached`：未退出则强杀 + 无限等待 + 记录退出码；`detached`：什么都不做，子进程存活；两种情况都关闭作业与进程句柄 | `ProcessWin32.cpp:52-65` |
| 回收 | 无需 `waitpid`：退出码来自 `GetExitCodeProcess`，`IsRunning`/`Wait`/`ExitCode` 任一次探测到 `WAIT_OBJECT_0` 即定格 `exited_` | `ProcessWin32.cpp:133-141` |

### POSIX：`ProcessPosix.cpp`

| 行为点 | 实现与语义 | 证据 |
|---|---|---|
| 创建入口 | `posix_spawn`（非 `fork`/`exec`），`argv` 末位 `nullptr`；命令解析用自实现 PATH 扫描（`argv[0]` 仍为原始 `command`） | `src/process/ProcessPosix.cpp:235-242`、`347-354`、`77-93` |
| 进程组/会话 | 非 `detached`：`POSIX_SPAWN_SETPGROUP` + `posix_spawnattr_setpgroup(0)` → 子进程自成组长（组号等于自身 PID）；`detached` 且有 `POSIX_SPAWN_SETSID` → 新建 session | `ProcessPosix.cpp:335-345` |
| 信号默认值 | `posix_spawnattr_setsigdefault` 把 `SIGPIPE` 恢复为默认处置，避免子进程继承忽略态 | `ProcessPosix.cpp:329-335` |
| 重定向 | 通过 `adddup2` + `addclose` 文件动作把 `stdout`/`stderr` 指向 `O_WRONLY\|O_CREAT\|O_TRUNC`（`0644`、`O_CLOEXEC` 可用时加）的文件；stdin 一律继承 | `ProcessPosix.cpp:268-316`；`src/process/detail/PosixProcessSupport.cpp:14-24` |
| 工作目录 | 需 `posix_spawn_file_actions_addchdir_np`：macOS 或 glibc ≥ 2.29 才编译进该分支，否则非空 `working_directory` 抛 `InternalError`（宏 `MCP_PROCESS_HAS_SPAWN_CHDIR`） | `ProcessPosix.cpp:26-32`、`317-327` |
| `Terminate` 序列 | `grace_ms > 0` 时先对进程组 `SIGTERM` 再 `Wait(grace_ms)`；宽限内退出后若 `kill(-pid, 0)` 判定组内仍存活则补 `SIGKILL`；否则 `SIGKILL` 组后 `Wait(-1)` | `ProcessPosix.cpp:188-208`；`PosixProcessSupport.cpp:30-39` |
| 进程组信号语义 | `SignalPosixProcessGroup` 用 `kill(-pid, sig)`，返回 `ESRCH` 时回退单进程 `kill(pid, sig)` | `PosixProcessSupport.cpp:30-34` |
| 回收 | `waitpid(pid, WNOHANG)` 非阻塞回收；`EINTR` 重试，`ECHILD` 视为已退出（无码），被信号杀死折算 `128 + signum`；`Wait` 用 10 ms 轮询而非条件变量 | `PosixProcessSupport.cpp:41-61`；`ProcessPosix.cpp:38`、`161-180`、`211-221` |
| 析构 | 非 `detached`：`SIGKILL` 进程组后阻塞回收；`detached`：另起一条 `detach()` 线程阻塞回收，避免析构者被 `waitpid` 卡住；两者都吞掉回收异常 | `ProcessPosix.cpp:135-150` |

## 与 stdio 传输的边界

::: warning
`StdioClientTransport` **不经过**公共 `Spawn`。它只 include `mcp/transport/detail/PlatformIO.hpp` 并调用内部 `CreateProcess`（参数为 `mcp::detail` 内的 `ProcessStartInfo`，`src/transport/StdioClientTransport.cpp:4`、`194-206`）。这些内部符号（进程句柄、管道句柄、启动信息结构）不是用户 API，不得当接口引用；本节只说明行为差异。
:::

| 维度 | 公共 `Spawn` | stdio 传输内部路径 | 证据 |
|---|---|---|---|
| 声明位置 | `include/mcp/process/Process.hpp`，带 `MCP_API` | `include/mcp/transport/detail/PlatformIO.hpp`（`detail/` 内部头，未导出） | `Process.hpp:14-36`；`PlatformIO.hpp:26-44` |
| 标准流 | 复用父进程句柄/fd，或把 stdout/stderr 落盘，**没有管道回读通道** | `pipe()`/匿名管道双向建链，父侧持有 stdin 写端与 stdout/stderr 读端 | `ProcessWin32.cpp:172-204`；`ProcessPosix.cpp:268-316`；`src/transport/detail/posix_platform.cpp:221-250` |
| 底层创建 | Windows `CreateProcessW`（挂起 + 作业 + 恢复）；POSIX `posix_spawn` | Windows `CreateProcessW` + `CREATE_NO_WINDOW`，仅 `terminate_process_tree` 时才挂起建作业；POSIX `fork` + `execvp`，条件成立才 `setpgid(0,0)` | `ProcessWin32.cpp:206-253`；`win32_platform.cpp:510-511`、`534-536`；`posix_platform.cpp:319`、`350`、`360` |
| 终止宽限 | `Terminate(grace_ms)`：Windows `CTRL_BREAK_EVENT`、POSIX 进程组 `SIGTERM` | 内部句柄的 `Terminate(timeout_ms)`：Windows 直接作业/进程强杀，POSIX 单进程 `SIGTERM`（`terminate_process_tree` 为真才发进程组）后超时 `SIGKILL` | `ProcessWin32.cpp:117-130`；`ProcessPosix.cpp:188-208`；`win32_platform.cpp:433-437`；`posix_platform.cpp:159-161`、`179-191` |
| 作业对象 | 恒尝试（`detached` 时不带 kill-on-close） | 仅 `terminate_process_tree = true` 时创建，`kill_on_close = true`、`breakaway_ok = false` | `ProcessWin32.cpp:231`；`win32_platform.cpp:534-541` |
| 共用底座 | 两条路径**共用**同一批底层平台辅助：`src/process/detail/PosixProcessSupport.*` 与 `WindowsProcessSupport.*`（重定向打开、进程组信号、`waitpid` 回收、命令行拼装、环境块、作业对象） | 同左 | `ProcessPosix.cpp:3`；`ProcessWin32.cpp:4`；`posix_platform.cpp:4`；`win32_platform.cpp:5` |
| 归属与调用面 | 编入 `mcp-transport`，生产代码零调用者，仅 `tests/unit/ProcessTests.cpp` 与 `ProcessTestHelper.cpp` 覆盖 | 由 `StdioClientTransport::Connect()` 调用，并受 `shutdown_timeout`、`terminate_process_tree` 影响 | `CMakeLists.txt:145-156`；`src/transport/StdioClientTransport.cpp:187-206`、`68-69` |

## 任务存储

### `TaskStatus` 与 `TaskState`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `TaskStatus` | `enum class { Pending, Working, Completed, Failed, Cancelled, InputRequired }` | 记录初始 `Pending` | 终态为 `Completed`/`Failed`/`Cancelled`，进入终态后写操作返回 `false`；落盘为整数 | `include/mcp/server/McpTaskStore.hpp:16-23`；`src/server/FileTaskStore.cpp:39-42`、`49` |
| `TaskState` | `struct` | — | 任务记录全集；`FileTaskStore` 逐字段 JSON 化 | `McpTaskStore.hpp:25-43` |
| `task_id` | `std::string` | 空 | 由 `CreateTask` 参数写入 | `McpTaskStore.hpp:26` |
| `owner` | `std::string` | 空 | 会话归属，`SetTaskOwner` 可改写 | `McpTaskStore.hpp:27` |
| `status` | `TaskStatus` | `Pending` | 唯一状态字段 | `McpTaskStore.hpp:28` |
| `result` | `std::optional<JsonValue>` | 无 | `UpdateTask` 写入即为完成结果；`FailTask` 把错误载荷也写进该字段 | `McpTaskStore.hpp:29`；`FileTaskStore.cpp:304`、`389` |
| `error_message` | `std::optional<std::string>` | 无 | `CancelTask` 的 reason、`FailTask` 载荷里的 `message` 字段落此 | `McpTaskStore.hpp:30`；`FileTaskStore.cpp:334`、`391-393` |
| `input_required` | `std::optional<JsonValue>` | 无 | MRTR 停机时的载荷快照（与 `input_requests` 并存，语义由上层决定） | `McpTaskStore.hpp:31` |
| `progress` | `double` | `0` | 无可选包装，恒序列化 | `McpTaskStore.hpp:32`；`FileTaskStore.cpp:53` |
| `progress_total` | `std::optional<double>` | 无 | 可选总量 | `McpTaskStore.hpp:33` |
| `created_at` | `std::string` | 空→建时定格 | 存的是毫秒时间戳**字符串**，TTL 判定靠 `ParseMillis` 解析，解析失败则永不过期 | `McpTaskStore.hpp:34`；`FileTaskStore.cpp:21-26`、`27-37`、`471-479` |
| `status_message` | `std::optional<std::string>` | 无 | 状态附言 | `McpTaskStore.hpp:35` |
| `last_updated_at` | `std::optional<std::string>` | 无 | 每次成功写入刷新为当前毫秒 | `McpTaskStore.hpp:36`；`FileTaskStore.cpp:235` |
| `ttl_ms` | `std::optional<int64_t>` | 无 | `<= 0` 视为不过期 | `McpTaskStore.hpp:37`；`FileTaskStore.cpp:475` |
| `poll_interval_ms` | `std::optional<int64_t>` | 无 | 建议轮询间隔，纯载荷 | `McpTaskStore.hpp:38` |
| `input_requests` | `std::optional<InputRequests>` | 无 | `ParkTaskForInput` 写入 | `McpTaskStore.hpp:39`；`FileTaskStore.cpp:533-534` |
| `input_responses` | `std::optional<JsonValue>` | 无 | `AdvanceTaskWithInput` 逐键合并已有对象 | `McpTaskStore.hpp:40`；`FileTaskStore.cpp:497-503` |
| `tool_name` / `tool_arguments` | `std::optional<std::string>` / `std::optional<JsonValue>` | 无 | `BindTaskTool` 写入；JSON 键名为 `toolName`/`toolArguments` | `McpTaskStore.hpp:41-42`；`FileTaskStore.cpp:562-563`、`62-63` |

### `IMcpTaskStore` 虚函数

该类**不带** `MCP_API`，纯 C++ 抽象；带函数体的虚函数是「可选能力」的默认实现，未覆写即返回 `false` 或空集合。

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `~IMcpTaskStore` | `virtual ~IMcpTaskStore() = default` | — | 虚析构 | `include/mcp/server/McpTaskStore.hpp:47` |
| `CreateTask` | `virtual TaskState CreateTask(const std::string& task_id) = 0` | — | 唯一纯虚的创建入口 | `McpTaskStore.hpp:49` |
| `CreateTask`（重载） | `virtual TaskState CreateTask(task_id, owner, std::optional<int64_t> ttl_ms)` | 非纯虚 | 模板方法：转调单参版后按 `owner` 非空、`ttl_ms` 有值分别调 `SetTaskOwner`/`SetTaskTtl` | `McpTaskStore.hpp:50-58` |
| `GetTask` | `virtual std::optional<TaskState> GetTask(const std::string&) = 0` | — | 按 id 取快照 | `McpTaskStore.hpp:59` |
| `UpdateTask` | `virtual bool UpdateTask(const std::string&, const std::optional<JsonValue>& result) = 0` | — | `result` 有值→`Completed`，为空→`Working` | `McpTaskStore.hpp:60-61`；`FileTaskStore.cpp:304` |
| `CancelTask` | `virtual bool CancelTask(const std::string&, const std::optional<std::string>& reason) = 0` | — | 置 `Cancelled`，reason 入 `error_message` | `McpTaskStore.hpp:62-63`；`FileTaskStore.cpp:330-334` |
| `SetTaskStatus` | `virtual bool SetTaskStatus(const std::string&, TaskStatus) = 0` | — | 裸状态写入；终态改非终态被拒 | `McpTaskStore.hpp:64`；`FileTaskStore.cpp:355-358` |
| `AdvanceTaskWithInput` | `virtual bool AdvanceTaskWithInput(const std::string&, const JsonValue&)` | `false` | MRTR 推进，仅 `InputRequired` 可推进 | `McpTaskStore.hpp:65`；`FileTaskStore.cpp:493-506` |
| `ParkTaskForInput` | `virtual bool ParkTaskForInput(const std::string&, const InputRequests&)` | `false` | MRTR 停机 | `McpTaskStore.hpp:66`；`FileTaskStore.cpp:529-534` |
| `BindTaskTool` | `virtual bool BindTaskTool(const std::string&, const std::string&, const std::optional<JsonValue>&)` | `false` | 记录续跑所需的工具与参数 | `McpTaskStore.hpp:67-68`；`FileTaskStore.cpp:559-563` |
| `GetAllTasks` | `virtual std::vector<TaskState> GetAllTasks() = 0` | — | 全量枚举（无序） | `McpTaskStore.hpp:69` |
| `FailTask` | `virtual bool FailTask(const std::string&, const JsonValue& error_payload)` | `false` | 置 `Failed`，载荷入 `result` | `McpTaskStore.hpp:70-75`；`FileTaskStore.cpp:385-392` |
| `RemoveTask` | `virtual bool RemoveTask(const std::string&)` | `false` | 物理删除，与 `CancelTask` 不同 | `McpTaskStore.hpp:76-80` |
| `GetExpiredTaskIds` | `virtual std::vector<std::string> GetExpiredTaskIds(int64_t now_ms)` | `{}` | 只枚举不删除 | `McpTaskStore.hpp:81-85`；`FileTaskStore.cpp:471-481` |
| `SetTaskOwner` | `virtual bool SetTaskOwner(const std::string&, const std::string&)` | `false` | 归属改写 | `McpTaskStore.hpp:86-91` |
| `SetTaskTtl` | `virtual bool SetTaskTtl(const std::string&, int64_t ttl_ms)` | `false` | TTL 改写 | `McpTaskStore.hpp:92-97` |

接入点：`ServerOptions` 的 `task_store` 字段持有 `std::shared_ptr<IMcpTaskStore>`（`include/mcp/server/ServerOptions.hpp:78`），`McpServer::SweepExpiredTasks` 以 `GetExpiredTaskIds` + `RemoveTask` 组合作为唯一回收路径，两者抛出的异常都只记 Error 日志（`include/mcp/server/McpServer.hpp:179`；`src/server/McpServer.cpp:1070-1088`）。

### `FileTaskStore`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| 构造 | `explicit FileTaskStore(std::filesystem::path storage_path)` | — | 参数是**单个日志文件路径**（非目录），构造即加载；文件不存在视为空库 | `include/mcp/storage/FileTaskStore.hpp:16`；`src/server/FileTaskStore.cpp:117-121`、`139-143` |
| 析构 | `~FileTaskStore() override` | — | 析构内 `Flush()` → 全量原子重写 | `FileTaskStore.hpp:17`；`FileTaskStore.cpp:123-125`、`586-589` |
| 15 个覆写 | 与 `IMcpTaskStore` 同名同签名，全部 `override` | — | 无默认实现残留：含 `AdvanceTaskWithInput`/`ParkTaskForInput`/`BindTaskTool`/`FailTask`/`RemoveTask`/`GetExpiredTaskIds`/`SetTaskOwner`/`SetTaskTtl` | `FileTaskStore.hpp:19-41` |
| 写入语义 | 更新类函数返回 `bool` | `false` | id 不存在 → `false`；已终态 → `false`（`SetTaskOwner`/`SetTaskTtl`/`BindTaskTool` 无终态守卫）；落盘失败 → 回滚内存并抛 `McpError(InternalError)` | `FileTaskStore.cpp:300-301`、`313-316`、`329-330`、`355-356`、`385-386` |
| 创建语义 | `CreateTask` | — | 重复 id 抛 `McpError(McpErrorCode::InvalidParams, "task already exists: " + id)` | `FileTaskStore.cpp:239-241`、`269-271` |
| 并发模型 | `write_mutex_` 串行写 + `shared_mutex` 读写锁 | — | 读函数只持读锁；写函数全程持 `write_mutex_` | `FileTaskStore.hpp:52-53`；`FileTaskStore.cpp:283-288`、`294` |
| 日志格式 | 每行一条 JSON：`{"task_id":...,"state":{...}}`；删除追加 `{"task_id":...,"removed":true}` | — | 追加写，重放时按行覆盖/删除 | `FileTaskStore.cpp:101-114`、`213-216`、`591-633` |
| 压缩触发 | `pending_records_ > kMinCompactThreshold + 2 × 存活任务数` | `kMinCompactThreshold = 64` | 满足即在追加后重写全文件：临时文件落盘 + `fsync`/`FlushFileBuffers` + `rename` 原子替换（内部辅助，`src/detail/AtomicJsonFile.cpp:49-92`），计数归零 | `FileTaskStore.cpp:19`、`611-614`、`635-650` |
| 损坏处理 | 中间行解析失败 → 复制同名 `.corrupt` 备份并**清空整库**；末行残片只记日志忽略 | — | 兼容旧的 `{"tasks":{...}}` 快照格式 | `FileTaskStore.cpp:127-137`、`187-196`、`201-205` |
| 归属目标 | `mcp-server` 静态库 | — | 示例用法见 `examples/conformance/server/SkillsAndTasks.cpp:456` | `CMakeLists.txt:172-175` |

## 事件与会话存储

### `EventStore`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `EventStore` | `class EventStore`（虚析构） | — | 可覆写的进程内基类，非纯虚；无 `MCP_API` | `include/mcp/http/EventStore.hpp:15-18` |
| `Append` | `virtual uint64_t Append(std::string_view session_id, std::string event_data)` | — | 全局单调递增 id 自 `1` 起（跨会话共用一个计数器），`event_data` 按值移入 | `EventStore.hpp:20`；`src/http/EventStore.cpp:8-22`、`37` |
| `GetEventsSince` | `virtual std::vector<std::pair<uint64_t, std::string>> GetEventsSince(std::string_view, uint64_t last_event_id) const` | 空 | 返回 `id > last_event_id` 的全部事件，按序 | `EventStore.hpp:22-23`；`EventStore.cpp:24-37` |
| `Clear` | `virtual void Clear(std::string_view session_id)` | — | 整会话删除，不回收 id | `EventStore.hpp:25`；`EventStore.cpp:39-42` |
| `kMaxEventsPerSession` | `static constexpr size_t` | `10000` | 超限从头裁剪 | `EventStore.hpp:27`；`EventStore.cpp:16-20` |
| 接入点 | `StreamableHttpServerOptions::event_store` 为 `std::shared_ptr<EventStore>` | 未设置 | 会话模式下的 `Last-Event-ID` 回放依赖它 | `include/mcp/transport/StreamableHttpServerTransport.hpp:51` |

### `FileEventStore`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| 构造 | `explicit FileEventStore(std::filesystem::path storage_dir)` | — | 参数是**目录**（与 `FileTaskStore` 相反），`create_directories` 失败抛 `std::runtime_error` | `include/mcp/storage/FileEventStore.hpp:15`；`src/http/FileEventStore.cpp:269-278` |
| `Append` | `override uint64_t` | — | 逐行 JSONL 追加，id 取该会话文件内 `max_id + 1`（不接基类计数器）；末行缺换行先补 | `FileEventStore.hpp:17-18`；`FileEventStore.cpp:292-321` |
| `GetEventsSince` | `override std::vector<std::pair<uint64_t, std::string>> const` | 空 | 全量重扫文件后过滤 `id > last_event_id` | `FileEventStore.hpp:19-21`；`FileEventStore.cpp:323-333` |
| `Clear` | `override void` | — | 删除会话文件（不删锁文件），失败抛 `std::runtime_error` | `FileEventStore.hpp:22`；`FileEventStore.cpp:335-345` |
| 文件命名 | `<storage_dir>/sess-<转义 id>.jsonl` 与同名 `.lock` | — | 非 `[A-Za-z0-9_-]` 字符按 `~hh` 十六进制转义，杜绝路径穿越 | `FileEventStore.cpp:30-44`、`280-289` |
| 跨进程串行 | 每个操作先取独占文件锁（POSIX `flock`，Windows `LockFileEx` 无限等待） | — | 打开或加锁失败抛 `std::runtime_error("event store: failed to ...")` | `FileEventStore.cpp:46-110`、`296`、`326`、`337` |
| 裁剪 | 事件数超 `EventStore::kMaxEventsPerSession` 时整文件原子重写 | 10000 | 保留末尾 10000 条，写失败抛错 | `FileEventStore.cpp:249-265`、`316-319` |

### `SessionStore` 与 `InMemorySessionStore`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `SessionRecord` | `struct { std::string protocol_version; int64_t created_at_ms; int64_t last_seen_ms; }` | 两个时间戳 `0` | 跨进程接管会话所需的最小载荷 | `include/mcp/http/SessionStore.hpp:17-21` |
| `Save` / `Load` / `Remove` | 纯虚 `void` / `std::optional<SessionRecord>` / `void` | — | 三个必备接口 | `SessionStore.hpp:27-29` |
| `TrySave` | `virtual bool TrySave(const std::string&, const SessionRecord&)` | 转 `Save` 后 `true` | 满容时的非抛出路径，`InMemorySessionStore::Save` 正是经它判满 | `SessionStore.hpp:31-34`；`SessionStore.hpp:43-46` |
| `Size` / `SweepExpired` | `virtual size_t Size() const` / `virtual size_t SweepExpired()` | `0` | 可选实现，基类返回 `0` | `SessionStore.hpp:36`、`38` |
| `InMemorySessionStore` | `class InMemorySessionStore : public SessionStore` | — | 进程内实现，覆写全部 6 个接口 | `SessionStore.hpp:41`、`43-88` |
| `Save`（内存版） | `override void` | — | 走 `TrySave`，容量满时抛 `std::runtime_error("session store is full")` | `SessionStore.hpp:43-46` |
| `TrySave`（内存版） | `override bool` | — | 先扫过期再判 `records_.size() >= detail::kMaxHttpSessions`；`last_seen_ms == 0` 补当前毫秒 | `SessionStore.hpp:48-60` |
| `Load`（内存版） | `override std::optional<SessionRecord>` | `nullopt` | 命中过期即删并返回空；命中则刷新 `last_seen_ms`（读操作会写） | `SessionStore.hpp:62-73` |
| `Size` / `SweepExpired`（内存版） | `override` | — | 加锁取 `records_.size()` / 返回本轮删除数 | `SessionStore.hpp:80-88` |
| 过期规则 | `now - max(last_seen_ms, created_at_ms) > detail::kSessionIdleTimeoutMs` | 30 分钟（1800000 ms） | 两个时间戳全为 `0` 的记录永不过期 | `SessionStore.hpp:96-105`；`include/mcp/transport/detail/Limits.hpp:24` |
| 容量上限 | `detail::kMaxHttpSessions` | `10000` | 满时 `TrySave` 返 `false`、`Save` 抛 | `Limits.hpp:23`；`SessionStore.hpp:53-54` |
| 接入点 | `StreamableHttpServerOptions::session_store` 为 `std::shared_ptr<SessionStore>` | 未设置 | 会话接管与回收由该 Options 注入 | `include/mcp/transport/StreamableHttpServerTransport.hpp:53` |

## HTTP 服务端底座

`HttpServer`/`EventStore`/`SessionStore` 的传输侧用法与 Options 全字段以 [传输层参考](/zh/reference/transports) 为准；本节只列底座自身的接口、默认值与实现行为。

### `HttpRequest`、`HttpResponse` 与 `HttpServerOptions`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `HttpRequest` | `struct { method; path; headers; body }`，头为 `unordered_map<string,string>` | 全空 | 由实现解析后交出，键名小写化 | `include/mcp/http/HttpServer.hpp:20-25` |
| `HttpResponse` | `struct`，7 字段 | `status_code{200}`、`status_text{"OK"}`、`is_sse{false}`、`sse_close_after_write{false}` | 后两个 SSE 字段控制「写完即断」与定向投递标签 | `HttpServer.hpp:27-35` |
| `HttpHandler` | `std::function<void(const HttpRequest&, HttpResponse&)>` | — | 路由处理体，就地填 `HttpResponse` | `HttpServer.hpp:37` |
| `on_request` | `HttpRequestCallback`（`void(const HttpRequest&)`） | 未设置 | 每个解析成功的请求先于路由调用，异常被吞并记 Warning | `HttpServer.hpp:39`、`44`；`src/http/HttpServerImpl.cpp:452-457` |
| `on_connect` | `HttpConnectCallback`（`void()`） | 未设置 | 每条接受到的连接触发一次，异常被吞 | `HttpServer.hpp:40`、`45`；`HttpServerImpl.cpp:529-534` |
| `on_disconnect` | `HttpDisconnectCallback`（`void()`） | 未设置 | 仅在**某个 SSE 客户端被真实移除**时触发（含发送失败自动摘除） | `HttpServer.hpp:41`、`46`；`HttpServerImpl.cpp:867-884` |
| `allowed_hosts` | `std::vector<std::string>` | `{}`（只放行回环） | `Host` 头缺失即拒；非空时逐条**精确**匹配，空则只接受 `localhost`/`127.0.0.1`/`::1` | `HttpServer.hpp:48`；`HttpServerImpl.cpp:766-786`、`835-849` |
| `allowed_origins` | `std::vector<std::string>` | `{}` | 无 `Origin` 头直接放行；非空时精确匹配，空则要求 Origin 主机与 Host 主机一致或同属回环 | `HttpServer.hpp:49`；`HttpServerImpl.cpp:787-796`、`820-833` |
| `sse_keep_alive_ms` | `int` | `0`（不起心跳线程） | `> 0` 才启动 KeepAlive 线程，按该间隔向全部 SSE 客户端发心跳 | `HttpServer.hpp:51`；`HttpServerImpl.cpp:234-236`、`951-959` |
| `bind_host` | `std::string` | `""` → `INADDR_ANY` | 可带方括号的 IPv6 字面量；解析失败抛 `std::runtime_error("HttpServer: invalid bind_host: ...")`；`listen` backlog 固定 16 | `HttpServer.hpp:53`；`HttpServerImpl.cpp:175-197`、`211-228` |
| `max_request_body_bytes` | `std::optional<std::size_t>` | `detail::kMaxHttpBodyBytes`（4 MiB） | 按 `Content-Length` 预判，超限直接 413；`nullopt` 回退同一常量 | `HttpServer.hpp:55`；`HttpServerImpl.cpp:42`、`659-667`、`430-433` |

### `HttpServer` 公开接口

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| 构造 | `HttpServer(uint16_t port, const HttpServerOptions& options = {})` | 选项默认构造 | 只存配置，不占端口 | `include/mcp/http/HttpServer.hpp:61-62`；`src/http/HttpServer.cpp:15-20` |
| `~HttpServer` | 隐式声明、实现内 `Stop()` | — | 析构即停 | `HttpServer.hpp:63`；`HttpServer.cpp:22-24` |
| `Start` | `void Start()` | — | 幂等（原子交换）；失败时复位状态位并原样重抛；`SetHandler` 的表在此刻快照 | `HttpServer.hpp:65`；`HttpServer.cpp:26-37` |
| `Stop` | `void Stop()` | — | 幂等；在未 join 的旁路线程上调用底层停止，走 `JoinThreadSafely` | `HttpServer.hpp:67`；`HttpServer.cpp:39-49` |
| `SetHandler` | `void SetHandler(std::string_view method, std::string_view path, HttpHandler handler)` | — | `(method, path)` 精确匹配；`Start()` 之后调用抛 `std::logic_error` | `HttpServer.hpp:69-70`；`HttpServer.cpp:51-58`；`HttpServerImpl.cpp:477-479` |
| `SseClientId` | `using SseClientId = uint64_t` | 服务端内自 `1` 递增 | **未启动时 `AddSseClient` 返回 `0`**，故 `0` 是唯一的失败哨兵值 | `HttpServer.hpp:72`；`HttpServer.cpp:60-72`；`src/http/HttpServerImpl.hpp:105` |
| `AddSseClient` | `SseClientId AddSseClient(std::function<void(std::string_view)> send_fn[, std::string tag])` | 无 tag | 两个重载；注册即唤醒心跳线程 | `HttpServer.hpp:73-75`；`HttpServer.cpp:60-72`；`HttpServerImpl.cpp:855-865` |
| `RemoveSseClient` | `void RemoveSseClient(SseClientId id)` | — | 未启动为无操作；真实移除才触发 `on_disconnect` | `HttpServer.hpp:76`；`HttpServer.cpp:74-77`；`HttpServerImpl.cpp:867-884` |
| `BroadcastSse` | `void BroadcastSse(std::string_view event[, const std::function<bool(const std::string&)>& accept])` | 全发 | 谓词按 tag 过滤，谓词抛异常按 `true` 处理；事件超 `detail::kMaxSseEventBytes`（1 MiB）截断 | `HttpServer.hpp:77-79`；`HttpServerImpl.cpp:909-935` |
| `SseClientCount` | `std::size_t SseClientCount() const` | `0` | 未启动返回 `0` | `HttpServer.hpp:80`；`HttpServer.cpp:91-94` |

::: tip
判定顺序为：`on_request` 回调 → `OPTIONS` 一律 204（在 Host/Origin 校验**之前**短路，且不查路由表）→ Host/Origin 校验失败 403 → 路由未命中 404，见 `src/http/HttpServerImpl.cpp:452-479`。
:::

## Token 缓存

`ITokenCache` 是客户端 OAuth token 的可插拔持久层契约（`StoreTokens`/`GetTokens`/`ClearTokens` 纯虚，`StoreBoundIssuer`/`GetBoundIssuer` 带默认实现），`InMemoryTokenCache` 为进程内实现，`FileTokenCache` 为单文件实现——字段、过期语义与 issuer 绑定规则一律以 [授权与扩展参考](/zh/reference/auth-and-extensions) 为准，本页只定边界与落盘事实。

| 符号 | 定位 | 证据与链接 |
|---|---|---|
| `ITokenCache` | 契约与 `TokenContainer` 字段语义 | `include/mcp/client/auth/TokenCache.hpp:23-32`；细节见 [授权与扩展参考](/zh/reference/auth-and-extensions) |
| `FileTokenCache` | `mcp-client` 目标内的单文件实现：构造参数是**文件路径**，`tokens_` 为空时 `Save()` 删除文件；Windows 落盘前经 `CryptProtectData` 加密、POSIX 写明文，两者都走仅属主可访问的原子写，且 POSIX 载入前拒绝「他人可读或非本人所有」的旧文件；issuer 锚点另存 `<cache_path>.issuer`，空 issuer 即删锚点 | `include/mcp/storage/FileTokenCache.hpp:13`；`src/client/FileTokenCache.cpp:78-82`、`84-90`、`103-107`、`109-123`、`173`、`199-232`；`src/detail/AtomicJsonFile.cpp:98-110`；`CMakeLists.txt:181-186`；细节见 [授权与扩展参考](/zh/reference/auth-and-extensions) |

头文件位置提醒：`TokenCache.hpp` 实际在 `include/mcp/client/auth/TokenCache.hpp`（不存在 `include/mcp/auth/` 目录）。

相关任务写法：[/zh/howto/manage-subprocesses](/zh/howto/manage-subprocesses)、[/zh/howto/run-long-tasks](/zh/howto/run-long-tasks)。

## 本页对应源码

- 进程：`include/mcp/process/Process.hpp`、`src/process/ProcessWin32.cpp`、`src/process/ProcessPosix.cpp`、`src/process/detail/WindowsProcessSupport.cpp`、`src/process/detail/PosixProcessSupport.cpp`
- 边界对照：`include/mcp/transport/detail/PlatformIO.hpp`、`src/transport/StdioClientTransport.cpp`、`src/transport/detail/win32_platform.cpp`、`src/transport/detail/posix_platform.cpp`、`CMakeLists.txt`
- 任务存储：`include/mcp/server/McpTaskStore.hpp`、`include/mcp/storage/FileTaskStore.hpp`、`src/server/FileTaskStore.cpp`、`include/mcp/server/ServerOptions.hpp`、`src/server/McpServer.cpp`
- 事件与会话：`include/mcp/http/EventStore.hpp`、`src/http/EventStore.cpp`、`include/mcp/storage/FileEventStore.hpp`、`src/http/FileEventStore.cpp`、`include/mcp/http/SessionStore.hpp`、`include/mcp/transport/detail/Limits.hpp`
- HTTP 底座：`include/mcp/http/HttpServer.hpp`、`src/http/HttpServer.cpp`、`src/http/HttpServerImpl.cpp`、`src/http/HttpServerImpl.hpp`
- Token 缓存：`include/mcp/client/auth/TokenCache.hpp`、`include/mcp/storage/FileTokenCache.hpp`、`src/client/FileTokenCache.cpp`
- 行为佐证：`include/mcp/transport/StreamableHttpServerTransport.hpp`、`examples/conformance/server/SkillsAndTasks.cpp`、`tests/unit/ProcessTests.cpp`、`tests/unit/FileEventStoreTests.cpp`
