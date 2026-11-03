# Turn a Slow Tool into a Task

Add MCP task semantics to a long-running tool: `tools/call` returns a `taskId` immediately, the real work continues in the background, and the client polls with `tasks/get`, cancels cooperatively with `tasks/cancel`, and resumes after `input_required` with `tasks/update`. Signature and field tables live only in [/en/reference/server](/en/reference/server), [/en/reference/client](/en/reference/client) and [/en/reference/types](/en/reference/types); this page only shows how to wire them together.

## Prerequisites

- Headers: `mcp/server/McpServer.hpp`, `mcp/server/ServerOptions.hpp`, `mcp/server/McpTaskStore.hpp`, `mcp/server/RequestContext.hpp`, `mcp/storage/FileTaskStore.hpp`, `mcp/client/McpClient.hpp`, `mcp/client/ClientOptions.hpp`, `mcp/McpTypes.hpp`, `mcp/JsonValue.hpp`, `mcp/Content.hpp`, `mcp/Implementation.hpp`, `mcp/transport/InMemoryTransport.hpp`; always write the full subdirectory path.
- Flags: server side needs `ServerOptions::enable_tasks_extension = true` **and** a non-null `ServerOptions::task_store` — neither alone is enough; client side needs `ClientOptions::enable_tasks_extension = true`.
- Protocol era: tasking is discussable across all five versions, but 2026-07-28 (modern) additionally requires the client to declare the `io.modelcontextprotocol/tasks` extension — see "Era differences".
- Boundary with elicitation: this page covers only "the task parks at `input_required` and resumes via `tasks/update`"; blocking on a reverse request inside the handler belongs to [/en/howto/request-extra-input](/en/howto/request-extra-input) and [/en/howto/collect-user-input](/en/howto/collect-user-input).

## Declare task execution

The only entry point for tasking is the tool's own `ToolOptions::execution`: set `mode` to `ToolExecutionMode::Task`. `task_support` decides what happens when the client does not cooperate — `Optional` (the default) may degrade to synchronous execution, `Required` must become a task or it returns a capability error.

`execution` has no fluent setter and must be assigned wholesale; the remaining fields still use `Description()`/`InputSchema()`.

<<< @/snippets/run-long-tasks.cpp#task-declaration

The decision point is `src/server/McpServer.cpp`: only `tool_def.execution && mode == Task` enters the task branch. `Auto`/`Manual` only affect the `execution` declaration advertised in `tools/list` and never change how the server executes.

## Enable the extension and inject the store

The inbound handlers for `tasks/get`, `tasks/update`, `tasks/cancel`, `tasks/result` and `tasks/list` are **registered only when `options_.task_store` is non-null**, and the first thing each handler does is check `enable_tasks_extension`. In other words: hand it a `task_store` without the flag and the task methods still answer `MethodNotFound: tasks extension is disabled` (`McpServer.cpp` for the `if (store)` guard, plus the three gates); set the flag without a store and `tools/call` never enters the task branch, so the tool still runs synchronously.

The built-in implementation is `FileTaskStore` (JSON on disk, append-only log plus compaction). Rolling your own `IMcpTaskStore` is allowed, but only six pure virtuals must be implemented — `CreateTask/GetTask/UpdateTask/CancelTask/SetTaskStatus/GetAllTasks` — the rest ship with default bodies (`include/mcp/server/McpTaskStore.hpp`).

<<< @/snippets/run-long-tasks.cpp#task-enable

Task IDs look like `task-<session_id>-<16 random hex bytes>` (`McpServer.cpp`) and the owner is anchored to the session that issued `tools/call`; `ttl_ms` always takes the default 300000 (`McpServer.cpp`), and expired tasks are swept as a side effect of every creation (`McpServer.cpp`, `SweepExpiredTasks`).

## Check for cancellation inside the handler

The handler receives the same `RequestContext`, so a single `IsCancellationRequested()` check at each loop boundary is enough. The flag is a `std::atomic<bool>` held by shared pointer: ordinary (non-task) tools get the per-request flag fed by the protocol-level `notifications/cancelled` (`McpServer.cpp`), task tools get the `running_task_cancel_flags_[taskId]` entry written by `tasks/cancel` (`McpServer.cpp`); both paths funnel into the same predicate.

<<< @/snippets/run-long-tasks.cpp#task-cancellation

Cancellation is cooperative: if the handler never reads the flag it runs to completion. A task already in a terminal state (`completed`/`failed`/`cancelled`) does not transition again on `tasks/cancel` — the server just returns an empty ack (`McpServer.cpp`); a cancellation that has not landed yet is resolved as `cancelled` before it is resolved as `failed` when the task wraps up (`McpServer.cpp`).

## Poll to completion on the client

