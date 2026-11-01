# 把耗时工具改造成任务

给一个跑得很慢的工具加上 MCP 任务（tasks）语义：`tools/call` 立刻回一个 `taskId`，真正的执行体在后台跑，客户端用 `tasks/get` 轮询、用 `tasks/cancel` 协作取消、用 `tasks/update` 在 `input_required` 后续跑。签名与字段表只在 [/zh/reference/server](/zh/reference/server)、[/zh/reference/client](/zh/reference/client) 与 [/zh/reference/types](/zh/reference/types) 维护，本页只讲怎么把它们串起来。

## 前置条件

- 头文件：`mcp/server/McpServer.hpp`、`mcp/server/ServerOptions.hpp`、`mcp/server/McpTaskStore.hpp`、`mcp/server/RequestContext.hpp`、`mcp/storage/FileTaskStore.hpp`、`mcp/client/McpClient.hpp`、`mcp/client/ClientOptions.hpp`、`mcp/McpTypes.hpp`、`mcp/JsonValue.hpp`、`mcp/Content.hpp`、`mcp/Implementation.hpp`、`mcp/transport/InMemoryTransport.hpp`；include 必须写全子目录路径。
- 开关：服务端 `ServerOptions::enable_tasks_extension = true` **且** `ServerOptions::task_store` 非空，两者缺一不可；客户端 `ClientOptions::enable_tasks_extension = true`。
- 协议时代：任务化在 5 个版本上都能谈，但 2026-07-28（modern）额外要求客户端声明 `io.modelcontextprotocol/tasks` 扩展，见「时代差异」。
- 与 elicitation 的分工：本页只讲「任务停在 `input_required` 后靠 `tasks/update` 续跑」；handler 内部就地阻塞反问用户属于 [/zh/howto/request-extra-input](/zh/howto/request-extra-input) 与 [/zh/howto/collect-user-input](/zh/howto/collect-user-input)。

## 声明任务化执行

任务化的唯一入口是工具自己的 `ToolOptions::execution`：把 `mode` 设成 `ToolExecutionMode::Task`。`task_support` 决定客户端不配合时的行为——`Optional`（默认值）表示可以退化成同步执行，`Required` 表示必须成任务，否则回能力错误。

`execution` 没有链式 setter，只能整体赋值；其余字段照常用 `Description()`/`InputSchema()`。

<<< @/snippets/run-long-tasks.cpp#task-declaration

判据在 `src/server/McpServer.cpp:2331-2333`：`tool_def.execution && mode == Task` 才进任务分支，`Auto`/`Manual` 只影响 `tools/list` 里的 `execution` 声明，不改变服务端执行方式。

## 打开扩展开关并注入 store

`tasks/get`、`tasks/update`、`tasks/cancel`、`tasks/result`、`tasks/list` 的入口处理器**只在 `options_.task_store` 非空时才注册**，而每个处理器第一件事是检查 `enable_tasks_extension`。也就是说：只塞 `task_store` 而不打开关，任务方法照样回 `MethodNotFound: tasks extension is disabled`（`McpServer.cpp:1511` 的 `if (store)`、`1514-1517`、`1556-1559`、`1796-1799` 三处门控）；只打开关不给 store，`tools/call` 走不进任务分支，工具仍以同步方式执行。

内置实现是 `FileTaskStore`（JSON 落盘，追加写 + 压实）；`IMcpTaskStore` 也允许自研，需要落地的只有 `CreateTask/GetTask/UpdateTask/CancelTask/SetTaskStatus/GetAllTasks` 六个纯虚函数，其余带默认实现（`include/mcp/server/McpTaskStore.hpp:45-97`）。

<<< @/snippets/run-long-tasks.cpp#task-enable

