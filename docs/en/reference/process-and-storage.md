# Process and Storage Reference

This page is the **authoritative field table** for the public process API, the task/event/session stores and the HTTP server substrate; every field, type and default is checked against the headers and the platform implementations. For stdio transport options see [Transports reference](/en/reference/transports), for facade signatures see [Server reference](/en/reference/server) and [Client reference](/en/reference/client), and for OAuth and token-cache details see [Auth and extensions reference](/en/reference/auth-and-extensions). No procedural steps on this page.

## Public process API

`include/mcp/process/Process.hpp` declares exactly three symbols: the `SpawnOptions` struct, the abstract `SpawnedProcess` class and the free function `Spawn`. It is the only process header carrying `MCP_API` (the macro is empty in the all-static build, `include/mcp/Export.hpp:15-16`), and its implementation is compiled into `mcp-transport` per platform (`CMakeLists.txt:145-156`).

```cpp
struct MCP_API SpawnOptions { /* 8 fields, see the table below */ };
class MCP_API SpawnedProcess { /* pure-virtual interface, 5 member functions */ };
MCP_API std::unique_ptr<SpawnedProcess> Spawn(const SpawnOptions& options);
```

### `SpawnOptions` field by field

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `SpawnOptions` | `struct MCP_API SpawnOptions` | — | Platform-neutral launch description; no constructor, all fields public | `include/mcp/process/Process.hpp:14-23` |
| `command` | `std::string` | empty (required) | Empty makes `Spawn` throw `McpErrorCode::InvalidParams`; on POSIX a value without `/` is resolved by scanning PATH entry by entry with `access(X_OK)`, on Windows it is handed to `CreateProcessW` which does its own search | `Process.hpp:15`, `src/process/ProcessPosix.cpp:41-42`, `77-93` |
| `arguments` | `std::vector<std::string>` | `{}` | Windows joins them into a single command line with the quote/backslash rules, POSIX builds `argv`; on both platforms the command slot is `command` | `Process.hpp:16`, `src/process/detail/WindowsProcessSupport.cpp:24-54`, `ProcessPosix.cpp:235-242` |
| `working_directory` | `std::string` | empty (inherits the parent's) | Windows widens it and passes `lpCurrentDirectory`; POSIX needs `posix_spawn_file_actions_addchdir_np` (macOS or glibc ≥ 2.29) — without it a non-empty value throws `InternalError` | `Process.hpp:17`, `src/process/ProcessWin32.cpp:162-164`, `ProcessPosix.cpp:26-32`, `317-327` |
| `environment` | `std::map<std::string, std::string>` | `{}` | An empty key or a key containing `=` throws `InvalidParams`; POSIX overrides by exact key, Windows override matching is **case-insensitive** | `Process.hpp:18`, `ProcessWin32.cpp:19-23`, `ProcessPosix.cpp:43-47`, `WindowsProcessSupport.cpp:56-92` |
| `inherit_environment` | `bool` | `true` | When true the parent environment is merged in; on POSIX, if there is no override at all the inherited `environ` is passed straight to `posix_spawn` | `Process.hpp:19`, `ProcessPosix.cpp:95-109`, `246-253`, `ProcessWin32.cpp:166-170` |
| `stdout_path` | `std::string` | empty (inherits the parent's stdout) | Non-empty means **truncate and recreate** (POSIX `O_TRUNC` + `0644`, Windows `CREATE_ALWAYS` write-only); redirects stdout only | `Process.hpp:20`, `ProcessWin32.cpp:180-185`, `ProcessPosix.cpp:260`, `src/process/detail/PosixProcessSupport.cpp:14-24` |
| `stderr_path` | `std::string` | empty (inherits the parent's stderr) | Same as above, stderr only; there is no append variant of `stdout_path` | `Process.hpp:21`, `ProcessWin32.cpp:186-191`, `ProcessPosix.cpp:261` |
| `detached` | `bool` | `false` | Windows adds `DETACHED_PROCESS` and `CREATE_BREAKAWAY_FROM_JOB` (retried without breakaway when the latter is denied) and the job object drops `KILL_ON_JOB_CLOSE`; on POSIX, `POSIX_SPAWN_SETSID` creates a new session when available, otherwise a new process group is still created | `Process.hpp:22`, `ProcessWin32.cpp:206-220`, `231`, `ProcessPosix.cpp:335-345`, `WindowsProcessSupport.cpp:108-122` |

### `SpawnedProcess` members

`SpawnedProcess` is abstract; objects are created by platform-internal implementation classes (Windows `WindowsSpawnedProcess`, POSIX `PosixSpawnedProcess`, both in an anonymous namespace and not user-visible types).

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `~SpawnedProcess` | `virtual ~SpawnedProcess() = default` | — | Virtual destructor; `Spawn` returns `std::unique_ptr<SpawnedProcess>`; teardown differs per platform (next two sections) | `Process.hpp:27`, `36` |
| `Pid` | `virtual int64_t Pid() const = 0` | fixed at construction | Child PID, returned unconditionally, no liveness check | `Process.hpp:29`, `ProcessWin32.cpp:67`, `ProcessPosix.cpp:152` |
| `IsRunning` | `virtual bool IsRunning() const = 0` | — | Windows uses `WaitForSingleObject(0)` and records the exit code on the side; any result other than `WAIT_TIMEOUT`/`WAIT_OBJECT_0` throws `InternalError`; POSIX reaps with a non-blocking `waitpid(WNOHANG)` | `Process.hpp:30`, `ProcessWin32.cpp:69-81`, `ProcessPosix.cpp:154-159` |
| `Wait` | `virtual bool Wait(int timeout_ms) = 0` | — | `timeout_ms < 0` waits forever (Windows `INFINITE`, POSIX a 10 ms poll loop); already exited returns `true` immediately, a timeout returns `false` | `Process.hpp:31`, `ProcessWin32.cpp:83-102`, `ProcessPosix.cpp:38`, `161-180` |
| `ExitCode` | `virtual std::optional<int> ExitCode() const = 0` | `std::nullopt` (not exited) | Probes once with zero waiting when not yet exited; on POSIX death by signal yields `128 + signum`, `ECHILD` counts as exited without a code | `Process.hpp:32`, `ProcessWin32.cpp:104-115`, `ProcessPosix.cpp:182-186`, `PosixProcessSupport.cpp:41-61` |
| `Terminate` | `virtual bool Terminate(int grace_ms) = 0` | always `true` (unless it throws) | Grace-then-force sequence is platform specific (next two sections); `grace_ms <= 0` skips the grace period | `Process.hpp:33`, `ProcessWin32.cpp:117-130`, `ProcessPosix.cpp:188-208` |

### `Spawn` and error semantics

| Item | Conclusion | Evidence |
|---|---|---|
| Signature | `std::unique_ptr<SpawnedProcess> Spawn(const SpawnOptions& options)`, `options` by const reference, ownership returned | `include/mcp/process/Process.hpp:36` |
| Validation | Both platforms share the same `ValidateOptions`: empty `command` → `McpError(McpErrorCode::InvalidParams, "spawn command must not be empty")`; empty environment key or one containing `=` → same code with `"spawn environment key must be non-empty and must not contain '='"` | `ProcessWin32.cpp:16-24`, `ProcessPosix.cpp:40-48` |
| Platform failure | Always `McpError(McpErrorCode::InternalError, ...)`: Windows messages end with `Windows error <N>`, POSIX messages carry `std::strerror` text | `ProcessWin32.cpp:226-229`, `249-251`, `ProcessPosix.cpp:273-275`, `296-301`, `351-354` |
| Redirect open failure | The platform helper throws `std::runtime_error`; `Spawn` narrows it to `McpError(InternalError, what())` and closes the descriptors/handles already opened | `ProcessWin32.cpp:26-32`, `192-197`, `ProcessPosix.cpp:111-117`, `262-266` |
| Production callers | **None.** Repo-wide, `Spawn` only appears in `include/mcp/process/Process.hpp`, `src/process/ProcessWin32.cpp`, `src/process/ProcessPosix.cpp` and the tests `tests/unit/ProcessTests.cpp`, `tests/unit/ProcessTestHelper.cpp` | `include/mcp/process/Process.hpp:36`, `tests/unit/ProcessTests.cpp:1` |

## Platform implementation differences

### Windows: `ProcessWin32.cpp`

| Behaviour | Implementation and semantics | Evidence |
|---|---|---|
| Creation entry point | `CreateProcessW` with `bInheritHandles = TRUE`; `dwFlags` always carries `CREATE_UNICODE_ENVIRONMENT`, `CREATE_SUSPENDED`, `CREATE_NEW_PROCESS_GROUP` | `src/process/ProcessWin32.cpp:206-212` |
| Standard handles | The three parent standard handles are duplicated via `DuplicateHandle(..., DUPLICATE_SAME_ACCESS)` with the inherit flag set and placed in `STARTF_USESTDHANDLES`; when `stdout_path`/`stderr_path` are set they become file handles instead | `ProcessWin32.cpp:34-45`, `172-204` |
| Suspend, assign, resume | The child is created suspended, then a job object is created and `AssignProcessToJobObject` is called, and only then `ResumeThread`; a failed resume triggers `TerminateProcess` + infinite wait + closing every handle and throws `InternalError` | `ProcessWin32.cpp:231-253` |
| Job object | Always attempted: `kill_on_close = !detached`, `breakaway_ok = true`; a failed creation or assignment only logs a Warning and continues without a job object (no throw) | `ProcessWin32.cpp:231-240`, `src/process/detail/WindowsProcessSupport.cpp:108-126` |
| Environment block | UTF-8 → UTF-16 conversion uses `MB_ERR_INVALID_CHARS` (invalid UTF-8 throws); the block ends with two `L'\0'` and is paired with `CREATE_UNICODE_ENVIRONMENT` | `WindowsProcessSupport.cpp:11-22`, `56-92`, `ProcessWin32.cpp:166-170`, `206` |
| `Terminate` sequence | With `grace_ms > 0` it first sends `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid)` then `Wait(grace_ms)`; if the child exits within the grace period a `TerminateJobObject(job, 1)` sweeps the rest; otherwise `TerminateWindowsJobOrProcess` (job object → `TerminateJobObject`, none → `TerminateProcess`) followed by `Wait(-1)` | `ProcessWin32.cpp:117-130`, `WindowsProcessSupport.cpp:128-131` |
| Destructor | Non-`detached`: if still running, force-kill + infinite wait + record the exit code; `detached`: does nothing, the child survives; both close the job and process handles | `ProcessWin32.cpp:52-65` |
| Reaping | No `waitpid` needed: the exit code comes from `GetExitCodeProcess`, and whichever of `IsRunning`/`Wait`/`ExitCode` first sees `WAIT_OBJECT_0` pins `exited_` | `ProcessWin32.cpp:133-141` |

### POSIX: `ProcessPosix.cpp`

| Behaviour | Implementation and semantics | Evidence |
|---|---|---|
| Creation entry point | `posix_spawn` (not `fork`/`exec`) with a `nullptr`-terminated `argv`; command resolution is a hand-written PATH scan (`argv[0]` stays the original `command`) | `src/process/ProcessPosix.cpp:235-242`, `347-354`, `77-93` |
| Process group / session | Non-`detached`: `POSIX_SPAWN_SETPGROUP` + `posix_spawnattr_setpgroup(0)` → the child becomes the leader of its own group (group id equals its PID); `detached` with `POSIX_SPAWN_SETSID` available → new session | `ProcessPosix.cpp:335-345` |
| Signal dispositions | `posix_spawnattr_setsigdefault` restores `SIGPIPE` to its default action so the child does not inherit an ignored disposition | `ProcessPosix.cpp:329-335` |
| Redirection | `adddup2` + `addclose` file actions point `stdout`/`stderr` at files opened `O_WRONLY`, `O_CREAT`, `O_TRUNC` (`0644`, plus `O_CLOEXEC` when available); stdin is always inherited | `ProcessPosix.cpp:268-316`, `src/process/detail/PosixProcessSupport.cpp:14-24` |
| Working directory | Requires `posix_spawn_file_actions_addchdir_np`, compiled in only on macOS or glibc ≥ 2.29; otherwise a non-empty `working_directory` throws `InternalError` (macro `MCP_PROCESS_HAS_SPAWN_CHDIR`) | `ProcessPosix.cpp:26-32`, `317-327` |
| `Terminate` sequence | With `grace_ms > 0` it sends `SIGTERM` to the process group then `Wait(grace_ms)`; if the child exits within the grace period but `kill(-pid, 0)` still finds group members, a `SIGKILL` follows; otherwise `SIGKILL` to the group plus `Wait(-1)` | `ProcessPosix.cpp:188-208`, `PosixProcessSupport.cpp:30-39` |
| Process-group signalling | `SignalPosixProcessGroup` uses `kill(-pid, sig)` and falls back to the single process `kill(pid, sig)` on `ESRCH` | `PosixProcessSupport.cpp:30-34` |
| Reaping | Non-blocking `waitpid(pid, WNOHANG)`; `EINTR` retries, `ECHILD` counts as exited (without a code), death by signal maps to `128 + signum`; `Wait` polls every 10 ms instead of using a condition variable | `PosixProcessSupport.cpp:41-61`, `ProcessPosix.cpp:38`, `161-180`, `211-221` |
| Destructor | Non-`detached`: `SIGKILL` the group then reap blocking; `detached`: a separate `detach()`-ed thread reaps blocking so the destructor is never stuck in `waitpid`; both swallow reaping exceptions | `ProcessPosix.cpp:135-150` |

## Boundary with the stdio transport

::: warning
`StdioClientTransport` does **not** go through the public `Spawn`. It only includes `mcp/transport/detail/PlatformIO.hpp` and calls the internal `CreateProcess` with a `ProcessStartInfo` from namespace `mcp::detail` (`src/transport/StdioClientTransport.cpp:4`, `194-206`). Those internal symbols (process handle, pipe handles, start-info struct) are not user API and must never be cited as interface; this section only describes the behavioural difference.
:::

| Dimension | Public `Spawn` | Internal stdio transport path | Evidence |
|---|---|---|---|
| Declaration site | `include/mcp/process/Process.hpp`, marked `MCP_API` | `include/mcp/transport/detail/PlatformIO.hpp` (a `detail/` internal header, not exported) | `Process.hpp:14-36`, `PlatformIO.hpp:26-44` |
| Standard streams | Reuses the parent handles/descriptors, or sends stdout/stderr to files — **there is no pipe read-back channel** | `pipe()` wires both directions; the parent keeps the stdin write end and the stdout/stderr read ends | `ProcessWin32.cpp:172-204`, `ProcessPosix.cpp:268-316`, `src/transport/detail/posix_platform.cpp:221-250` |
| Low-level creation | Windows `CreateProcessW` (suspended + job + resume); POSIX `posix_spawn` | Windows `CreateProcessW` with `CREATE_NO_WINDOW`, suspended and job-assigned only when `terminate_process_tree`; POSIX `fork` + `execvp`, `setpgid(0,0)` only when that flag is set | `ProcessWin32.cpp:206-253`, `win32_platform.cpp:510-511`, `534-536`, `posix_platform.cpp:319`, `350`, `360` |
| Termination grace | `Terminate(grace_ms)`: Windows `CTRL_BREAK_EVENT`, POSIX `SIGTERM` to the process group | The internal handle's `Terminate(timeout_ms)`: Windows kills the job/process straight away, POSIX sends `SIGTERM` to the single process (process group only when `terminate_process_tree`) and `SIGKILL` after the timeout | `ProcessWin32.cpp:117-130`, `ProcessPosix.cpp:188-208`, `win32_platform.cpp:433-437`, `posix_platform.cpp:159-161`, `179-191` |
| Job object | Always attempted (`detached` drops kill-on-close) | Created only when `terminate_process_tree = true`, with `kill_on_close = true`, `breakaway_ok = false` | `ProcessWin32.cpp:231`, `win32_platform.cpp:534-541` |
| Shared substrate | Both paths **share** the same low-level platform helpers `src/process/detail/PosixProcessSupport.*` and `WindowsProcessSupport.*` (redirect opening, process-group signalling, `waitpid` reaping, command-line building, environment block, job object) | Same as left | `ProcessPosix.cpp:3`, `ProcessWin32.cpp:4`, `posix_platform.cpp:4`, `win32_platform.cpp:5` |
| Ownership and call sites | Compiled into `mcp-transport`, zero production callers, covered only by `tests/unit/ProcessTests.cpp` and `ProcessTestHelper.cpp` | Called by `StdioClientTransport::Connect()` and shaped by `shutdown_timeout` and `terminate_process_tree` | `CMakeLists.txt:145-156`, `src/transport/StdioClientTransport.cpp:187-206`, `68-69` |

## Task store

### `TaskStatus` and `TaskState`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `TaskStatus` | `enum class { Pending, Working, Completed, Failed, Cancelled, InputRequired }` | records start as `Pending` | Terminal states are `Completed`/`Failed`/`Cancelled`; once terminal, writes return `false`; persisted as an integer | `include/mcp/server/McpTaskStore.hpp:16-23`, `src/server/FileTaskStore.cpp:39-42`, `49` |
| `TaskState` | `struct` | — | The complete task record; `FileTaskStore` serialises it field by field to JSON | `McpTaskStore.hpp:25-43` |
| `task_id` | `std::string` | empty | Written from the `CreateTask` argument | `McpTaskStore.hpp:26` |
| `owner` | `std::string` | empty | Session ownership, rewritable via `SetTaskOwner` | `McpTaskStore.hpp:27` |
| `status` | `TaskStatus` | `Pending` | The only status field | `McpTaskStore.hpp:28` |
| `result` | `std::optional<JsonValue>` | none | What `UpdateTask` stores as the finished result; `FailTask` also writes the error payload here | `McpTaskStore.hpp:29`, `FileTaskStore.cpp:304`, `389` |
| `error_message` | `std::optional<std::string>` | none | Holds the `CancelTask` reason and the `message` field of the `FailTask` payload | `McpTaskStore.hpp:30`, `FileTaskStore.cpp:334`, `391-393` |
| `input_required` | `std::optional<JsonValue>` | none | Payload snapshot taken when the task parks for MRTR (coexists with `input_requests`; the upper layer decides the semantics) | `McpTaskStore.hpp:31` |
| `progress` | `double` | `0` | Not optional, always serialised | `McpTaskStore.hpp:32`, `FileTaskStore.cpp:53` |
| `progress_total` | `std::optional<double>` | none | Optional total | `McpTaskStore.hpp:33` |
| `created_at` | `std::string` | empty → fixed at creation | A millisecond timestamp stored as a **string**; TTL expiry parses it with `ParseMillis` and a parse failure means the record never expires | `McpTaskStore.hpp:34`, `FileTaskStore.cpp:21-26`, `27-37`, `471-479` |
| `status_message` | `std::optional<std::string>` | none | Free-text status note | `McpTaskStore.hpp:35` |
| `last_updated_at` | `std::optional<std::string>` | none | Refreshed to the current millisecond on every successful write | `McpTaskStore.hpp:36`, `FileTaskStore.cpp:235` |
| `ttl_ms` | `std::optional<int64_t>` | none | `<= 0` means never expires | `McpTaskStore.hpp:37`, `FileTaskStore.cpp:475` |
| `poll_interval_ms` | `std::optional<int64_t>` | none | Suggested polling interval, payload only | `McpTaskStore.hpp:38` |
| `input_requests` | `std::optional<InputRequests>` | none | Written by `ParkTaskForInput` | `McpTaskStore.hpp:39`, `FileTaskStore.cpp:533-534` |
| `input_responses` | `std::optional<JsonValue>` | none | `AdvanceTaskWithInput` merges key by key into the existing object | `McpTaskStore.hpp:40`, `FileTaskStore.cpp:497-503` |
| `tool_name` / `tool_arguments` | `std::optional<std::string>` / `std::optional<JsonValue>` | none | Written by `BindTaskTool`; JSON keys are `toolName`/`toolArguments` | `McpTaskStore.hpp:41-42`, `FileTaskStore.cpp:562-563`, `62-63` |

### `IMcpTaskStore` virtual functions

The class carries **no** `MCP_API` and is a plain C++ abstract interface; a virtual function with a body is an optional capability with a default implementation, so unless it is overridden it returns `false` or an empty container.

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `~IMcpTaskStore` | `virtual ~IMcpTaskStore() = default` | — | Virtual destructor | `include/mcp/server/McpTaskStore.hpp:47` |
| `CreateTask` | `virtual TaskState CreateTask(const std::string& task_id) = 0` | — | The only pure-virtual creation entry point | `McpTaskStore.hpp:49` |
| `CreateTask` (overload) | `virtual TaskState CreateTask(task_id, owner, std::optional<int64_t> ttl_ms)` | not pure virtual | Template method: delegates to the one-argument version, then calls `SetTaskOwner`/`SetTaskTtl` when `owner` is non-empty / `ttl_ms` has a value | `McpTaskStore.hpp:50-58` |
| `GetTask` | `virtual std::optional<TaskState> GetTask(const std::string&) = 0` | — | Snapshot lookup by id | `McpTaskStore.hpp:59` |
| `UpdateTask` | `virtual bool UpdateTask(const std::string&, const std::optional<JsonValue>& result) = 0` | — | A value in `result` → `Completed`, empty → `Working` | `McpTaskStore.hpp:60-61`, `FileTaskStore.cpp:304` |
| `CancelTask` | `virtual bool CancelTask(const std::string&, const std::optional<std::string>& reason) = 0` | — | Sets `Cancelled`, reason lands in `error_message` | `McpTaskStore.hpp:62-63`, `FileTaskStore.cpp:330-334` |
| `SetTaskStatus` | `virtual bool SetTaskStatus(const std::string&, TaskStatus) = 0` | — | Raw status write; moving away from a terminal status is refused | `McpTaskStore.hpp:64`, `FileTaskStore.cpp:355-358` |
| `AdvanceTaskWithInput` | `virtual bool AdvanceTaskWithInput(const std::string&, const JsonValue&)` | `false` | MRTR advance, only `InputRequired` tasks can advance | `McpTaskStore.hpp:65`, `FileTaskStore.cpp:493-506` |
| `ParkTaskForInput` | `virtual bool ParkTaskForInput(const std::string&, const InputRequests&)` | `false` | MRTR parking | `McpTaskStore.hpp:66`, `FileTaskStore.cpp:529-534` |
| `BindTaskTool` | `virtual bool BindTaskTool(const std::string&, const std::string&, const std::optional<JsonValue>&)` | `false` | Records the tool and arguments needed to resume | `McpTaskStore.hpp:67-68`, `FileTaskStore.cpp:559-563` |
| `GetAllTasks` | `virtual std::vector<TaskState> GetAllTasks() = 0` | — | Full enumeration (unordered) | `McpTaskStore.hpp:69` |
| `FailTask` | `virtual bool FailTask(const std::string&, const JsonValue& error_payload)` | `false` | Sets `Failed`, payload lands in `result` | `McpTaskStore.hpp:70-75`, `FileTaskStore.cpp:385-392` |
| `RemoveTask` | `virtual bool RemoveTask(const std::string&)` | `false` | Physical deletion, unlike `CancelTask` | `McpTaskStore.hpp:76-80` |
| `GetExpiredTaskIds` | `virtual std::vector<std::string> GetExpiredTaskIds(int64_t now_ms)` | `{}` | Enumerates only, never deletes | `McpTaskStore.hpp:81-85`, `FileTaskStore.cpp:471-481` |
| `SetTaskOwner` | `virtual bool SetTaskOwner(const std::string&, const std::string&)` | `false` | Rewrites ownership | `McpTaskStore.hpp:86-91` |
| `SetTaskTtl` | `virtual bool SetTaskTtl(const std::string&, int64_t ttl_ms)` | `false` | Rewrites the TTL | `McpTaskStore.hpp:92-97` |

Integration point: `ServerOptions` holds the store in its `task_store` field as `std::shared_ptr<IMcpTaskStore>` (`include/mcp/server/ServerOptions.hpp:78`), and `McpServer::SweepExpiredTasks` is the only reclamation path — `GetExpiredTaskIds` followed by `RemoveTask`, with exceptions from either call logged as Error only (`include/mcp/server/McpServer.hpp:179`, `src/server/McpServer.cpp:1070-1088`).

### `FileTaskStore`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| Constructor | `explicit FileTaskStore(std::filesystem::path storage_path)` | — | The argument is a **single journal file path** (not a directory); loading happens in the constructor and a missing file means an empty store | `include/mcp/storage/FileTaskStore.hpp:16`, `src/server/FileTaskStore.cpp:117-121`, `139-143` |
| Destructor | `~FileTaskStore() override` | — | Calls `Flush()` → full atomic rewrite | `FileTaskStore.hpp:17`, `FileTaskStore.cpp:123-125`, `586-589` |
| 15 overrides | Same names and signatures as `IMcpTaskStore`, all `override` | — | No default implementation survives: including `AdvanceTaskWithInput`/`ParkTaskForInput`/`BindTaskTool`/`FailTask`/`RemoveTask`/`GetExpiredTaskIds`/`SetTaskOwner`/`SetTaskTtl` | `FileTaskStore.hpp:19-41` |
| Write semantics | Update functions return `bool` | `false` | Unknown id → `false`; already terminal → `false` (`SetTaskOwner`/`SetTaskTtl`/`BindTaskTool` have no terminal guard); persistence failure → in-memory rollback plus `McpError(InternalError)` | `FileTaskStore.cpp:300-301`, `313-316`, `329-330`, `355-356`, `385-386` |
| Create semantics | `CreateTask` | — | A duplicate id throws `McpError(McpErrorCode::InvalidParams, "task already exists: " + id)` | `FileTaskStore.cpp:239-241`, `269-271` |
| Concurrency model | `write_mutex_` serialises writes + `shared_mutex` for readers | — | Readers take only the shared lock; writers hold `write_mutex_` for the whole call | `FileTaskStore.hpp:52-53`, `FileTaskStore.cpp:283-288`, `294` |
| Journal format | One JSON line per record: `{"task_id":...,"state":{...}}`; deletion appends `{"task_id":...,"removed":true}` | — | Append-only; replay overwrites or erases line by line | `FileTaskStore.cpp:101-114`, `213-216`, `591-633` |
| Compaction trigger | `pending_records_ > kMinCompactThreshold + 2 × live task count` | `kMinCompactThreshold = 64` | When met, the whole file is rewritten right after the append: temporary file + `fsync`/`FlushFileBuffers` + atomic `rename` (internal helper, `src/detail/AtomicJsonFile.cpp:49-92`), counter reset to zero | `FileTaskStore.cpp:19`, `611-614`, `635-650` |
| Corruption handling | A bad line in the middle → copy a `.corrupt` backup with the same name and **clear the whole store**; a truncated trailing line is only logged and ignored | — | The legacy `{"tasks":{...}}` snapshot format is still read | `FileTaskStore.cpp:127-137`, `187-196`, `201-205` |
| Owning target | `mcp-server` static library | — | Example usage: `examples/conformance/server/SkillsAndTasks.cpp:456` | `CMakeLists.txt:172-175` |

## Event and session stores

### `EventStore`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `EventStore` | `class EventStore` (virtual destructor) | — | Overridable in-process base class, not pure virtual; no `MCP_API` | `include/mcp/http/EventStore.hpp:15-18` |
| `Append` | `virtual uint64_t Append(std::string_view session_id, std::string event_data)` | — | Monotonically increasing global id starting at `1` (one counter shared across sessions); `event_data` is moved in by value | `EventStore.hpp:20`, `src/http/EventStore.cpp:8-22`, `37` |
| `GetEventsSince` | `virtual std::vector<std::pair<uint64_t, std::string>> GetEventsSince(std::string_view, uint64_t last_event_id) const` | empty | Returns every event with `id > last_event_id`, in order | `EventStore.hpp:22-23`, `EventStore.cpp:24-37` |
| `Clear` | `virtual void Clear(std::string_view session_id)` | — | Drops the whole session and does not recycle ids | `EventStore.hpp:25`, `EventStore.cpp:39-42` |
| `kMaxEventsPerSession` | `static constexpr size_t` | `10000` | Excess events are trimmed from the front | `EventStore.hpp:27`, `EventStore.cpp:16-20` |
| Integration point | `StreamableHttpServerOptions::event_store` is a `std::shared_ptr<EventStore>` | unset | `Last-Event-ID` replay in session mode relies on it | `include/mcp/transport/StreamableHttpServerTransport.hpp:51` |

### `FileEventStore`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| Constructor | `explicit FileEventStore(std::filesystem::path storage_dir)` | — | The argument is a **directory** (the opposite of `FileTaskStore`); a failing `create_directories` throws `std::runtime_error` | `include/mcp/storage/FileEventStore.hpp:15`, `src/http/FileEventStore.cpp:269-278` |
| `Append` | `override uint64_t` | — | Appends one JSONL line per event; the id is `max_id + 1` within that session file (the base-class counter is bypassed); a missing trailing newline is added first | `FileEventStore.hpp:17-18`, `FileEventStore.cpp:292-321` |
| `GetEventsSince` | `override std::vector<std::pair<uint64_t, std::string>> const` | empty | Rescans the whole file and filters `id > last_event_id` | `FileEventStore.hpp:19-21`, `FileEventStore.cpp:323-333` |
| `Clear` | `override void` | — | Removes the session file (the lock file stays), failure throws `std::runtime_error` | `FileEventStore.hpp:22`, `FileEventStore.cpp:335-345` |
| File naming | `<storage_dir>/sess-<escaped id>.jsonl` plus a sibling `.lock` | — | Characters outside `[A-Za-z0-9_-]` are escaped as `~hh`, which rules out path traversal | `FileEventStore.cpp:30-44`, `280-289` |
| Cross-process serialisation | Every operation first takes an exclusive file lock (POSIX `flock`, Windows `LockFileEx` waiting indefinitely) | — | Failing to open or lock throws `std::runtime_error("event store: failed to ...")` | `FileEventStore.cpp:46-110`, `296`, `326`, `337` |
| Trimming | When the count exceeds `EventStore::kMaxEventsPerSession` the whole file is rewritten atomically | 10000 | Keeps the last 10000 events; a failed write throws | `FileEventStore.cpp:249-265`, `316-319` |

### `SessionStore` and `InMemorySessionStore`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `SessionRecord` | `struct { std::string protocol_version; int64_t created_at_ms; int64_t last_seen_ms; }` | both timestamps `0` | The minimum payload needed to take over a session across processes | `include/mcp/http/SessionStore.hpp:17-21` |
| `Save` / `Load` / `Remove` | pure virtual `void` / `std::optional<SessionRecord>` / `void` | — | The three mandatory operations | `SessionStore.hpp:27-29` |
| `TrySave` | `virtual bool TrySave(const std::string&, const SessionRecord&)` | delegates to `Save`, returns `true` | The non-throwing path when the store is full — `InMemorySessionStore::Save` uses it for the capacity check | `SessionStore.hpp:31-34`, `SessionStore.hpp:43-46` |
| `Size` / `SweepExpired` | `virtual size_t Size() const` / `virtual size_t SweepExpired()` | `0` | Optional implementations; the base returns `0` | `SessionStore.hpp:36`, `38` |
| `InMemorySessionStore` | `class InMemorySessionStore : public SessionStore` | — | In-process implementation overriding all 6 operations | `SessionStore.hpp:41`, `43-88` |
| `Save` (in-memory) | `override void` | — | Goes through `TrySave` and throws `std::runtime_error("session store is full")` when capacity is reached | `SessionStore.hpp:43-46` |
| `TrySave` (in-memory) | `override bool` | — | Sweeps expired records first, then rejects once `records_.size() >= detail::kMaxHttpSessions`; a `last_seen_ms == 0` is filled with the current millisecond | `SessionStore.hpp:48-60` |
| `Load` (in-memory) | `override std::optional<SessionRecord>` | `nullopt` | An expired hit is erased and returned as empty; a live hit refreshes `last_seen_ms` (reading writes) | `SessionStore.hpp:62-73` |
| `Size` / `SweepExpired` (in-memory) | `override` | — | Locked `records_.size()` / the number erased in this round | `SessionStore.hpp:80-88` |
| Expiry rule | `now - max(last_seen_ms, created_at_ms) > detail::kSessionIdleTimeoutMs` | 30 minutes (1800000 ms) | A record whose two timestamps are both `0` never expires | `SessionStore.hpp:96-105`, `include/mcp/transport/detail/Limits.hpp:24` |
| Capacity limit | `detail::kMaxHttpSessions` | `10000` | When full `TrySave` returns `false` and `Save` throws | `Limits.hpp:23`, `SessionStore.hpp:53-54` |
| Integration point | `StreamableHttpServerOptions::session_store` is a `std::shared_ptr<SessionStore>` | unset | Session takeover and reclamation are injected through this Options field | `include/mcp/transport/StreamableHttpServerTransport.hpp:53` |

## HTTP server substrate

The transport-side usage of `HttpServer`/`EventStore`/`SessionStore` and the full transport Options are owned by the [Transports reference](/en/reference/transports); this section lists only the substrate's own interface, defaults and implementation behaviour.

### `HttpRequest`, `HttpResponse` and `HttpServerOptions`

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `HttpRequest` | `struct { method; path; headers; body }`, headers as `unordered_map<string,string>` | all empty | Handed over after parsing; header names are lower-cased | `include/mcp/http/HttpServer.hpp:20-25` |
| `HttpResponse` | `struct` with 7 fields | `status_code{200}`, `status_text{"OK"}`, `is_sse{false}`, `sse_close_after_write{false}` | The last two SSE fields control "close after writing" and the targeted-delivery tag | `HttpServer.hpp:27-35` |
| `HttpHandler` | `std::function<void(const HttpRequest&, HttpResponse&)>` | — | Route body; fills `HttpResponse` in place | `HttpServer.hpp:37` |
| `on_request` | `HttpRequestCallback` (`void(const HttpRequest&)`) | unset | Called for every successfully parsed request, before routing; exceptions are swallowed and logged as Warning | `HttpServer.hpp:39`, `44`, `src/http/HttpServerImpl.cpp:452-457` |
| `on_connect` | `HttpConnectCallback` (`void()`) | unset | Fires once per accepted connection; exceptions are swallowed | `HttpServer.hpp:40`, `45`, `HttpServerImpl.cpp:529-534` |
| `on_disconnect` | `HttpDisconnectCallback` (`void()`) | unset | Fires only when an SSE client entry is **actually removed** (including the automatic removal after a failed send) | `HttpServer.hpp:41`, `46`, `HttpServerImpl.cpp:867-884` |
| `allowed_hosts` | `std::vector<std::string>` | `{}` (loopback only) | A missing `Host` header is rejected; when non-empty each entry must match **exactly**, when empty only `localhost`/`127.0.0.1`/`::1` are accepted | `HttpServer.hpp:48`, `HttpServerImpl.cpp:766-786`, `835-849` |
| `allowed_origins` | `std::vector<std::string>` | `{}` | A request without an `Origin` header is allowed; when non-empty matching is exact, when empty the Origin host must equal the Host host or both must be loopback | `HttpServer.hpp:49`, `HttpServerImpl.cpp:787-796`, `820-833` |
| `sse_keep_alive_ms` | `int` | `0` (no heartbeat thread) | Only `> 0` starts the KeepAlive thread, which heartbeats every SSE client at that interval | `HttpServer.hpp:51`, `HttpServerImpl.cpp:234-236`, `951-959` |
| `bind_host` | `std::string` | `""` → `INADDR_ANY` | Accepts bracketed IPv6 literals; an unparseable value throws `std::runtime_error("HttpServer: invalid bind_host: ...")`; the `listen` backlog is fixed at 16 | `HttpServer.hpp:53`, `HttpServerImpl.cpp:175-197`, `211-228` |
| `max_request_body_bytes` | `std::optional<std::size_t>` | `detail::kMaxHttpBodyBytes` (4 MiB) | Decided from `Content-Length` before reading, over the limit answers 413; `nullopt` falls back to the same constant | `HttpServer.hpp:55`, `HttpServerImpl.cpp:42`, `659-667`, `430-433` |

### `HttpServer` public interface

| Symbol | Signature/Type | Default | Notes | Evidence |
|---|---|---|---|---|
| Constructor | `HttpServer(uint16_t port, const HttpServerOptions& options = {})` | default-constructed options | Stores configuration only, binds no port | `include/mcp/http/HttpServer.hpp:61-62`, `src/http/HttpServer.cpp:15-20` |
| `~HttpServer` | Declared implicitly, calls `Stop()` | — | Destruction stops the server | `HttpServer.hpp:63`, `HttpServer.cpp:22-24` |
| `Start` | `void Start()` | — | Idempotent (atomic exchange); on failure it resets the flag and rethrows; the `SetHandler` table is snapshotted at this moment | `HttpServer.hpp:65`, `HttpServer.cpp:26-37` |
| `Stop` | `void Stop()` | — | Idempotent; the underlying stop runs on a helper thread so a caller never joins itself, via `JoinThreadSafely` | `HttpServer.hpp:67`, `HttpServer.cpp:39-49` |
| `SetHandler` | `void SetHandler(std::string_view method, std::string_view path, HttpHandler handler)` | — | Exact `(method, path)` match; calling it after `Start()` throws `std::logic_error` | `HttpServer.hpp:69-70`, `HttpServer.cpp:51-58`, `HttpServerImpl.cpp:477-479` |
| `SseClientId` | `using SseClientId = uint64_t` | incremented from `1` inside the server | **`AddSseClient` returns `0` when the server is not started**, so `0` is the only failure sentinel | `HttpServer.hpp:72`, `HttpServer.cpp:60-72`, `src/http/HttpServerImpl.hpp:105` |
| `AddSseClient` | `SseClientId AddSseClient(std::function<void(std::string_view)> send_fn[, std::string tag])` | no tag | Two overloads; registering wakes the heartbeat thread | `HttpServer.hpp:73-75`, `HttpServer.cpp:60-72`, `HttpServerImpl.cpp:855-865` |
| `RemoveSseClient` | `void RemoveSseClient(SseClientId id)` | — | A no-op when not started; `on_disconnect` fires only on a real removal | `HttpServer.hpp:76`, `HttpServer.cpp:74-77`, `HttpServerImpl.cpp:867-884` |
| `BroadcastSse` | `void BroadcastSse(std::string_view event[, const std::function<bool(const std::string&)>& accept])` | send to all | The predicate filters by tag and a throwing predicate is treated as `true`; events longer than `detail::kMaxSseEventBytes` (1 MiB) are truncated | `HttpServer.hpp:77-79`, `HttpServerImpl.cpp:909-935` |
| `SseClientCount` | `std::size_t SseClientCount() const` | `0` | Returns `0` when not started | `HttpServer.hpp:80`, `HttpServer.cpp:91-94` |

::: tip
The decision order is: `on_request` callback → every `OPTIONS` request answered 204 (short-circuited **before** the Host/Origin check and never looking up the route table) → Host/Origin failure 403 → unmatched route 404, see `src/http/HttpServerImpl.cpp:452-479`.
:::

## Token cache

`ITokenCache` is the pluggable persistence contract for client OAuth tokens (`StoreTokens`/`GetTokens`/`ClearTokens` are pure virtual, `StoreBoundIssuer`/`GetBoundIssuer` come with default implementations), `InMemoryTokenCache` is the in-process implementation and `FileTokenCache` the single-file one — field semantics, expiry rules and issuer binding are owned by the [Auth and extensions reference](/en/reference/auth-and-extensions); this page only fixes the boundary and the on-disk facts.

| Symbol | Role | Evidence and link |
|---|---|---|
| `ITokenCache` | Contract and `TokenContainer` field semantics | `include/mcp/client/auth/TokenCache.hpp:23-32`; details in the [Auth and extensions reference](/en/reference/auth-and-extensions) |
| `FileTokenCache` | Single-file implementation inside the `mcp-client` target: the constructor takes a **file path**, `Save()` deletes the file when `tokens_` is empty; Windows encrypts with `CryptProtectData` before writing and POSIX writes plaintext, both through the owner-only atomic write, and on POSIX loading refuses a file owned by someone else or readable by group/other; the issuer anchor lives in `<cache_path>.issuer` and an empty issuer removes the anchor | `include/mcp/storage/FileTokenCache.hpp:13`, `src/client/FileTokenCache.cpp:78-82`, `84-90`, `103-107`, `109-123`, `173`, `199-232`, `src/detail/AtomicJsonFile.cpp:98-110`, `CMakeLists.txt:181-186`; details in the [Auth and extensions reference](/en/reference/auth-and-extensions) |

Header location note: `TokenCache.hpp` really lives at `include/mcp/client/auth/TokenCache.hpp` (there is no `include/mcp/auth/` directory).

Task-shaped guides: [/en/howto/manage-subprocesses](/en/howto/manage-subprocesses), [/en/howto/run-long-tasks](/en/howto/run-long-tasks).

## Source files behind this page

- Process: `include/mcp/process/Process.hpp`, `src/process/ProcessWin32.cpp`, `src/process/ProcessPosix.cpp`, `src/process/detail/WindowsProcessSupport.cpp`, `src/process/detail/PosixProcessSupport.cpp`
- Boundary comparison: `include/mcp/transport/detail/PlatformIO.hpp`, `src/transport/StdioClientTransport.cpp`, `src/transport/detail/win32_platform.cpp`, `src/transport/detail/posix_platform.cpp`, `CMakeLists.txt`
- Task store: `include/mcp/server/McpTaskStore.hpp`, `include/mcp/storage/FileTaskStore.hpp`, `src/server/FileTaskStore.cpp`, `include/mcp/server/ServerOptions.hpp`, `src/server/McpServer.cpp`
- Events and sessions: `include/mcp/http/EventStore.hpp`, `src/http/EventStore.cpp`, `include/mcp/storage/FileEventStore.hpp`, `src/http/FileEventStore.cpp`, `include/mcp/http/SessionStore.hpp`, `include/mcp/transport/detail/Limits.hpp`
- HTTP substrate: `include/mcp/http/HttpServer.hpp`, `src/http/HttpServer.cpp`, `src/http/HttpServerImpl.cpp`, `src/http/HttpServerImpl.hpp`
- Token cache: `include/mcp/client/auth/TokenCache.hpp`, `include/mcp/storage/FileTokenCache.hpp`, `src/client/FileTokenCache.cpp`
- Behaviour evidence: `include/mcp/transport/StreamableHttpServerTransport.hpp`, `examples/conformance/server/SkillsAndTasks.cpp`, `tests/unit/ProcessTests.cpp`, `tests/unit/FileEventStoreTests.cpp`