`CallToolAsTask` reads the `tools/call` task receipt into a `GetTaskResult` handle (only `task_id` and `status`, no result body) and hands it to `PollTaskToCompletion`, which polls `tasks/get` at the server-suggested `pollIntervalMs`.

<<< @/snippets/run-long-tasks.cpp#task-poll

Three things to watch:

- Terminal states are the wire strings `completed`/`failed`/`cancelled` (`McpClient.cpp`); neither `working` nor `input_required` is terminal.
- `PollTaskToCompletion` waits for a terminal state only, so a task parked at `input_required` polls until it times out and throws `InternalError` (`McpClient.cpp`) — tasks that may park need your own `GetTask` branch.
- If the server actually finished synchronously (for instance a modern client that did not declare the tasks extension, so an `Optional` tool degraded), `CallToolAsTask` returns a degraded handle with `task_id == "<none>"` and `status == "completed"`, and the result body is already in `result` (`McpClient.cpp`) — do not feed that id to `tasks/get`.

## Cancel a task deliberately

<<< @/snippets/run-long-tasks.cpp#task-cancel

`CancelTask` returns an ack as soon as the shared flag is set; it does not wait for the executor to actually stop. To confirm where it stopped, call `GetTask` or `PollTaskToCompletion` once more. `tasks/cancel` only honours the owner session, and reaching another session's task produces exactly the same error as reaching a task that never existed: `InvalidParams: task not found: <taskId>` (`McpServer.cpp`).

## Resume a task from input_required

When the handler returns `CallToolResult::input_required`, the server parks the task at `input_required` and records the `input_requests`; the client then calls `tasks/update` (with `inputResponses` only), `AdvanceTaskWithInput` merges the new responses key by key into what is already stored, and the task goes back to `working`.

<<< @/snippets/run-long-tasks.cpp#task-input-required

`input_responses` is an object indexed by the keys of `input_requests`, and each entry has the `ElicitResult` shape: `{"action":"accept","content":{...}}`. Every time the handler wakes up it must re-decide "which items are still missing" — the responses collected in earlier rounds are handed back verbatim, so the check order must be "look at what you already have, then ask only for what is absent", otherwise the task loops forever on the same question.

Three hard constraints (`McpServer.cpp`): `result` and `inputResponses` are mutually exclusive and supplying both returns `InvalidParams`; a task that is not at `input_required` returns `InvalidParams: task is not waiting for input`; in the modern era the resume **re-invokes the tool in place** and puts the final result in this very `tasks/update` response, while the legacy era returns only an ack and the final result still has to be fetched with `tasks/get`/`tasks/result`.

## Complete code

Server and client each enable the extension, the task tool is registered, a task is started and polled to a terminal state, and finally a running task is cancelled cooperatively:

<<< @/snippets/run-long-tasks.cpp#full

## Verification

Snippets are syntax-checked only (clang-cl uses `/Zs`, GCC/Clang use `-fsyntax-only`); no process needs to start:

```bash
cmake -S . -B build/p2d -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p2d --target mcp-doc-snippet-run-long-tasks
```

Expected output (measured locally): configure prints `[mcp-docs] syntax-checking <N> snippet(s) with '/Zs'` and the build prints

```text
[1/4] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-run-long-tasks.dir\__\__\docs\snippets\run-long-tasks.cpp.obj
```

with exit code 0 and no `error:` line (`clang-cl: warning: argument unused during compilation: '-c'` is inherent to this mode and can be ignored).

Behaviour is guarded by these existing cases; run `git submodule update --init third_party/googletest` first, then `ctest --preset debug -R '<case name>'` (names and owning files in `tests/unit/McpServerTests.cpp`, `tests/unit/McpClientTests.cpp`): `McpServerTest.TasksRequireExplicitExtensionFlag`, `McpServerTest.TasksMethodsGateWithoutExtension`, `McpServerTest.CrossSessionTaskAccessIsRejected`, `McpServerTest.ExpiredTasksAreReclaimedWhenNewTaskIsCreated`, `McpServerTest.CallToolTaskModeCancelsRunningTaskCooperatively`, `McpServerTest.TerminalTaskCannotBeRegressedByUpdateOrCancel`, `McpServerTest.TaskIdentifiersAreSessionScopedAndNotSequential`, `McpServerTest.TasksUpdateResumeCompletesParkedTask`, `McpClientTest.PollTaskViaTasksGet`, `McpClientTest.CallToolAsTaskDegradesSynchronousResult`.

## Era differences

