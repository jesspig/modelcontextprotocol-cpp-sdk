# 错误码与错误模型

本页只给对照表与一句话结论：`McpErrorCode` 全量枚举、每个码在 SDK 内的实际抛出点、协议时代的出站重映射、错误在三种 API 形态上的不同呈现，以及用户 handler 异常的收敛行为。签名与字段细节见 [客户端参考](/zh/reference/client)、[类型层参考](/zh/reference/types)、[传输层参考](/zh/reference/transports)，时代语义见 [协议版本](/zh/learn/protocol-versions)。

## `McpError` 类型

| 符号 | 签名 | 结论 | 证据 |
|---|---|---|---|
| `McpError` | `class McpError : public std::runtime_error` | SDK 唯一的错误抛出类型，`what()` 即消息文本 | `include/mcp/McpError.hpp:13` |
| 构造（两参） | `McpError(McpErrorCode code, std::string message)` | `data` 恒为空 | `McpError.hpp:15-18` |
| 构造（三参） | `McpError(McpErrorCode code, std::string message, std::optional<JsonValue> data)` | 携带规范载荷（`requiredCapabilities`、`supported` 等） | `McpError.hpp:20-24` |
| `Code` | `McpErrorCode Code() const noexcept` | 返回枚举原值，不做归一化 | `McpError.hpp:26` |
| `ErrorMessage` | `std::string_view ErrorMessage() const noexcept` | 与 `what()` 同源，二者恒等 | `McpError.hpp:27` |
| `Data` | `const std::optional<JsonValue>& Data() const noexcept` | 可选，多数抛出点不填 | `McpError.hpp:28` |

## `McpErrorCode` 全表

底层类型 `int32_t`，共 20 项，按声明顺序列出（`include/mcp/ErrorCodes.hpp:8-30`）。

