# Process and Storage Reference

This page is the **authoritative field table** for the public process API, the task/event/session stores and the HTTP server substrate; every field, type and default is checked against the headers and the platform implementations. For stdio transport options see [Transports reference](/en/reference/transports), for facade signatures see [Server reference](/en/reference/server) and [Client reference](/en/reference/client), and for OAuth and token-cache details see [Auth and extensions reference](/en/reference/auth-and-extensions). No procedural steps on this page.

## Public process API

`include/mcp/process/Process.hpp` declares exactly three symbols: the `SpawnOptions` struct, the abstract `SpawnedProcess` class and the free function `Spawn`. It is the only process header carrying `MCP_API` (the macro is empty in the all-static build, see `include/mcp/Export.hpp`), and its implementation is compiled into `mcp-transport` per platform (see `CMakeLists.txt`).

```cpp
struct MCP_API SpawnOptions { /* 8 fields, see the table below */ };
class MCP_API SpawnedProcess { /* pure-virtual interface, 5 member functions */ };
MCP_API std::unique_ptr<SpawnedProcess> Spawn(const SpawnOptions& options);
```

### `SpawnOptions` field by field

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `SpawnOptions` | `struct MCP_API SpawnOptions` | — | Platform-neutral launch description; no constructor, all fields public |
| `command` | `std::string` | empty (required) | Empty makes `Spawn` throw `McpErrorCode::InvalidParams`; on POSIX a value without `/` is resolved by scanning PATH entry by entry with `access(X_OK)`, on Windows it is handed to `CreateProcessW` which does its own search |
| `arguments` | `std::vector<std::string>` | `{}` | Windows joins them into a single command line with the quote/backslash rules, POSIX builds `argv`; on both platforms the command slot is `command` |
| `working_directory` | `std::string` | empty (inherits the parent's) | Windows widens it and passes `lpCurrentDirectory`; POSIX needs `posix_spawn_file_actions_addchdir_np` (macOS or glibc ≥ 2.29) — without it a non-empty value throws `InternalError` |
| `environment` | `std::map<std::string, std::string>` | `{}` | An empty key or a key containing `=` throws `InvalidParams`; POSIX overrides by exact key, Windows override matching is **case-insensitive** |
| `inherit_environment` | `bool` | `true` | When true the parent environment is merged in; on POSIX, if there is no override at all the inherited `environ` is passed straight to `posix_spawn` |
| `stdout_path` | `std::string` | empty (inherits the parent's stdout) | Non-empty means **truncate and recreate** (POSIX `O_TRUNC` + `0644`, Windows `CREATE_ALWAYS` write-only); redirects stdout only |
| `stderr_path` | `std::string` | empty (inherits the parent's stderr) | Same as above, stderr only; there is no append variant of `stdout_path` |
| `detached` | `bool` | `false` | Windows adds `DETACHED_PROCESS` and `CREATE_BREAKAWAY_FROM_JOB` (retried without breakaway when the latter is denied) and the job object drops `KILL_ON_JOB_CLOSE`; on POSIX, `POSIX_SPAWN_SETSID` creates a new session when available, otherwise a new process group is still created |

### `SpawnedProcess` members

`SpawnedProcess` is abstract; objects are created by platform-internal implementation classes (Windows `WindowsSpawnedProcess`, POSIX `PosixSpawnedProcess`, both in an anonymous namespace and not user-visible types).

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `~SpawnedProcess` | `virtual ~SpawnedProcess() = default` | — | Virtual destructor; `Spawn` returns `std::unique_ptr<SpawnedProcess>`; teardown differs per platform (next two sections) |
| `Pid` | `virtual int64_t Pid() const = 0` | fixed at construction | Child PID, returned unconditionally, no liveness check |
| `IsRunning` | `virtual bool IsRunning() const = 0` | — | Windows uses `WaitForSingleObject(0)` and records the exit code on the side; any result other than `WAIT_TIMEOUT`/`WAIT_OBJECT_0` throws `InternalError`; POSIX reaps with a non-blocking `waitpid(WNOHANG)` |
| `Wait` | `virtual bool Wait(int timeout_ms) = 0` | — | `timeout_ms < 0` waits forever (Windows `INFINITE`, POSIX a 10 ms poll loop); already exited returns `true` immediately, a timeout returns `false` |
| `ExitCode` | `virtual std::optional<int> ExitCode() const = 0` | `std::nullopt` (not exited) | Probes once with zero waiting when not yet exited; on POSIX death by signal yields `128 + signum`, `ECHILD` counts as exited without a code |
| `Terminate` | `virtual bool Terminate(int grace_ms) = 0` | always `true` (unless it throws) | Grace-then-force sequence is platform specific (next two sections); `grace_ms <= 0` skips the grace period |

### `Spawn` and error semantics

| Item | Conclusion |
|---|---|
| Signature | `std::unique_ptr<SpawnedProcess> Spawn(const SpawnOptions& options)`, `options` by const reference, ownership returned |
| Validation | Both platforms share the same `ValidateOptions`: empty `command` → `McpError(McpErrorCode::InvalidParams, "spawn command must not be empty")`; empty environment key or one containing `=` → same code with `"spawn environment key must be non-empty and must not contain '='"` |
| Platform failure | Always `McpError(McpErrorCode::InternalError, ...)`: Windows messages end with `Windows error <N>`, POSIX messages carry `std::strerror` text |
| Redirect open failure | The platform helper throws `std::runtime_error`; `Spawn` narrows it to `McpError(InternalError, what())` and closes the descriptors/handles already opened |
| Production callers | **None.** Repo-wide, `Spawn` only appears in `include/mcp/process/Process.hpp`, `src/process/ProcessWin32.cpp`, `src/process/ProcessPosix.cpp` and the tests `tests/unit/ProcessTests.cpp`, `tests/unit/ProcessTestHelper.cpp` |

Validation, failure handling and redirection are implemented per platform in `src/process/ProcessWin32.cpp` and `src/process/ProcessPosix.cpp`, with the shared low-level helpers in `src/process/detail/WindowsProcessSupport.cpp` and `src/process/detail/PosixProcessSupport.cpp`.

## Platform implementation differences

### Windows: `ProcessWin32.cpp`

| Behaviour | Implementation and semantics |
|---|---|
| Creation entry point | `CreateProcessW` with `bInheritHandles = TRUE`; `dwFlags` always carries `CREATE_UNICODE_ENVIRONMENT`, `CREATE_SUSPENDED`, `CREATE_NEW_PROCESS_GROUP` |
| Standard handles | The three parent standard handles are duplicated via `DuplicateHandle(..., DUPLICATE_SAME_ACCESS)` with the inherit flag set and placed in `STARTF_USESTDHANDLES`; when `stdout_path`/`stderr_path` are set they become file handles instead |
| Suspend, assign, resume | The child is created suspended, then a job object is created and `AssignProcessToJobObject` is called, and only then `ResumeThread`; a failed resume triggers `TerminateProcess` + infinite wait + closing every handle and throws `InternalError` |
| Job object | Always attempted: `kill_on_close = !detached`, `breakaway_ok = true`; a failed creation or assignment only logs a Warning and continues without a job object (no throw) |
| Environment block | UTF-8 → UTF-16 conversion uses `MB_ERR_INVALID_CHARS` (invalid UTF-8 throws); the block ends with two `L'\0'` and is paired with `CREATE_UNICODE_ENVIRONMENT` |
| `Terminate` sequence | With `grace_ms > 0` it first sends `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid)` then `Wait(grace_ms)`; if the child exits within the grace period a `TerminateJobObject(job, 1)` sweeps the rest; otherwise `TerminateWindowsJobOrProcess` (job object → `TerminateJobObject`, none → `TerminateProcess`) followed by `Wait(-1)` |
| Destructor | Non-`detached`: if still running, force-kill + infinite wait + record the exit code; `detached`: does nothing, the child survives; both close the job and process handles |
| Reaping | No `waitpid` needed: the exit code comes from `GetExitCodeProcess`, and whichever of `IsRunning`/`Wait`/`ExitCode` first sees `WAIT_OBJECT_0` pins `exited_` |

### POSIX: `ProcessPosix.cpp`

| Behaviour | Implementation and semantics |
|---|---|
| Creation entry point | `posix_spawn` (not `fork`/`exec`) with a `nullptr`-terminated `argv`; command resolution is a hand-written PATH scan (`argv[0]` stays the original `command`) |
| Process group / session | Non-`detached`: `POSIX_SPAWN_SETPGROUP` + `posix_spawnattr_setpgroup(0)` → the child becomes the leader of its own group (group id equals its PID); `detached` with `POSIX_SPAWN_SETSID` available → new session |
| Signal dispositions | `posix_spawnattr_setsigdefault` restores `SIGPIPE` to its default action so the child does not inherit an ignored disposition |
| Redirection | `adddup2` + `addclose` file actions point `stdout`/`stderr` at files opened `O_WRONLY`, `O_CREAT`, `O_TRUNC` (`0644`, plus `O_CLOEXEC` when available); stdin is always inherited |
| Working directory | Requires `posix_spawn_file_actions_addchdir_np`, compiled in only on macOS or glibc ≥ 2.29; otherwise a non-empty `working_directory` throws `InternalError` (macro `MCP_PROCESS_HAS_SPAWN_CHDIR`) |
| `Terminate` sequence | With `grace_ms > 0` it sends `SIGTERM` to the process group then `Wait(grace_ms)`; if the child exits within the grace period but `kill(-pid, 0)` still finds group members, a `SIGKILL` follows; otherwise `SIGKILL` to the group plus `Wait(-1)` |
| Process-group signalling | `SignalPosixProcessGroup` uses `kill(-pid, sig)` and falls back to the single process `kill(pid, sig)` on `ESRCH` |
| Reaping | Non-blocking `waitpid(pid, WNOHANG)`; `EINTR` retries, `ECHILD` counts as exited (without a code), death by signal maps to `128 + signum`; `Wait` polls every 10 ms instead of using a condition variable |
| Destructor | Non-`detached`: `SIGKILL` the group then reap blocking; `detached`: a separate `detach()`-ed thread reaps blocking so the destructor is never stuck in `waitpid`; both swallow reaping exceptions |

## Boundary with the stdio transport

::: warning
`StdioClientTransport` does **not** go through the public `Spawn`. It only includes `mcp/transport/detail/PlatformIO.hpp` and calls the internal `CreateProcess` with a `ProcessStartInfo` from namespace `mcp::detail` (implementation in `src/transport/StdioClientTransport.cpp`). Those internal symbols (process handle, pipe handles, start-info struct) are not user API and must never be cited as interface; this section only describes the behavioural difference.
:::

| Dimension | Public `Spawn` | Internal stdio transport path |
|---|---|---|
| Declaration site | `include/mcp/process/Process.hpp`, marked `MCP_API` | `include/mcp/transport/detail/PlatformIO.hpp` (a `detail/` internal header, not exported) |
| Standard streams | Reuses the parent handles/descriptors, or sends stdout/stderr to files — **there is no pipe read-back channel** | `pipe()` wires both directions; the parent keeps the stdin write end and the stdout/stderr read ends |
| Low-level creation | Windows `CreateProcessW` (suspended + job + resume); POSIX `posix_spawn` | Windows `CreateProcessW` with `CREATE_NO_WINDOW`, suspended and job-assigned only when `terminate_process_tree`; POSIX `fork` + `execvp`, `setpgid(0,0)` only when that flag is set |
| Termination grace | `Terminate(grace_ms)`: Windows `CTRL_BREAK_EVENT`, POSIX `SIGTERM` to the process group | The internal handle's `Terminate(timeout_ms)`: Windows kills the job/process straight away, POSIX sends `SIGTERM` to the single process (process group only when `terminate_process_tree`) and `SIGKILL` after the timeout |
| Job object | Always attempted (`detached` drops kill-on-close) | Created only when `terminate_process_tree = true`, with `kill_on_close = true`, `breakaway_ok = false` |
| Shared substrate | Both paths **share** the same low-level platform helpers `src/process/detail/PosixProcessSupport.*` and `WindowsProcessSupport.*` (redirect opening, process-group signalling, `waitpid` reaping, command-line building, environment block, job object) | Same as left |
| Ownership and call sites | Compiled into `mcp-transport`, zero production callers, covered only by `tests/unit/ProcessTests.cpp` and `ProcessTestHelper.cpp` | Called by `StdioClientTransport::Connect()` and shaped by `shutdown_timeout` and `terminate_process_tree` |

The internal path's platform side is implemented in `src/transport/detail/win32_platform.cpp` and `src/transport/detail/posix_platform.cpp`.

## Task store

### `TaskStatus` and `TaskState`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `TaskStatus` | `enum class { Pending, Working, Completed, Failed, Cancelled, InputRequired }` | records start as `Pending` | Terminal states are `Completed`/`Failed`/`Cancelled`; once terminal, writes return `false`; persisted as an integer |
| `TaskState` | `struct` | — | The complete task record; `FileTaskStore` serialises it field by field to JSON |
| `task_id` | `std::string` | empty | Written from the `CreateTask` argument |
| `owner` | `std::string` | empty | Session ownership, rewritable via `SetTaskOwner` |
| `status` | `TaskStatus` | `Pending` | The only status field |
| `result` | `std::optional<JsonValue>` | none | What `UpdateTask` stores as the finished result; `FailTask` also writes the error payload here |
| `error_message` | `std::optional<std::string>` | none | Holds the `CancelTask` reason and the `message` field of the `FailTask` payload |
| `input_required` | `std::optional<JsonValue>` | none | Payload snapshot taken when the task parks for MRTR (coexists with `input_requests`; the upper layer decides the semantics) |
| `progress` | `double` | `0` | Not optional, always serialised |
| `progress_total` | `std::optional<double>` | none | Optional total |
| `created_at` | `std::string` | empty → fixed at creation | A millisecond timestamp stored as a **string**; TTL expiry parses it with `ParseMillis` and a parse failure means the record never expires |
| `status_message` | `std::optional<std::string>` | none | Free-text status note |
| `last_updated_at` | `std::optional<std::string>` | none | Refreshed to the current millisecond on every successful write |
| `ttl_ms` | `std::optional<int64_t>` | none | `<= 0` means never expires |
| `poll_interval_ms` | `std::optional<int64_t>` | none | Suggested polling interval, payload only |
| `input_requests` | `std::optional<InputRequests>` | none | Written by `ParkTaskForInput` |
| `input_responses` | `std::optional<JsonValue>` | none | `AdvanceTaskWithInput` merges key by key into the existing object |
| `tool_name` / `tool_arguments` | `std::optional<std::string>` / `std::optional<JsonValue>` | none | Written by `BindTaskTool`; JSON keys are `toolName`/`toolArguments` |

The fields are declared in `include/mcp/server/McpTaskStore.hpp`; serialisation and the TTL decision are in `src/server/FileTaskStore.cpp`.

### `IMcpTaskStore` virtual functions

The class carries **no** `MCP_API` and is a plain C++ abstract interface; a virtual function with a body is an optional capability with a default implementation, so unless it is overridden it returns `false` or an empty container.

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `~IMcpTaskStore` | `virtual ~IMcpTaskStore() = default` | — | Virtual destructor |
| `CreateTask` | `virtual TaskState CreateTask(const std::string& task_id) = 0` | — | The only pure-virtual creation entry point |
| `CreateTask` (overload) | `virtual TaskState CreateTask(task_id, owner, std::optional<int64_t> ttl_ms)` | not pure virtual | Template method: delegates to the one-argument version, then calls `SetTaskOwner`/`SetTaskTtl` when `owner` is non-empty / `ttl_ms` has a value |
| `GetTask` | `virtual std::optional<TaskState> GetTask(const std::string&) = 0` | — | Snapshot lookup by id |
| `UpdateTask` | `virtual bool UpdateTask(const std::string&, const std::optional<JsonValue>& result) = 0` | — | A value in `result` → `Completed`, empty → `Working` |
| `CancelTask` | `virtual bool CancelTask(const std::string&, const std::optional<std::string>& reason) = 0` | — | Sets `Cancelled`, reason lands in `error_message` |
| `SetTaskStatus` | `virtual bool SetTaskStatus(const std::string&, TaskStatus) = 0` | — | Raw status write; moving away from a terminal status is refused |
| `AdvanceTaskWithInput` | `virtual bool AdvanceTaskWithInput(const std::string&, const JsonValue&)` | `false` | MRTR advance, only `InputRequired` tasks can advance |
| `ParkTaskForInput` | `virtual bool ParkTaskForInput(const std::string&, const InputRequests&)` | `false` | MRTR parking |
| `BindTaskTool` | `virtual bool BindTaskTool(const std::string&, const std::string&, const std::optional<JsonValue>&)` | `false` | Records the tool and arguments needed to resume |
| `GetAllTasks` | `virtual std::vector<TaskState> GetAllTasks() = 0` | — | Full enumeration (unordered) |
| `FailTask` | `virtual bool FailTask(const std::string&, const JsonValue& error_payload)` | `false` | Sets `Failed`, payload lands in `result` |
| `RemoveTask` | `virtual bool RemoveTask(const std::string&)` | `false` | Physical deletion, unlike `CancelTask` |
| `GetExpiredTaskIds` | `virtual std::vector<std::string> GetExpiredTaskIds(int64_t now_ms)` | `{}` | Enumerates only, never deletes |
| `SetTaskOwner` | `virtual bool SetTaskOwner(const std::string&, const std::string&)` | `false` | Rewrites ownership |
| `SetTaskTtl` | `virtual bool SetTaskTtl(const std::string&, int64_t ttl_ms)` | `false` | Rewrites the TTL |

Every interface above is declared in the same `include/mcp/server/McpTaskStore.hpp`.

Integration point: `ServerOptions` holds the store in its `task_store` field as `std::shared_ptr<IMcpTaskStore>` (declared in `include/mcp/server/ServerOptions.hpp`), and `McpServer::SweepExpiredTasks` is the only reclamation path — `GetExpiredTaskIds` followed by `RemoveTask`, with exceptions from either call logged as Error only (declared in `include/mcp/server/McpServer.hpp`, implemented in `src/server/McpServer.cpp`).

### `FileTaskStore`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| Constructor | `explicit FileTaskStore(std::filesystem::path storage_path)` | — | The argument is a **single journal file path** (not a directory); loading happens in the constructor and a missing file means an empty store |
| Destructor | `~FileTaskStore() override` | — | Calls `Flush()` → full atomic rewrite |
| 15 overrides | Same names and signatures as `IMcpTaskStore`, all `override` | — | No default implementation survives: including `AdvanceTaskWithInput`/`ParkTaskForInput`/`BindTaskTool`/`FailTask`/`RemoveTask`/`GetExpiredTaskIds`/`SetTaskOwner`/`SetTaskTtl` |
| Write semantics | Update functions return `bool` | `false` | Unknown id → `false`; already terminal → `false` (`SetTaskOwner`/`SetTaskTtl`/`BindTaskTool` have no terminal guard); persistence failure → in-memory rollback plus `McpError(InternalError)` |
| Create semantics | `CreateTask` | — | A duplicate id throws `McpError(McpErrorCode::InvalidParams, "task already exists: " + id)` |
| Concurrency model | `write_mutex_` serialises writes + `shared_mutex` for readers | — | Readers take only the shared lock; writers hold `write_mutex_` for the whole call |
| Journal format | One JSON line per record: `{"task_id":...,"state":{...}}`; deletion appends `{"task_id":...,"removed":true}` | — | Append-only; replay overwrites or erases line by line |
| Compaction trigger | `pending_records_ > kMinCompactThreshold + 2 × live task count` | `kMinCompactThreshold = 64` | When met, the whole file is rewritten right after the append: temporary file + `fsync`/`FlushFileBuffers` + atomic `rename` (internal helper in `src/detail/AtomicJsonFile.cpp`), counter reset to zero |
| Corruption handling | A bad line in the middle → copy a `.corrupt` backup with the same name and **clear the whole store**; a truncated trailing line is only logged and ignored | — | The legacy `{"tasks":{...}}` snapshot format is still read |
| Owning target | `mcp-server` static library | — | Example usage: `examples/conformance/server/SkillsAndTasks.cpp` |

The interface is declared in `include/mcp/storage/FileTaskStore.hpp`; appending, replay, compaction and corruption handling are all in `src/server/FileTaskStore.cpp`.

## Event and session stores

### `EventStore`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `EventStore` | `class EventStore` (virtual destructor) | — | Overridable in-process base class, not pure virtual; no `MCP_API` |
| `Append` | `virtual uint64_t Append(std::string_view session_id, std::string event_data)` | — | Monotonically increasing global id starting at `1` (one counter shared across sessions); `event_data` is moved in by value |
| `GetEventsSince` | `virtual std::vector<std::pair<uint64_t, std::string>> GetEventsSince(std::string_view, uint64_t last_event_id) const` | empty | Returns every event with `id > last_event_id`, in order |
| `Clear` | `virtual void Clear(std::string_view session_id)` | — | Drops the whole session and does not recycle ids |
| `kMaxEventsPerSession` | `static constexpr size_t` | `10000` | Excess events are trimmed from the front |
| Integration point | `StreamableHttpServerOptions::event_store` is a `std::shared_ptr<EventStore>` | unset | `Last-Event-ID` replay in session mode relies on it |

The interface is declared in `include/mcp/http/EventStore.hpp`, the in-process implementation and trimming in `src/http/EventStore.cpp`, and the injection field in `include/mcp/transport/StreamableHttpServerTransport.hpp`.

### `FileEventStore`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| Constructor | `explicit FileEventStore(std::filesystem::path storage_dir)` | — | The argument is a **directory** (the opposite of `FileTaskStore`); a failing `create_directories` throws `std::runtime_error` |
| `Append` | `override uint64_t` | — | Appends one JSONL line per event; the id is `max_id + 1` within that session file (the base-class counter is bypassed); a missing trailing newline is added first |
| `GetEventsSince` | `override std::vector<std::pair<uint64_t, std::string>> const` | empty | Rescans the whole file and filters `id > last_event_id` |
| `Clear` | `override void` | — | Removes the session file (the lock file stays), failure throws `std::runtime_error` |
| File naming | `<storage_dir>/sess-<escaped id>.jsonl` plus a sibling `.lock` | — | Characters outside `[A-Za-z0-9_-]` are escaped as `~hh`, which rules out path traversal |
| Cross-process serialisation | Every operation first takes an exclusive file lock (POSIX `flock`, Windows `LockFileEx` waiting indefinitely) | — | Failing to open or lock throws `std::runtime_error("event store: failed to ...")` |
| Trimming | When the count exceeds `EventStore::kMaxEventsPerSession` the whole file is rewritten atomically | 10000 | Keeps the last 10000 events; a failed write throws |

The interface is declared in `include/mcp/storage/FileEventStore.hpp`; escaping, file locking, appending and trimming are all in `src/http/FileEventStore.cpp`.

### `SessionStore` and `InMemorySessionStore`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `SessionRecord` | `struct { std::string protocol_version; int64_t created_at_ms; int64_t last_seen_ms; }` | both timestamps `0` | The minimum payload needed to take over a session across processes |
| `Save` / `Load` / `Remove` | pure virtual `void` / `std::optional<SessionRecord>` / `void` | — | The three mandatory operations |
| `TrySave` | `virtual bool TrySave(const std::string&, const SessionRecord&)` | delegates to `Save`, returns `true` | The non-throwing path when the store is full — `InMemorySessionStore::Save` uses it for the capacity check |
| `Size` / `SweepExpired` | `virtual size_t Size() const` / `virtual size_t SweepExpired()` | `0` | Optional implementations; the base returns `0` |
| `InMemorySessionStore` | `class InMemorySessionStore : public SessionStore` | — | In-process implementation overriding all 6 operations |
| `Save` (in-memory) | `override void` | — | Goes through `TrySave` and throws `std::runtime_error("session store is full")` when capacity is reached |
| `TrySave` (in-memory) | `override bool` | — | Sweeps expired records first, then rejects once `records_.size() >= detail::kMaxHttpSessions`; a `last_seen_ms == 0` is filled with the current millisecond |
| `Load` (in-memory) | `override std::optional<SessionRecord>` | `nullopt` | An expired hit is erased and returned as empty; a live hit refreshes `last_seen_ms` (reading writes) |
| `Size` / `SweepExpired` (in-memory) | `override` | — | Locked `records_.size()` / the number erased in this round |
| Expiry rule | `now - max(last_seen_ms, created_at_ms) > detail::kSessionIdleTimeoutMs` | 30 minutes (1800000 ms) | A record whose two timestamps are both `0` never expires |
| Capacity limit | `detail::kMaxHttpSessions` | `10000` | When full `TrySave` returns `false` and `Save` throws |
| Integration point | `StreamableHttpServerOptions::session_store` is a `std::shared_ptr<SessionStore>` | unset | Session takeover and reclamation are injected through this Options field |

The interface and the in-memory implementation share one header, `include/mcp/http/SessionStore.hpp`; the two capacity and timeout constants are in `include/mcp/transport/detail/Limits.hpp` and the injection field in `include/mcp/transport/StreamableHttpServerTransport.hpp`.

## HTTP server substrate

The transport-side usage of `HttpServer`/`EventStore`/`SessionStore` and the full transport Options are owned by the [Transports reference](/en/reference/transports); this section lists only the substrate's own interface, defaults and implementation behaviour.

### `HttpRequest`, `HttpResponse` and `HttpServerOptions`

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| `HttpRequest` | `struct { method; path; headers; body }`, headers as `unordered_map<string,string>` | all empty | Handed over after parsing; header names are lower-cased |
| `HttpResponse` | `struct` with 7 fields | `status_code{200}`, `status_text{"OK"}`, `is_sse{false}`, `sse_close_after_write{false}` | The last two SSE fields control "close after writing" and the targeted-delivery tag |
| `HttpHandler` | `std::function<void(const HttpRequest&, HttpResponse&)>` | — | Route body; fills `HttpResponse` in place |
| `on_request` | `HttpRequestCallback` (`void(const HttpRequest&)`) | unset | Called for every successfully parsed request, before routing; exceptions are swallowed and logged as Warning |
| `on_connect` | `HttpConnectCallback` (`void()`) | unset | Fires once per accepted connection; exceptions are swallowed |
| `on_disconnect` | `HttpDisconnectCallback` (`void()`) | unset | Fires only when an SSE client entry is **actually removed** (including the automatic removal after a failed send) |
| `allowed_hosts` | `std::vector<std::string>` | `{}` (loopback only) | A missing `Host` header is rejected; when non-empty each entry must match **exactly**, when empty only `localhost`/`127.0.0.1`/`::1` are accepted |
| `allowed_origins` | `std::vector<std::string>` | `{}` | A request without an `Origin` header is allowed; when non-empty matching is exact, when empty the Origin host must equal the Host host or both must be loopback |
| `sse_keep_alive_ms` | `int` | `0` (no heartbeat thread) | Only `> 0` starts the KeepAlive thread, which heartbeats every SSE client at that interval |
| `bind_host` | `std::string` | `""` → `INADDR_ANY` | Accepts bracketed IPv6 literals; an unparseable value throws `std::runtime_error("HttpServer: invalid bind_host: ...")`; the `listen` backlog is fixed at 16 |
| `max_request_body_bytes` | `std::optional<std::size_t>` | `detail::kMaxHttpBodyBytes` (4 MiB) | Decided from `Content-Length` before reading, over the limit answers 413; `nullopt` falls back to the same constant |

The fields are declared in `include/mcp/http/HttpServer.hpp`; Host/Origin validation, the callback trigger points and the body-size decision are in `src/http/HttpServerImpl.cpp`.

### `HttpServer` public interface

| Symbol | Signature/Type | Default | Notes |
|---|---|---|---|
| Constructor | `HttpServer(uint16_t port, const HttpServerOptions& options = {})` | default-constructed options | Stores configuration only, binds no port |
| `~HttpServer` | Declared implicitly, calls `Stop()` | — | Destruction stops the server |
| `Start` | `void Start()` | — | Idempotent (atomic exchange); on failure it resets the flag and rethrows; the `SetHandler` table is snapshotted at this moment |
| `Stop` | `void Stop()` | — | Idempotent; the underlying stop runs on a helper thread so a caller never joins itself, via `JoinThreadSafely` |
| `SetHandler` | `void SetHandler(std::string_view method, std::string_view path, HttpHandler handler)` | — | Exact `(method, path)` match; calling it after `Start()` throws `std::logic_error` |
| `SseClientId` | `using SseClientId = uint64_t` | incremented from `1` inside the server | **`AddSseClient` returns `0` when the server is not started**, so `0` is the only failure sentinel |
| `AddSseClient` | `SseClientId AddSseClient(std::function<void(std::string_view)> send_fn[, std::string tag])` | no tag | Two overloads; registering wakes the heartbeat thread |
| `RemoveSseClient` | `void RemoveSseClient(SseClientId id)` | — | A no-op when not started; `on_disconnect` fires only on a real removal |
| `BroadcastSse` | `void BroadcastSse(std::string_view event[, const std::function<bool(const std::string&)>& accept])` | send to all | The predicate filters by tag and a throwing predicate is treated as `true`; events longer than `detail::kMaxSseEventBytes` (1 MiB) are truncated |
| `SseClientCount` | `std::size_t SseClientCount() const` | `0` | Returns `0` when not started |

The interface is declared in `include/mcp/http/HttpServer.hpp`; `src/http/HttpServer.cpp` only forwards, the real scheduling is in `src/http/HttpServerImpl.cpp` and `src/http/HttpServerImpl.hpp`.

::: tip
The decision order is: `on_request` callback → every `OPTIONS` request answered 204 (short-circuited **before** the Host/Origin check and never looking up the route table) → Host/Origin failure 403 → unmatched route 404, see `src/http/HttpServerImpl.cpp`.
:::

## Token cache

`ITokenCache` is the pluggable persistence contract for client OAuth tokens (`StoreTokens`/`GetTokens`/`ClearTokens` are pure virtual, `StoreBoundIssuer`/`GetBoundIssuer` come with default implementations), `InMemoryTokenCache` is the in-process implementation and `FileTokenCache` the single-file one — field semantics, expiry rules and issuer binding are owned by the [Auth and extensions reference](/en/reference/auth-and-extensions); this page only fixes the boundary and the on-disk facts.

| Symbol | Role | Declaration site and details |
|---|---|---|
| `ITokenCache` | Contract and `TokenContainer` field semantics | Declared in `include/mcp/client/auth/TokenCache.hpp`; details in the [Auth and extensions reference](/en/reference/auth-and-extensions) |
| `FileTokenCache` | Single-file implementation inside the `mcp-client` target: the constructor takes a **file path**, `Save()` deletes the file when `tokens_` is empty; Windows encrypts with `CryptProtectData` before writing and POSIX writes plaintext, both through the owner-only atomic write (helper in `src/detail/AtomicJsonFile.cpp`), and on POSIX loading refuses a file owned by someone else or readable by group/other; the issuer anchor lives in `<cache_path>.issuer` and an empty issuer removes the anchor | Declared in `include/mcp/storage/FileTokenCache.hpp`, implemented in `src/client/FileTokenCache.cpp`, target membership in `CMakeLists.txt`; details in the [Auth and extensions reference](/en/reference/auth-and-extensions) |

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