| Item | legacy (≤ 2025-06-18) | modern (2026-07-28) |
|---|---|---|
| `tasks/get`/`tasks/update`/`tasks/cancel` | available | available, but the client must declare `io.modelcontextprotocol/tasks`, otherwise `MissingRequiredClientCapability` (`-32021`) |
| `tasks/result`, `tasks/list` | available | **no such method**, answers `MethodNotFound` (`McpServer.cpp`) |
| `tools/call` task receipt shape | flat `taskId/status/createdAt` plus a nested `task` object | flat fields only, the nested `task` is erased (`McpServer.cpp`) |
| Task status notification routing | `SendNotification` broadcast | `NotifySubscribers` per subscription with `_meta.subscriptionId` required (`McpServer.cpp`, `McpSessionHandler.cpp`) |
| `Optional` task tool + client without the extension | still becomes a task | degrades to synchronous execution (`McpServer.cpp`) |
| `Required` task tool + client without the extension | still becomes a task | capability error, after argument validation |

The wire status strings are identical in both eras: `working`, `input_required`, `completed`, `failed`, `cancelled`; the C++ `TaskStatus::Pending` never reaches the wire and always maps to `working` (`McpServer.cpp`).

## Common mistakes

| Symptom | Cause | Fix |
|---|---|---|
| Every `tasks/*` answers `MethodNotFound: tasks extension is disabled` | `task_store` supplied without `enable_tasks_extension` | Set both; the store and the flag are jointly necessary |
| `tools/call` finishes synchronously with no `taskId` | Flag set but `task_store` null, or the tool never set `execution.mode = Task` | Supply the store, and confirm `execution` was assigned wholesale rather than mutated on a local copy |
| Client throws `MethodNotFound` immediately from `PollTaskToCompletion`/`UpdateTask` | `ClientOptions::enable_tasks_extension` unset (the client blocks it locally, see `McpClient.cpp`) | Enable it on the client too; note `GetTask`/`CancelTask` skip that check, so do not use them as a probe |
| Modern call answers `-32021` with `data.requiredCapabilities.extensions` naming `io.modelcontextprotocol/tasks` | The tool is `Required` but the client never declared the tasks extension | Declare the extension in `ClientOptions::capabilities`/`extensions` |
| `GetTask` answers `InvalidParams: task not found: <id>` | The task belongs to another session, or it aged past `ttl_ms` and was swept | Poll from the same session; for a longer lifetime implement `SetTaskTtl` yourself |
| Status stays `completed` after a cancel | The cancellation arrived after the executor passed its last checkpoint | Expected: terminal states do not migrate; put checkpoints before genuinely expensive boundaries |
| Task parks at `input_required` until the poll times out with `InternalError` | `PollTaskToCompletion` waits for a terminal state and `input_required` is not one | Branch on `GetTask` yourself, submit `tasks/update`, then keep polling |
| `tasks/update` answers `task is not waiting for input` | The task already left `input_required`; or `result` and `inputResponses` were both sent | Submit only while at `input_required`, and keep the two fields exclusive |
| After a resume the handler asks for the same input again | It re-asked without first reading `ctx.Params().input_responses` | Reorder the check to "look at what you already have, then ask only for what is absent" |

## Next steps

- Report progress while a task runs: [/en/howto/report-progress](/en/howto/report-progress)
- Ask the user back from inside a task (MRTR/elicitation): [/en/howto/request-extra-input](/en/howto/request-extra-input), [/en/howto/collect-user-input](/en/howto/collect-user-input)
- Tool registration and argument validation: [/en/howto/register-a-tool](/en/howto/register-a-tool), [/en/howto/validate-tool-input](/en/howto/validate-tool-input)
- Full two-era semantics: [/en/learn/protocol-versions](/en/learn/protocol-versions)
- Signatures and field tables: [/en/reference/server](/en/reference/server), [/en/reference/client](/en/reference/client), [/en/reference/types](/en/reference/types)

## Source behind this page

- `include/mcp/McpTypes.hpp` (`ToolExecutionMode`/`TaskSupport`/`ToolExecution`, `ToolOptions::execution`, `CreateTaskResult`/`GetTaskResult`)
- `include/mcp/server/ServerOptions.hpp` (`enable_tasks_extension`, `task_store`)
- `include/mcp/server/McpTaskStore.hpp` (`TaskStatus`, `TaskState`, `IMcpTaskStore`)
- `include/mcp/storage/FileTaskStore.hpp`
- `include/mcp/server/McpServer.hpp` (`RegisterTool`, `SendTaskStatus`)
- `include/mcp/server/RequestContext.hpp` (`IsCancellationRequested`)
- `include/mcp/client/McpClient.hpp` (`CallToolAsTask`, `GetTask`, `UpdateTask`, `CancelTask`, `PollTaskToCompletion`)
- `include/mcp/transport/InMemoryTransport.hpp`
- `src/server/McpServer.cpp` (the three task paths, owner isolation, subscription routing, terminal and cancel precedence)
- `src/client/McpClient.cpp` (task gating, polling, degraded handle)
- `src/protocol/McpSessionHandler.cpp` (`NotifySubscribers` and `subscriptionId`)