| 枚举项 | 数值 | 语义 | 典型触发点 | 抛出点证据 |
|---|---|---|---|---|
| `ParseError` | -32700 | JSON 文本本身解析不出来 | 对端报文非法 JSON，消息带失败偏移 | `src/core/JsonParser.cpp:35`；HTTP 侧映射 400：`src/http/StreamableHttpServerTransport.cpp:118` |
| `InvalidRequest` | -32600 | 信封不符合 JSON-RPC 2.0 或当前时代要求 | `jsonrpc` 版本不符、请求 `id` 缺失或为 null、未初始化即调用、现代时代缺 `_meta` | `src/core/JsonRpc.cpp:17`、`src/core/JsonRpc.cpp:104`、`src/protocol/McpSessionHandler.cpp:419`、`src/server/McpServer.cpp:590` |
| `MethodNotFound` | -32601 | 方法不存在或不在协商时代 | era 校验未命中、扩展未启用（skills/tasks/apps）、客户端未装 elicitation/sampling/roots handler | `src/protocol/McpSessionHandler.cpp:415`、`src/server/McpServer.cpp:2956`、`src/client/McpClient.cpp:739` |
| `InvalidParams` | -32602 | `params` 或 `_meta` 不合契约，以及各类"未命中" | 字段类型/范围校验、`_meta` 字段类型非法、tool/resource/prompt/task/skill 未命中、`outputSchema` 不符、tasks 请求缺 `taskId` | `src/core/Content.cpp:213`、`src/protocol/McpSessionHandler.cpp:425`、`src/protocol/WireCodec.cpp:308`、`src/server/McpServer.cpp:2148` |
| `InternalError` | -32603 | SDK 内部故障，也是"非 `McpError` 异常"的统一收敛码 | 安全随机数失败、原子 JSON 存储写失败、DPoP/OAuth 内部错误、handler 泛化 | `src/server/McpServer.cpp:438`、`src/detail/AtomicJsonFile.cpp:106`、`src/protocol/McpSessionHandler.cpp:505` |
| `HeaderMismatch` | -32020 | `Mcp-Method`/`Mcp-Name` 头与报文不一致 | 状态化 HTTP 缺必需头、`Mcp-Name` 与 body 的 `taskId` 不符 | `src/protocol/WireCodec.cpp:311`、`src/http/StreamableHttpServerTransport.cpp:317`、`src/http/StreamableHttpServerTransport.cpp:1197` |
| `MissingRequiredClientCapability` | -32021 | 客户端缺必需能力，`data.requiredCapabilities` 指明缺哪个 | `VerifyCapability` 校验、服务端要求 sampling/elicitation/tasks | `src/protocol/McpSessionHandler.cpp:533`、`src/server/McpServer.cpp:386`、`src/server/McpServer.cpp:1165` |
| `UnsupportedProtocolVersion` | -32022 | 协商版本不被支持，`data.supported`/`data.requested` | `_meta.protocolVersion` 未支持、HTTP 版本头不符、`server/discover` 探测被拒 | `src/protocol/McpSessionHandler.cpp:432`、`src/http/StreamableHttpServerTransport.cpp:362`、`src/client/McpClient.cpp:542` |
| `UrlElicitationRequired` | -32042 | 协议定义：必须走 URL 型 elicitation | 见下方专节 | **SDK 内无抛出点** |
| `ResourceNotFound` | -32002 | 协议保留：资源不可读 | 见下方专节 | **SDK 内无抛出点** |
| `ConnectionClosed` | -32000 | 连接或句柄已关闭 | socket/HTTP 读到底、`HttpClient` 已关闭、关闭时 pending 请求被中止 | `src/transport/detail/net/HttpClient.cpp:60`、`src/transport/detail/net/TcpSocketPosix.cpp:195`、`src/protocol/McpSessionHandler.cpp:275` |
| `RequestTimeout` | -32001 | 请求超时 | 会话层 deadline 到期（默认 60s）、HTTP 读写超时、URL elicitation 看门狗 | `src/protocol/McpSessionHandler.cpp:337`、`src/transport/detail/net/HttpClient.cpp:245`、`src/server/McpServer.cpp:1254` |
| `RequestCancelled` | -32800 | 请求被取消，`data.reason` 可选 | 对端 `notifications/cancelled`、本地 `CancelRequest` 或 `RequestHandle::Cancel` | `src/protocol/McpSessionHandler.cpp:1100`、`src/protocol/McpSessionHandler.cpp:1128` |
| `ConnectionRefused` | -32003 | 连接建立失败 | TCP connect 被拒、Winsock 初始化失败 | `src/transport/detail/net/TcpSocketPosix.cpp:128`、`src/transport/detail/net/TcpSocketWin32.cpp:87`、`src/transport/detail/net/TcpSocketWin32.cpp:130` |
| `TlsHandshakeFailed` | -32004 | TLS 不可用或握手失败 | OpenSSL 初始化/`SSL_new`/握手失败；未编译 TLS 支持时同码 | `src/transport/detail/net/TlsSocket.cpp:93`、`src/transport/detail/net/TlsSocket.cpp:137`、`src/transport/detail/net/TlsSocket.cpp:279` |
| `ProtocolViolation` | -32005 | 协议用法越界（多为对端行为） | 分页未在 64 页内收敛、现代时代调 `logging/setLevel` 或发 `notifications/roots/list_changed`、URL scheme 非法 | `src/client/McpClient.cpp:169`、`src/client/McpClient.cpp:1915`、`src/transport/detail/net/HttpClient.cpp:69` |
| `TaskNotFound` | -32006 | 协议保留：任务不存在 | 见下方专节 | **SDK 内无抛出点** |
| `HandlerError` | -32007 | 本地回调或扩展前置条件不满足 | 未声明 tasks 扩展却收到 task 结果、MRTR 出现未知 input request 方法、URL elicitation 返回非法 action、apps/server card 未启用 | `src/client/McpClient.cpp:290`、`src/client/McpClient.cpp:1042`、`src/server/McpServer.cpp:2768` |
| `DeserializeFailed` | -32008 | 类型化反序列化失败 | `JsonValue` 取到错误类型、内容块/枚举无法还原、错误码数值超出 `int32_t` | `src/core/JsonValue.cpp:184`、`src/core/Content.cpp:123`、`src/core/JsonRpc.cpp:68` |
| `SessionExpired` | -32009 | 状态化会话已失效 | 服务端回收会话后仍有挂起请求、客户端 POST 得到 404 | `src/http/StreamableHttpServerTransport.cpp:949`、`src/http/StreamableHttpClientTransport.cpp:85` |

### 三个没有抛出点的码

