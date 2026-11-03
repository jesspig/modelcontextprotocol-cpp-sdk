# 承接服务端发起的请求

服务端不只会回你的调用，它也会**主动问你**：要补充信息（elicitation）、要你给一段生成结果（sampling，已废弃）、要你的工作目录清单（roots，已废弃）。这一页讲客户端侧要挂哪些回调、SDK 替你自动做了什么、以及什么时候必须自己动手。签名与字段表见 [/zh/reference/client](/zh/reference/client)，反问的服务端侧写法见 [/zh/howto/collect-user-input](/zh/howto/collect-user-input)。

::: info 两个时代走两条完全不同的路
legacy（≤ 2025-11-25）时代反问是**独立的 JSON-RPC 请求**，`elicitation/create` 直接打进你的 handler；modern（2026-07-28）时代它变成同一次工具调用的 `input_required` 结果 + `inputResponses` 续轮，SDK 默认把这个来回**自动跑完**，你的 handler 仍然只是被调一次。
:::

## 前置条件

- 头文件：`mcp/client/ClientOptions.hpp`、`mcp/client/McpClient.hpp`、`mcp/McpTypes.hpp`、`mcp/Capabilities.hpp`、`mcp/Content.hpp`、`mcp/JsonValue.hpp`、`mcp/client/RequestHandle.hpp`、`mcp/protocol/IncomingRequestMeta.hpp`、`mcp/transport/InMemoryTransport.hpp`；include 必须写全子目录路径。
- 四个 handler 槽位都在 `ClientOptions` 上（`include/mcp/client/ClientOptions.hpp:42-45`）：`elicitation_handler`、`url_elicitation_handler`、`sampling_handler`、`roots_handler`；建好客户端后还能用 `SetElicitationHandler` / `SetUrlElicitationHandler` 补挂（`include/mcp/client/McpClient.hpp:117-121`）。
- handler 必须是**快速返回**的纯函数式逻辑：它跑在协议分发路径上，阻塞就是把整条连接卡住。要真人回答，先把问题转给自己的 UI 线程，再把结果交给 handler。
- 无需 CMake 开关。

## 表单式反问：elicitation

handler 收到 `ElicitRequestParams`（`message`、`requested_schema`、`mode="form"`），回 `ElicitResult{action, content}`（`include/mcp/McpTypes.hpp:350-361`）。三态就是 `action` 的三个字符串：`accept`（带 `content`）、`decline`、`cancel`。

<<< @/snippets/handle-server-initiated-requests.cpp#form-handler

::: warning 没挂 handler 的回法不是「沉默」
form 模式下没有 handler 时，客户端直接以 `MethodNotFound` + `elicitation not supported` 回错（`src/client/McpClient.cpp:735-738`）——服务端会把这次反问当成失败，而不是「用户还没答」。
:::

结果回投前，SDK 还有一次**可选**的默认值补全：只有当你显式声明 `capabilities->elicitation->form` 里带 `"applyDefaults": true` 时，才按 `requested_schema` 的默认值填充你留空的字段（`src/client/McpClient.cpp:239-265`）。不声明就完全不代填。

## URL 模式：让用户去外部完成

`mode == "url"` 时走 `url_elicitation_handler`，参数里带 `url`。这个模式只需要你表个态：`accept` 表示用户已开始外部流程，真正的结果由服务端后续自己收。

<<< @/snippets/handle-server-initiated-requests.cpp#url-handler

三处实现细节值得知道：

- 没挂 URL handler 时**自动回 `decline`**，不会报错（`src/client/McpClient.cpp:296-305`）。
- legacy 时代的 URL 反问必须带 `elicitationId`，缺了客户端直接回 `InvalidParams`（`:710-715`）。
- legacy 时代答完之后，客户端**自己补发** `notifications/elicitation/complete`（`{"elicitationId": ...}`）来唤醒等待中的服务端（`:722-733`）；modern 时代没有这一步，靠续轮结算。

## 能力表由 handler 反推

`DeriveClientCapabilities`（`src/client/McpClient.cpp:360-392`）按 handler 的存在与否拼能力表：form handler → `elicitation.form`，URL handler → `elicitation.url`，sampling → `sampling`，roots → `roots` 且顺带 `list_changed = true`。四个都没设且没显式给 `capabilities` 时返回空——即**不声明任何能力**。你显式写的 `capabilities` 不会被覆盖，SDK 只补空缺项。

<<< @/snippets/handle-server-initiated-requests.cpp#capability-derivation

这意味着「挂上 handler」≈「声明能力」，但想**只声明不实现**或**只实现不声明**都不成立：别指望服务端读你的代码，它只看 `initialize` 里的能力表。