任务 ID 形如 `task-<session_id>-<16 字节随机 hex>`（`McpServer.cpp:1016`），owner 锚定发起 `tools/call` 的那个会话；`ttl_ms` 固定取默认值 300000（`McpServer.cpp:352`），每次新建任务前顺带清扫过期任务（`McpServer.cpp:2415-2416`、`SweepExpiredTasks` 1070-1089）。

## 在 handler 内做协作取消

handler 拿到的还是同一个 `RequestContext`，只需在循环边界查一次 `IsCancellationRequested()`。这个标志是 `std::atomic<bool>` 的共享指针：普通（非任务）工具挂的是协议级 `notifications/cancelled` 对应的请求标志（`McpServer.cpp:2313-2317`），任务工具挂的是 `tasks/cancel` 写入的 `running_task_cancel_flags_[taskId]`（`McpServer.cpp:2418-2421`），两条路径都走同一个判定。

<<< @/snippets/run-long-tasks.cpp#task-cancellation

取消是协作式的：handler 不查标志就一直跑到底。已进终态（`completed`/`failed`/`cancelled`）的任务不会因 `tasks/cancel` 再迁移状态，服务端只回一个空 ack（`McpServer.cpp:1841-1851`）；尚未落地的取消在任务收尾时优先判成 `cancelled`，再判 `failed`（`McpServer.cpp:2429-2434`）。

## 客户端轮询到完成

`CallToolAsTask` 把 `tools/call` 的任务回执读成 `GetTaskResult` 句柄（只有 `task_id` 与 `status`，没有结果体），随后交给 `PollTaskToCompletion` 按服务端建议的 `pollIntervalMs` 轮 `tasks/get`。

<<< @/snippets/run-long-tasks.cpp#task-poll

三处要注意：

- 判定终态用 wire 字符串 `completed`/`failed`/`cancelled`（`McpClient.cpp:1789-1794`），`working` 与 `input_required` 都不是终态。
- `PollTaskToCompletion` 只等终态，`input_required` 会一路轮到超时抛 `InternalError`（`McpClient.cpp:1811-1813`），所以可能停在 `input_required` 的任务要自己 `GetTask` 分支处理。
- 若服务端其实同步跑完了（例如 modern 下客户端没声明 tasks 扩展、`Optional` 工具退化为同步），`CallToolAsTask` 回一个 `task_id == "<none>"`、`status == "completed"` 的降级句柄，结果体在 `result` 里（`McpClient.cpp:1730-1735`）——别拿它去 `tasks/get`。

## 主动取消一个任务

<<< @/snippets/run-long-tasks.cpp#task-cancel

`CancelTask` 置位共享标志后立刻返回 ack，不等执行体真的停下来；要确认落点就再 `GetTask` 或 `PollTaskToCompletion` 一次。`tasks/cancel` 只认 owner 会话，跨会话取别人的任务与取一个不存在的任务返回完全相同的错误：`InvalidParams: task not found: <taskId>`（`McpServer.cpp:1545-1550`、`1826-1831`）。

## 用 input_required 让任务续跑

handler 返回 `CallToolResult::input_required` 时，服务端把任务停在 `input_required` 并记下 `input_requests`；客户端补齐后调 `tasks/update`（只带 `inputResponses`），`AdvanceTaskWithInput` 按键增量合并进已存响应，任务回到 `working`。

<<< @/snippets/run-long-tasks.cpp#task-input-required

`input_responses` 的形状是以 `input_requests` 的键为下标的对象，每项是 `ElicitResult` 形态：`{"action":"accept","content":{...}}`。handler 每次被唤醒都要自己重新判断「还缺哪几项」——上一轮已收到的响应会原样带回来，所以判定顺序必须是「先查已有、缺什么再要什么」，否则就死循环停在同一处。

三条硬约束（`McpServer.cpp:1586-1605`）：`result` 与 `inputResponses` 互斥，同时给回 `InvalidParams`；任务不在 `input_required` 时回 `InvalidParams: task is not waiting for input`；modern 时代续跑由服务端**就地重跑工具**并把最终结果放进本次 `tasks/update` 的响应，legacy 时代只回 ack，最终结果仍靠 `tasks/get`/`tasks/result` 取。