::: warning
`UrlElicitationRequired`（-32042）、`ResourceNotFound`（-32002）、`TaskNotFound`（-32006）在 SDK 生产代码里**没有任何抛出点**，仅出现在两处纯登记位置：类目文案分支（`include/mcp/ErrorCodes.hpp:56-65`）与服务端已知码白名单 `IsKnownMcpErrorCode`（`src/server/McpServer.cpp:456-483`）。
:::

"未命中"类语义在 SDK 内一律走 `InvalidParams`：task `src/server/McpServer.cpp:1547`、tool `:2148`、resource `:2816`、prompt `:2912`、skill `:3030`。URL 型 elicitation 也不产生 -32042，服务端直接发 `mode:"url"` 的 elicitation 请求（`src/server/McpServer.cpp:1173`），客户端未声明 elicitation 能力时改抛 `MissingRequiredClientCapability`（`:1165`）。

## 与 `std::error_code` 的桥接

| 机制 | 结论 | 证据 |
|---|---|---|
| `is_error_code_enum` 特化 | `McpErrorCode` 可隐式转 `std::error_code` | `include/mcp/ErrorCodes.hpp:36-39` |
| `make_error_code` | `error_code(static_cast<int>(e), GetMcpErrorCategory())` | `ErrorCodes.hpp:99-101` |
| 类目名 | `McpErrorCategory::name()` 恒为 `"MCP"` | `ErrorCodes.hpp:45` |
| `message()` | 20 项各一句英文文案，未命中返回 `"Unknown MCP error"` | `ErrorCodes.hpp:46-70` |
| `default_error_condition()` | `ConnectionClosed`/`RequestTimeout`/`ConnectionRefused`/`TlsHandshakeFailed`/`SessionExpired` → `std::errc::connection_aborted`；`InternalError`/`DeserializeFailed`/`ProtocolViolation`/`HandlerError` → `std::errc::io_error`；`TaskNotFound` → `no_such_process`；`ResourceNotFound` → `no_such_file_or_directory`；其余 → 本类目自身 | `ErrorCodes.hpp:71-91` |

```cpp
std::error_code ec = e.Code();          // 隐式走 make_error_code
bool aborted = ec == std::errc::connection_aborted;  // 覆盖 5 个码
```

::: tip
桥接只到"码"这一层：`McpError` 没有返回 `std::error_code` 的成员，`ErrorMessage()` 与 `Data()` 也不参与；跨层比较用 `Code()` 或按上表归并后的 `errc`。
:::

## 时代重映射

::: info
`MakeWireCodec` 以 `protocol_version >= kLatestProtocolVersion`（`"2026-07-28"`）选 `Rev2026Codec`，否则 `Rev2025Codec`（`src/protocol/WireCodec.cpp:315-322`）。重映射只作用于**出站错误响应的编码处**：`McpSessionHandler::SendErrorResponse`（`src/protocol/McpSessionHandler.cpp:1054-1059`）与响应投递分支（`:581`、`:592`、`:606`）。本地 `McpError` 对象、日志与 `data` 都不受影响。
:::

### 被改写的三个码

| 码 | 旧时代出站 | 现代时代出站 | 是否记告警 |
|---|---|---|---|
| `RequestTimeout`（-32001） | 原样 | `InternalError`（-32603） | 是，`Warning` 级 |
| `ConnectionRefused`（-32003） | 原样 | `InternalError`（-32603） | 是，`Warning` 级 |
| `TlsHandshakeFailed`（-32004） | 原样 | `InternalError`（-32603） | 是，`Warning` 级 |

证据：`src/protocol/WireCodec.cpp:271-283`（现代编解码器的 `EncodeErrorCode`）、`src/protocol/WireCodec.cpp:158-160`（旧编解码器的同名函数原样返回）。

### 原样透传的码

-32020、-32021、-32022、-32042 不在 `switch` 分支内，两个时代都原样出站；其余内部码（-32000、-32002、-32005、-32006、-32007、-32008、-32009、-32800）同样透传。现代时代的发起方无法从响应码区分超时/拒连/TLS 三类故障，只能靠 message 与对端告警日志。

## 错误的三种交付形态