## 已废弃的 sampling 与 roots

`SetSamplingHandler` 与 `SetRootsHandler` 带 `[[deprecated("... SEP-2577")]]`（`include/mcp/client/McpClient.hpp:113-116`），2026-07-28 起这两类反向请求不再由服务端发起；`Ping()` 同样标记废弃（`:108-109`）。迁移写法是继续用 `ClientOptions` 的**成员字段**接住 legacy 对端的请求，并把同类需求改成 MRTR 反问：

<<< @/snippets/handle-server-initiated-requests.cpp#sampling-roots

`CreateMessageResult` 与 `ListRootsResult` 都还是正常可用的结果类型（`include/mcp/McpTypes.hpp:378-392`），只是新代码不该再依赖服务端主动来问；`MakeInputRequestForSampling` / `MakeInputRequestForRoots`（`:371-373`）是给**你**在续轮里向对方要同类输入的构造器。

## 自动续轮：客户端替你回一轮

modern 时代工具结果带 `resultType == "input_required"` 时，`SendRequestWithMrtr` 会就地补齐并重发，直到拿到完整结果或触发上限（`src/client/McpClient.cpp:1069-1135`）。默认配置是 `auto_fulfill = true`、`max_rounds = 10`、`round_timeout = 600s`（`include/mcp/client/ClientOptions.hpp:55-63`）。

规则要点：

- **所需 handler 缺失时不报错**，而是把 `input_required` 原样交回给你（`src/client/McpClient.cpp:997-1009`）——这是「自动模式静默降级」的唯一形态，务必检查结果再当成功用。
- 每轮超时取 `round_timeout` 与调用超时中的较小值；`max_total_timeout` 是整条流程的总预算，超了抛 `RequestTimeout` + `MRTR: exceeded max_total_timeout`（`:1077-1097`）。
- 轮数用尽抛 `InternalError` + `MRTR: exceeded max_rounds (N)`（`:1133-1134`）。
- 只想刷新 `requestState` 的空轮次会**指数退避**后重试，不填 `inputResponses`（`:1116-1121`）。
- 遇到 `SessionExpired` 且 `reinit_on_expired_session = true` 时自动重新 `initialize` 并重来一次（`:1057-1066`）。

要自己接管这一来回，关掉自动补齐：

<<< @/snippets/handle-server-initiated-requests.cpp#disable-auto-fulfil

## 手工续轮：拿到 input_required 自己回

`CallToolResult` 上有 `result_type` 与 `input_required`（`include/mcp/McpTypes.hpp:109-117`、`:268-273`）。注意 `CallTool()` **不会**把 `RequestOptions::input_responses` / `request_state` 写进出站参数（`src/client/McpClient.cpp:1312-1356`），所以续轮必须走 `SendRequest`，并把两个键直接放进 `params`：`inputResponses` 与 `requestState`（`src/detail/JsonFields.hpp:21`、`:33`）。

<<< @/snippets/handle-server-initiated-requests.cpp#manual-round

每个应答值用官方构造器生成——`MakeInputResponseFromElicitResult` / `MakeInputResponseFromCreateMessageResult` / `MakeInputResponseFromListRootsResult`（`include/mcp/McpTypes.hpp:374`、`:394-395`），别手搓结构；`input_requests` 的**键必须原样回用**（`src/client/McpClient.cpp:1019-1047`）。遇到 SDK 不认识的 `method` 会抛 `HandlerError` + `unsupported input request method: ...`（`:1041-1043`）。

## 其它被动侧入口

<<< @/snippets/handle-server-initiated-requests.cpp#passive-notifications

- `SetNotificationHandler(method, handler)` 接任意通知，回调签名是 `std::function<void(const JsonRpcNotification&)>`（`include/mcp/client/McpClient.hpp:29-30`、`:122-124`）。
- `SetLoggingHandler` 只收协议日志帧（`:126`）。
- 三类 `*_list_changed` 与 `resources_updated` 由 SDK **内置处理**：它们会清空本地响应缓存（`src/client/McpClient.cpp:748-760`），你不需要为此挂 handler。
- `SubscribeAsync` 只登记订阅（modern 时代 `subscriptions/listen`），见 [/zh/howto/serve-a-resource](/zh/howto/serve-a-resource)。

## 完整代码

<<< @/snippets/handle-server-initiated-requests.cpp#full

## 验证

片段只做语法校验（clang-cl 走 `/Zs`，GCC/Clang 走 `-fsyntax-only`），不需要起进程：

```bash
cmake -S . -B build/p3a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3a --target mcp-doc-snippet-handle-server-initiated-requests
```