## 完整代码

服务端与客户端各自打开扩展、注册任务工具、发起任务并轮询到终态，最后协作取消一个在跑的任务：

<<< @/snippets/run-long-tasks.cpp#full

## 验证

片段只做语法校验（clang-cl 走 `/Zs`，GCC/Clang 走 `-fsyntax-only`），不需要起进程：

```bash
cmake -S . -B build/p2d -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p2d --target mcp-doc-snippet-run-long-tasks
```

期望输出（本机实测）：configure 阶段打印 `[mcp-docs] syntax-checking <N> snippet(s) with '/Zs'`，构建阶段打印

```
[1/4] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-run-long-tasks.dir\__\__\docs\snippets\run-long-tasks.cpp.obj
```

返回码 0 且无 `error:` 行（`clang-cl: warning: argument unused during compilation: '-c'` 是该模式的固有提示，可忽略）。

行为侧由这些既有用例守护，需先 `git submodule update --init third_party/googletest` 再 `ctest --preset debug -R '<用例名>'`（用例名与归属见 `tests/unit/McpServerTests.cpp`、`tests/unit/McpClientTests.cpp`）：`McpServerTest.TasksRequireExplicitExtensionFlag`、`McpServerTest.TasksMethodsGateWithoutExtension`、`McpServerTest.CrossSessionTaskAccessIsRejected`、`McpServerTest.ExpiredTasksAreReclaimedWhenNewTaskIsCreated`、`McpServerTest.CallToolTaskModeCancelsRunningTaskCooperatively`、`McpServerTest.TerminalTaskCannotBeRegressedByUpdateOrCancel`、`McpServerTest.TaskIdentifiersAreSessionScopedAndNotSequential`、`McpServerTest.TasksUpdateResumeCompletesParkedTask`、`McpClientTest.PollTaskViaTasksGet`、`McpClientTest.CallToolAsTaskDegradesSynchronousResult`。

## 时代差异

| 项目 | legacy（≤ 2025-06-18） | modern（2026-07-28） |
|---|---|---|
| `tasks/get`/`tasks/update`/`tasks/cancel` | 可用 | 可用，但客户端必须声明 `io.modelcontextprotocol/tasks`，否则回 `MissingRequiredClientCapability`（`-32021`） |
| `tasks/result`、`tasks/list` | 可用 | **无此方法**，回 `MethodNotFound`（`McpServer.cpp:1876-1881`、`1894-1901`） |
| `tools/call` 任务回执形状 | 顶层 `taskId/status/createdAt` 之外再保留嵌套 `task` 对象 | 只留顶层扁平字段，`task` 嵌套被删除（`McpServer.cpp:2450-2451`、`2475-2476`） |
| 任务状态通知路由 | `SendNotification` 广播 | `NotifySubscribers` 按订阅投递，`_meta.subscriptionId` 必填（`McpServer.cpp:297-302`、`McpSessionHandler.cpp:1029-1035`） |
| `Optional` 任务工具 + 客户端未声明扩展 | 照常成任务 | 退化为同步执行（`McpServer.cpp:2342-2349`） |
| `Required` 任务工具 + 客户端未声明扩展 | 照常成任务 | 回能力错误，且先做入参校验 |

wire 状态字符串两个时代一致：`working`、`input_required`、`completed`、`failed`、`cancelled`；C++ 侧的 `TaskStatus::Pending` 不出现在线上，一律映射成 `working`（`McpServer.cpp:226-241`）。

## 常见错误

