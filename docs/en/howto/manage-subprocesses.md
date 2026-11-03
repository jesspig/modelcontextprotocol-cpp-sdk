# Keep subprocesses under control

Connecting to a local MCP server over stdio means you actually started a process, and its input, output, termination and reaping are your problem. This page covers two things: **what `mcp::Spawn` does** (a generic, protocol-independent child process) and **what the stdio transport does on its own** (and the fact that it does not listen to `SpawnOptions`). Transport selection is in [/en/learn/transports](/en/learn/transports).

::: info Two paths, not one
The stdio transport creates its process through the internal platform layer and **does not go through** `mcp::Spawn`; the two share low-level platform helpers but have separate entry points and option structs. Do not expect fields in `SpawnOptions` to influence stdio behaviour, and do not expect the stdio transport to manage a non-MCP process — that job is what `Spawn` is for.
:::

## Prerequisites

- Headers: `mcp/process/Process.hpp` (public `Spawn`), `mcp/transport/StdioClientTransport.hpp`, `mcp/Transport.hpp`, `mcp/client/McpClient.hpp`, `mcp/client/ClientOptions.hpp`, `mcp/Implementation.hpp`; include paths must carry the subdirectory.
- Platform: Windows terminates trees with a Job Object, POSIX uses `posix_spawn` plus its own process group and a blocking reap (no shell involved, argument boundaries preserved verbatim).
- No CMake switch needed; `Spawn` lives in `mcp-core`, the stdio transport in `mcp-transport`.
- The target executable must really be reachable from the path resolved against `working_directory` — `command` expands no shell variables and performs no magic beyond normal lookup.

## Start a protocol-independent child process

Eight fields on `SpawnOptions` plus five methods on `SpawnedProcess` (`include/mcp/process/Process.hpp`): `Pid()`, `IsRunning()`, `Wait(timeout_ms)`, `ExitCode()`, `Terminate(grace_ms)`. An empty `command` throws immediately.

<<< @/snippets/manage-subprocesses.cpp#spawn-plain

`Wait` returning `false` means the child did not exit within the window; `Terminate(grace_ms)` then gives it `grace_ms` to leave politely before killing it. With `detached = true` the child outlives the launcher (behaviour guarded by `SpawnTest.DetachedChildSurvivesLauncherExit`).

## The full stdio transport option set

Actual fields of `StdioClientTransportOptions` (`include/mcp/transport/StdioClientTransport.hpp`):

| Field | Type / default | Purpose |
|---|---|---|
| `command` | `std::string` | executable |
| `arguments` | `vector<string>` | passed verbatim, never through a shell |
| `name` | `""` | transport name; `Name()` falls back to `"stdio"` when empty |
| `working_directory` | `""` | child working directory |
| `inherit_environment_variables` | `true` | inherit the current environment |
| `environment_variables` | `map<string,string>` | added or overriding variables |
| `stderr_path` | `""` | file to drain stderr into |
| `on_stderr` | `function<void(string_view)>` | stderr callback |
| `shutdown_timeout` | `2000ms` | grace period for the child when closing |
| `terminate_process_tree` | `false` | also terminate descendants |
| `max_frame_bytes` | `kMaxStdioFrameBytes` | per-frame size limit |

<<< @/snippets/manage-subprocesses.cpp#stdio-options

A frame exceeding `max_frame_bytes` is **dropped and reported as an error** (the buffer is cleared, then the error notification fires — `src/transport/StdioClientTransport.cpp`) instead of growing memory without bound.

## Capturing stderr: callback or file, never both

::: danger Giving both throws
`stderr_path` and `on_stderr` are mutually exclusive; setting both makes `Connect()` throw `McpError(InvalidParams)` with the message `stderr_path and on_stderr are mutually exclusive` (`src/transport/StdioClientTransport.cpp`).
:::

<<< @/snippets/manage-subprocesses.cpp#stderr-drain

<<< @/snippets/manage-subprocesses.cpp#stderr-file

Draining runs on a **dedicated thread** (named `mcp-stderr`), and two implementation details matter:

- Reading 0 bytes does not necessarily mean the end: the POSIX `Read` polls with a 100 ms timeout and returns 0 when nothing arrived, so real EOF is decided by `IsEof()` (`include/mcp/transport/detail/PlatformIO.hpp`). Idle periods are not mistaken for disconnection.
- **Exceptions from your callback never propagate**: both `std::exception` and unknown exceptions are caught and logged at Error level. A throwing callback loses that chunk of output but does not take the reader thread down with it.

The callback is invoked directly from the draining thread, so it must be **fast**: push into your own queue, do not perform blocking IO or call `Close()` inside it.

## Process-tree termination and shutdown grace

With `terminate_process_tree = true`:

- Windows: the child is assigned to a Job Object configured with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, and termination calls `TerminateJobObject` (`src/process/detail/WindowsProcessSupport.cpp`), so grandchildren go too.
- POSIX: the child leads its own process group; termination signals the group and reaps blocking, leaving no zombie (`src/transport/detail/posix_platform.cpp` and `src/process/ProcessPosix.cpp`, guarded by `SpawnTest.PosixChildLeadsOwnProcessGroupAndIsReaped` and `TransportTest.StdioProcessReapedWithoutZombie`).

`shutdown_timeout` is precisely that polite-exit window: the close path hands it straight to the process termination call (`src/transport/StdioClientTransport.cpp`). **The POSIX grace period is measured on a real clock**, not by counting loop iterations — on macOS a 10 ms sleep measured roughly 5x its nominal value, so an iteration-based grace turned 300 ms into about 1.7 s (`src/transport/detail/posix_platform.cpp`).

## Shutdown order and threading traps

<<< @/snippets/manage-subprocesses.cpp#shutdown-order