期望输出：configure 阶段打印 `[mcp-docs] syntax-checking 14 snippet(s) with '/Zs'`，构建阶段打印

```text
[1/4] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-handle-server-initiated-requests.dir\__\__\docs\snippets\handle-server-initiated-requests.cpp.obj
```

返回码 0 且无 `error:` 行。若 CMake 报「object file 目录 194 字符接近 250 上限」的路径长度告警，说明工作树路径过深，换一个更浅的 `build` 目录即可，与本页内容无关。

## 时代差异

| 项目 | legacy（≤ 2025-11-25） | modern（2026-07-28） |
|---|---|---|
| 反问到达形态 | 独立请求 `elicitation/create` 打进 handler | 工具结果 `resultType == "input_required"` + `inputRequests` |
| 回答如何回传 | 该请求的响应体 | 下一轮 `params.inputResponses`（可带 `requestState`） |
| 谁驱动来回 | 服务端逐次发请求 | 客户端 `SendRequestWithMrtr` 自动续轮（可关） |
| 缺 handler | form 回 `MethodNotFound`；URL 自动 `decline` | 把 `input_required` 原样交回调用方，不报错 |
| URL 模式收尾 | 客户端补发 `notifications/elicitation/complete` | 由续轮结算，无该通知 |
| sampling / roots | 可用但已标记废弃 | 不再由服务端发起（SEP-2577） |

一行结论：**能力表决定服务端敢不敢问，时代决定问题长什么样**。细节矩阵见 [/zh/learn/protocol-versions](/zh/learn/protocol-versions)。

## 常见错误

| 对外表现 | 成因 | 处置 |
|---|---|---|
| 服务端报「客户端不支持 elicitation」 | 没挂 handler，或挂了但晚于 `initialize` | 在建客户端前用 `ClientOptions` 设好；补挂 setter 只对之后的请求生效 |
| 工具调用「成功」但结果空 | 自动续轮因缺 handler 降级，`input_required` 被你当完成 | 检查 `result_type == ResultType::InputRequired` 与 `input_required` 是否有值 |
| 抛 `MRTR: exceeded max_rounds (10)` | 服务端每轮都要新输入 | 提高 `max_rounds`，或改成任务见 [/zh/howto/run-long-tasks](/zh/howto/run-long-tasks) |
| 续轮被服务端判为无效 | `requestState` 没原样带回，或 `inputResponses` 的键被改名 | 键与 `requestState` 一律照抄回用 |
| 传了 `RequestOptions::input_responses` 给 `CallTool` 却没生效 | `CallTool` 不透传这两个字段 | 续轮走 `SendRequest` |
| URL 反问在 legacy 下报 `InvalidParams` | 请求缺 `elicitationId` | 这是对端的协议缺陷，客户端按规范拒绝 |
| handler 里弹对话框把连接卡死 | handler 跑在分发路径上 | 把交互挪到自己的线程，handler 只做「取答案或表态」 |
| schema 默认值没被补上 | 未声明 `applyDefaults` | 显式在 `capabilities->elicitation->form` 里写 `{"applyDefaults": true}` |

## 下一步

- 服务端侧怎么发起反问：[/zh/howto/collect-user-input](/zh/howto/collect-user-input)、[/zh/howto/request-extra-input](/zh/howto/request-extra-input)
- 用取消与超时收尾：[/zh/howto/cancel-a-request](/zh/howto/cancel-a-request)
- 客户端连接与关闭时机：[/zh/howto/connect-to-a-server](/zh/howto/connect-to-a-server)
- 能力表与协商：[/zh/learn/protocol-versions](/zh/learn/protocol-versions)、[/zh/reference/client](/zh/reference/client)

## 本页对应源码

- `include/mcp/client/ClientOptions.hpp`（四个 handler 槽位、`InputRequiredConfig`）
- `include/mcp/client/McpClient.hpp`（`SetElicitationHandler`、`SetUrlElicitationHandler`、`SetNotificationHandler`、废弃 setter）
- `include/mcp/McpTypes.hpp`（`ElicitRequestParams`、`ElicitResult`、`InputRequest`、`InputRequiredResult`、`Make*InputResponse*`、`ResultType`）
- `include/mcp/Capabilities.hpp`（`ElicitationCapability` 的 `form`/`url`）
- `include/mcp/Content.hpp`（`ContentVariant`、`TextContent`）
- `src/client/McpClient.cpp`（入站分发、能力推导、URL 收尾通知、MRTR 续轮与退避）
- `src/detail/JsonFields.hpp`（`inputResponses`、`requestState` 线上键名）