| 对外表现 | 成因 | 处置 |
|---|---|---|
| `tasks/*` 全部回 `MethodNotFound: tasks extension is disabled` | 只给了 `task_store` 没打 `enable_tasks_extension` | 两个都设；store 与开关是并列必要条件 |
| `tools/call` 直接同步跑完，没有 `taskId` | 开关打了但 `task_store` 为空，或工具没设 `execution.mode = Task` | 补齐 store，并确认 `execution` 是整体赋值而非只改了局部变量 |
| 客户端调 `PollTaskToCompletion`/`UpdateTask` 立刻抛 `MethodNotFound` | `ClientOptions::enable_tasks_extension` 未设（客户端在本地就拦，见 `McpClient.cpp:1751`、`1782`） | 客户端也打开开关；注意 `GetTask`/`CancelTask` 不做这项检查，别把它们当判据 |
| modern 下调用回 `-32021`，`data.requiredCapabilities.extensions` 带 `io.modelcontextprotocol/tasks` | 客户端没声明 tasks 扩展而工具是 `Required` | 在 `ClientOptions::capabilities`/`extensions` 里声明该扩展 |
| `GetTask` 回 `InvalidParams: task not found: <id>` | 任务不是本会话创建的，或已过 `ttl_ms` 被清扫 | 用同一会话轮询；需要更长存活就自己实现 `SetTaskTtl` |
| 取消后状态仍是 `completed` | 取消到达时执行体已越过最后一个检查点 | 属预期：终态不迁移；把取消检查点放在真正耗时的边界前 |
| 任务停在 `input_required` 直到轮询超时抛 `InternalError` | 用 `PollTaskToCompletion` 等终态，而 `input_required` 不是终态 | 自己 `GetTask` 分流，补 `tasks/update` 后再继续轮询 |
| `tasks/update` 回 `task is not waiting for input` | 任务已不在 `input_required`；或同时带了 `result` 与 `inputResponses` | 只在 `input_required` 时提交，两者互斥 |
| 续跑后 handler 又要求同一项输入 | 没先读回 `ctx.Params().input_responses` 就重新发问 | 判定顺序改为「先查已有、缺什么再要什么」 |

## 下一步

- 在任务执行途中上报进度：[/zh/howto/report-progress](/zh/howto/report-progress)
- 任务里反问用户（MRTR/elicitation）：[/zh/howto/request-extra-input](/zh/howto/request-extra-input)、[/zh/howto/collect-user-input](/zh/howto/collect-user-input)
- 工具注册与入参校验：[/zh/howto/register-a-tool](/zh/howto/register-a-tool)、[/zh/howto/validate-tool-input](/zh/howto/validate-tool-input)
- 双时代语义全表：[/zh/learn/protocol-versions](/zh/learn/protocol-versions)
- 签名与字段表：[/zh/reference/server](/zh/reference/server)、[/zh/reference/client](/zh/reference/client)、[/zh/reference/types](/zh/reference/types)

## 本页对应源码

- `include/mcp/McpTypes.hpp`（`ToolExecutionMode`/`TaskSupport`/`ToolExecution`、`ToolOptions::execution`、`CreateTaskResult`/`GetTaskResult`）
- `include/mcp/server/ServerOptions.hpp`（`enable_tasks_extension`、`task_store`）
- `include/mcp/server/McpTaskStore.hpp`（`TaskStatus`、`TaskState`、`IMcpTaskStore`）
- `include/mcp/storage/FileTaskStore.hpp`
- `include/mcp/server/McpServer.hpp`（`RegisterTool`、`SendTaskStatus`）
- `include/mcp/server/RequestContext.hpp`（`IsCancellationRequested`）
- `include/mcp/client/McpClient.hpp`（`CallToolAsTask`、`GetTask`、`UpdateTask`、`CancelTask`、`PollTaskToCompletion`）
- `include/mcp/transport/InMemoryTransport.hpp`
- `src/server/McpServer.cpp`（任务三路径、owner 隔离、订阅路由、终态与取消优先级）
- `src/client/McpClient.cpp`（任务开关、轮询与降级句柄）
- `src/protocol/McpSessionHandler.cpp`（`NotifySubscribers` 与 `subscriptionId`）