| 形态 | 签名 | 错误如何呈现 | 证据 |
|---|---|---|---|
| 句柄式 | `RequestHandle SendRequest(std::string_view method, JsonValue params, const RequestOptions& options)` | `future<RequestOutcome>` **不抛**，错误落在 `ErrorData` 分支 | `include/mcp/client/McpClient.hpp:130-133`、`include/mcp/client/RequestHandle.hpp:14-25` |
| raw 式 | `future<JsonValue> SendRequestRaw(...)`；兼容重载 `SendRequest(..., meta, timeout)` | 错误被序列化为 `{"code","message"[,"data"]}` 的**正常返回值** | `McpClient.hpp:134-143`、`src/client/McpClient.cpp:1884-1903` |
| typed 包装 | `CallTool`/`ListTools`/`GetTask`/`Ping` 等 | 内部 `DoSendRequest` 见 `code < 0` 即 `throw McpError(code, message)` | `src/client/McpClient.cpp:74-89`、`:61-71` |

### `RequestOutcome`

`using RequestOutcome = std::variant<JsonValue, ErrorData>;`（`include/mcp/protocol/IncomingRequestMeta.hpp:29`），配套 `OutcomeCallback`（`:30`）。会话层自产的错误（连接关闭、超时、取消）直接构造 `ErrorData` 投递：`src/protocol/McpSessionHandler.cpp:273-276`、`:335-338`、`:1100-1106`、`:1128-1137`。`DeliverOutcome` 把同一个 `ErrorData` 既序列化给 raw 回调、又原样交给 typed 回调（`:96-113`）。

### 两种 future 的差别

`SendRequest(..., meta, timeout)` 只是转调 `SendRequestRaw`（`src/client/McpClient.cpp:1884-1903`），二者行为完全一致，差别仅在命名意图；真正的分界是 `RequestOptions` 重载。raw future 的错误**不是异常**，只需判断 `get()` 结果里 `code` 是否为负；`RequestHandle` 的 future 则需 `std::get_if<ErrorData>`。

::: warning
typed 包装在还原时只取 `code` 与 `message`，**`data` 被丢弃**（`src/client/McpClient.cpp:84-86`）。要读 `requiredCapabilities`、`supported` 这类载荷，必须用 `RequestOutcome` 或 raw 形态。
:::

### 对端未知码的处理不一致

- 客户端侧 `ExtractProtocolError` 不做白名单校验：任意负数 `code` 都直接 `static_cast` 成 `McpErrorCode`（`src/client/McpClient.cpp:61-71`）。
- 服务端侧（发起 elicitation 等处）用 `FindResponseErrorCode`，**不在白名单内的负数码被当成功载荷忽略**（`src/server/McpServer.cpp:485-491`、`:1181`、`:1258`）。

## 用户 handler 异常的收敛

### 服务端工具 handler

| handler 抛出 | 对外结果 | 码是否保留 | 证据 |
|---|---|---|---|
| `McpError`（同步/非任务路径） | `CallToolResult.isError = true`，`content[0].text = e.ErrorMessage()` | **丢弃**（含 `data`） | `src/server/McpServer.cpp:2633-2636` |
| 其它 `std::exception` | 同样 `isError = true`，文本泛化为 `Error executing tool <name>`，原文只进 `Error` 日志 | 丢弃 | `:2637-2643`、`:549-551` |
| `...` 未知异常 | 同上 | 丢弃 | `:2644-2650` |
| 参数不合 `inputSchema` | `isError = true`，文本 `invalid arguments: <原因>` | 无码 | `:2617-2623`、`:553` |
| 结果不合 `outputSchema` | JSON-RPC 错误 `InvalidParams` | 保留 | `:2675-2679` |

::: danger
工具 handler 抛 `McpError` **不会**变成 JSON-RPC 错误响应，对端只看到 `isError` 文本。想让对端拿到码，只能依赖 SDK 自身的错误（能力校验、`InvalidParams` 等）。
:::

### 协议层与资源/Prompt handler

非工具路径用 `promise.set_exception(std::current_exception())` 原样传播 `McpError`（`src/server/McpServer.cpp:2815`、`:2911`；客户端 `src/client/McpClient.cpp:747`），由 `McpSessionHandler::DeliverResponse` 编码为 JSON-RPC error：码经时代重映射、`message` 取 `e.what()`、`data` 保留（`src/protocol/McpSessionHandler.cpp:576-585`）。非 `McpError` 的异常一律收敛为 `InternalError` + 固定文本 `"internal error"`，原始 `e.what()` 只进日志（`:587-610`）。handler **同步**抛出（未走 promise）时：`McpError` → `reject(e.Code(), e.what(), e.Data())`（`:491-497`），其它异常 → `InternalError` + `request handler failed`（`:499-513`、`:116-117`）。