- `client->Close()` first, then release the transport object; the `StdioClientTransport` object must outlive both the session it produced via `Connect()` and the client built on it, and `Pid()` is read from that same object (`include/mcp/transport/StdioClientTransport.hpp`).
- **Never call `Close()` from inside a transport callback**: stdio runs user callbacks directly on its read thread, so `Close()` there joins the calling thread. Every `Close()` belongs on a different thread; inside the SDK this is handled by `detail::JoinThreadSafely` (`src/transport/StdioClientTransport.cpp`).

## Health checks and restarts

<<< @/snippets/manage-subprocesses.cpp#watchdog

`Pid()` only has a value after `Connect()` (`std::optional<int64_t>`) — it is read from the transport object, so the order of "establish the session, then read the PID" cannot be swapped:

<<< @/snippets/manage-subprocesses.cpp#connect-and-pid

It answers "was a process ever started", **not** "is it still alive" — for liveness use a protocol-level `Ping()` (available in the legacy era; deprecated in the modern one, see [/en/howto/handle-server-initiated-requests](/en/howto/handle-server-initiated-requests)) or watch `ClientOptions::on_transport_error`. The idle-misjudgement trap is in the section above; `on_transport_close` and `on_transport_error` semantics are in [/en/howto/connect-to-a-server](/en/howto/connect-to-a-server).

## Complete code

<<< @/snippets/manage-subprocesses.cpp#full

The output parameter in `StartEchoServer` is deliberate: the caller must hold the `shared_ptr<StdioClientTransport>` until the client has finished closing, otherwise the transport is destroyed first and the session becomes dangling.

## Verification

Snippets are syntax-checked only (clang-cl `/Zs`, GCC/Clang `-fsyntax-only`):

```bash
cmake -S . -B build/p3a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3a --target mcp-doc-snippet-manage-subprocesses
```

Expected: configure prints `[mcp-docs] syntax-checking 16 snippet(s) with '/Zs'`, the build prints one `Building CXX object ...manage-subprocesses.cpp.obj` line, return code 0 with no `error:` lines.

Behaviour is guarded by existing cases; first run `git submodule update --init third_party/googletest`, then `cmake --preset debug && cmake --build --preset debug`, then:

```bash
ctest --preset debug -R 'SpawnTest|TransportTest.Stdio'
```

Coverage: argument boundaries without a shell, environment inheritance and overrides, working directory, both streams redirected and truncated, `Wait` timeout then exit code, non-zero exit codes, tree termination, detached survival, empty command rejected, concurrent spawns from several threads, POSIX process group and reaping, stderr capture and callback, `Pid()` availability, concurrent connects, shutdown grace, reaping without zombies.

## Era differences

Process lifecycle lives below the transport and is unrelated to the protocol era:

| Item | legacy (≤ 2025-11-25) | modern (2026-07-28) |
|---|---|---|
| stdio spawn / terminate / reap | identical | identical |
| `stderr_path` vs `on_stderr` exclusivity | identical | identical |
| Cancellation transport shape | stdio sends `notifications/cancelled` | stdio still sends the notification (only transports implementing `IRequestCanceller` close the stream), see [/en/howto/cancel-a-request](/en/howto/cancel-a-request) |

## Common mistakes

| Symptom | Cause | Fix |
|---|---|---|
| `Connect()` throws `InvalidParams` mentioning mutual exclusion | both `stderr_path` and `on_stderr` set | pick one: a file if you want it on disk, a callback if you want it in memory |
| After an idle stretch the child is judged to have exited | your own read loop treats `Read()` returning 0 as EOF | always consult `IsEof()` too |
| Process still there but the connection is unusable | liveness inferred from `Pid()` alone | probe at the protocol level or watch the transport error callback |
| Grandchildren survive after closing the client | `terminate_process_tree` defaults to `false` | turn it on explicitly when you need tree termination |
| A throwing callback lost part of the output | callback exceptions are caught and logged, not replayed | keep fallible heavy work out of the callback |
| `Close()` inside your stderr/read callback wedges | the IO thread joins itself | post the close action to another thread |
| Trying to hand `SpawnOptions` to stdio | they are not the same path | stdio uses `StdioClientTransportOptions`; generic processes use `Spawn` |
| Slow startup: 1.7 s to close on macOS | the old implementation estimated grace by iteration count | it now uses a real clock deadline; simply set a smaller `shutdown_timeout` |

## Next steps

- Transport choice, stateful vs stateless: [/en/learn/transports](/en/learn/transports)
- What happens after connecting, and when to close: [/en/howto/connect-to-a-server](/en/howto/connect-to-a-server)
- The equivalent assembly on the server side: [/en/howto/run-a-streamable-http-server](/en/howto/run-a-streamable-http-server)
- Full field tables: [/en/reference/transports](/en/reference/transports), [/en/reference/process-and-storage](/en/reference/process-and-storage)

## Source behind this page

- `include/mcp/process/Process.hpp` (`SpawnOptions`, `SpawnedProcess`, `Spawn`)
- `include/mcp/transport/StdioClientTransport.hpp` (`StdioClientTransportOptions`, `Pid()`)
- `include/mcp/transport/detail/PlatformIO.hpp` (internal process/pipe abstraction and `IsEof()`)
- `src/transport/StdioClientTransport.cpp` (exclusivity check, stderr drain thread, frame limit, close order)
- `src/process/detail/WindowsProcessSupport.cpp` (Job Object tree termination)
- `src/process/ProcessPosix.cpp` (`posix_spawn`, process group, reaping)
- `src/transport/detail/posix_platform.cpp` (real-clock shutdown grace)
- `tests/unit/ProcessTests.cpp`, `tests/unit/TransportTests.cpp`