### 任务化路径

挂起为 task 的工具调用**保留码**：`McpError` → 失败载荷 `{"code","message"[,"data"]}`（`src/server/McpServer.cpp:2523-2530`；required-task 首轮 `:2372-2376`），其它异常 → `InternalError` + 泛化文本（`:2531-2551`）。**恢复续跑**分支与之不同：`std::exception` 的 `e.what()` 会原样写进失败载荷（`:1725-1736`），只有 `...` 才用固定文本（`:1737-1745`）。

## 症状 → 码 → 常见成因 → 定位手段

| 症状 | 码 | 常见成因 | 定位手段 |
|---|---|---|---|
| 工具失败但对端只看到文本 | 无（`isError`） | handler 异常被收敛为 `isError` 结果 | 设 `MCP_LOG_LEVEL=4` 看 `tool '<name>' failed` |
| 现代时代超时/拒连/TLS 都成内部错误 | -32603（原 -32001/-32003/-32004） | 时代重映射 | 搜告警 `internal error code remapped` |
| 握手后反复重探或直接失败 | -32022 | 声明版本不在 supported | 读 `data.supported`/`requested`，对照 [协议版本](/zh/learn/protocol-versions) |
| HTTP 请求一律 400 | -32600/-32020 | 信封缺 `id`、缺 `Mcp-Method`/`Mcp-Name` 头 | 状态码映射 `src/http/StreamableHttpServerTransport.cpp:116-129` |
| 方法名没错却 404 | -32601 | 方法不在协商时代，或扩展未启用 | `WireValidation::NotInEra` 分支 `src/protocol/McpSessionHandler.cpp:414` |
| 空闲后首个请求失败、重试即好 | -32009 | 状态化会话被回收 | `reinit_on_expired_session`（默认 true）`include/mcp/client/ClientOptions.hpp:67` |
| 长任务中途断流 | -32000 | 读到底或对端关闭 | 用 `IsEof()` 区分超时与 EOF，见 [传输层参考](/zh/reference/transports) |
| 取消后拿不到结果 | -32800 | 对端 `notifications/cancelled` 或本地 `Cancel` | 检查 `data.reason`；`src/protocol/McpSessionHandler.cpp:1100` |
| 列表翻页跑不完 | -32005 | 对端 `nextCursor` 不收敛（>64 页） | `src/client/McpClient.cpp:169`、`:1392` |
| 连 TLS 端点必失败 | -32004 | 未编译 OpenSSL（`TLS support not compiled in`） | 确认 `MCP_HAVE_OPENSSL`，见 [安装与集成](/zh/start/installation) |
| MRTR 多轮后突然报错 | -32007 或 -32603 | handler 返回非法 action / 超出 `max_rounds` | `src/client/McpClient.cpp:290`、`:1133` |

## 本页对应源码

- `include/mcp/ErrorCodes.hpp`、`include/mcp/McpError.hpp`、`include/mcp/JsonRpc.hpp`
- `include/mcp/protocol/IncomingRequestMeta.hpp`、`include/mcp/client/RequestHandle.hpp`、`include/mcp/client/McpClient.hpp`、`include/mcp/client/ClientOptions.hpp`
- 协议层：`src/protocol/WireCodec.cpp`、`src/protocol/McpSessionHandler.cpp`
- 服务/客户端：`src/server/McpServer.cpp`、`src/client/McpClient.cpp`
- core：`src/core/JsonParser.cpp`、`src/core/JsonRpc.cpp`、`src/core/JsonValue.cpp`、`src/core/Content.cpp`
- 传输与网络：`src/http/StreamableHttpServerTransport.cpp`、`src/http/StreamableHttpClientTransport.cpp`、`src/transport/detail/net/HttpClient.cpp`、`src/transport/detail/net/TlsSocket.cpp`、`src/transport/detail/net/TcpSocketPosix.cpp`、`src/transport/detail/net/TcpSocketWin32.cpp`
- 其他抛出点：`src/detail/AtomicJsonFile.cpp`、`src/ext/DPoP.cpp`
