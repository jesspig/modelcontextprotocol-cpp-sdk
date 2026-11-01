# 传输层参考

本页是传输层与 HTTP 服务端底座公共符号的**唯一权威字段表**，字段、类型、默认值逐条对照头文件；概念叙述与选型理由见 [传输层概念](/zh/learn/transports)，服务端/客户端门面签名见 [服务端参考](/zh/reference/server) 与 [客户端参考](/zh/reference/client)，持久化与进程见 [进程与存储参考](/zh/reference/process-and-storage)。本页不含操作步骤。

## 传输契约

### `ITransport`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `SessionId` | `virtual std::string_view SessionId() const = 0` | — | 会话标识；无会话实现返回空串 | `include/mcp/Transport.hpp:21` |
| `GetMessageChannel` | `virtual MessageChannel& GetMessageChannel() = 0` | — | 入站消息队列引用 | `Transport.hpp:22` |
| `SendMessageAsync` | `virtual void SendMessageAsync(JsonRpcMessage message) = 0` | — | 按值接收出站消息，非阻塞语义由实现决定 | `Transport.hpp:23` |
| `BufferRequestStreamNotification` | `virtual bool BufferRequestStreamNotification(JsonRpcMessage)` | `false` | 把通知缓冲进当前请求流；返回是否已吞下该消息 | `Transport.hpp:24` |
| `RequiresRequestScopedNotifications` | `virtual bool RequiresRequestScopedNotifications() const` | `false` | 是否要求通知按请求作用域投递 | `Transport.hpp:25` |
| `Close` | `virtual void Close() = 0` | — | 幂等关闭；IO 线程回调内调用须走 `detail::JoinThreadSafely` | `Transport.hpp:26` |
| `IsStateless` | `virtual bool IsStateless() const` | `false` | 会话语义判定入口 | `Transport.hpp:27` |
| `Start` | `virtual void Start()` | 空实现 | 连接后显式启动读线程 | `Transport.hpp:28` |

### `TransportState` 与 `TransportBase`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `TransportState` | `enum class TransportState { Initial, Connected, Disconnected }` | `Initial` | 三态状态机，底层用 `std::atomic<int>` | `Transport.hpp:31`、`62` |
| `TransportBase` | `class TransportBase : public ITransport, public std::enable_shared_from_this<TransportBase>` | — | 所有会话侧传输的公共基类，持有 `channel_`/`session_id_` | `Transport.hpp:33`、`60-61` |
| `SetConnected` | `void SetConnected()` | — | 仅 `Initial → Connected` 生效（CAS），否则记 Warning 日志 | `Transport.hpp:42`、`80-86` |
| `SetDisconnected` | `void SetDisconnected()` | — | 重复调用只触发一次 `on_close_` | `Transport.hpp:43`、`88-93` |
| `GetState` | `TransportState GetState() const` | `Initial` | 原子读取当前状态 | `Transport.hpp:44` |
| `SetOnClose` / `SetOnError` | `void SetOnClose(std::function<void()>)` / `void SetOnError(std::function<void(std::string_view)>)` | 未设置 | 关闭与出错回调，加锁写入；读循环失败经 `SetOnError` 上报 | `Transport.hpp:46-53`、`58`、`111-114` |

### `IClientTransport`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `Name` | `virtual std::string_view Name() const = 0` | — | 传输名；各实现名为空时回退内置默认名 | `Transport.hpp:71` |
| `Connect` | `virtual std::shared_ptr<ITransport> Connect() = 0` | — | 连接工厂，返回会话对象（不是 `unique_ptr`） | `Transport.hpp:72` |

### `MessageChannel`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| 构造 | `explicit MessageChannel(size_t max_buffer = 64)` | `64` | 传输基类实际用 `detail::kChannelCapacity` | `include/mcp/protocol/MessageChannel.hpp:18`、`Transport.hpp:76` |
| `AsyncReceive` | `template <typename Callback> void AsyncReceive(Callback&& cb)` | — | 阻塞取消息，关闭后回 `operation_canceled` | `MessageChannel.hpp:21-38` |
| `Send` | `bool Send(JsonRpcMessage message)` | — | 队列满则阻塞等待 | `MessageChannel.hpp:40` |
| `TrySend` | `bool TrySend(JsonRpcMessage message)` | — | 满或关闭立即返回 `false` | `MessageChannel.hpp:51` |
| `Close` / `IsOpen` / `Empty` | `void Close()` / `bool IsOpen() const` / `bool Empty() const` | 打开 | 关闭唤醒两侧条件变量 | `MessageChannel.hpp:61`、`70`、`72` |

## stdio

### `StdioServerTransport`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| 构造 | `StdioServerTransport()` / `explicit StdioServerTransport(std::optional<std::size_t> max_frame_bytes)` | `detail::kMaxStdioFrameBytes`（8 MiB） | 进程自身 stdin/stdout 即管道，无 Options 结构体 | `include/mcp/transport/StdioServerTransport.hpp:18-19`、`33` |
| `Start` / `Close` / `SendMessageAsync` | `override` | — | `Start()` 拉起读线程；`SendMessageAsync` 写单行 JSON | `StdioServerTransport.hpp:22-24` |

### `StdioClientTransport`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| 构造 | `explicit StdioClientTransport(const StdioClientTransportOptions& options)` | — | 只存配置，不启动子进程 | `include/mcp/transport/StdioClientTransport.hpp:33` |
| `Name` | `std::string_view Name() const override` | `"stdio"` | `options.name` 为空时回退 | `StdioClientTransport.hpp:36`；`src/transport/StdioClientTransport.cpp:186` |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | 拉起子进程并返回会话传输；此处才校验选项互斥 | `StdioClientTransport.hpp:37`；`src/transport/StdioClientTransport.cpp:189-192` |
| `Pid` | `std::optional<int64_t> Pid() const` | `std::nullopt` | 仅 `Connect()` 成功后有值 | `StdioClientTransport.hpp:38`、`42` |

### `StdioClientTransportOptions`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `command` | `std::string` | 空（必填） | 子进程可执行文件 | `StdioClientTransport.hpp:18` |
| `arguments` | `std::vector<std::string>` | `{}` | 命令行参数 | `StdioClientTransport.hpp:19` |
| `name` | `std::string` | `""` | 空时 `Name()` 回退 `"stdio"` | `StdioClientTransport.hpp:20` |
| `working_directory` | `std::string` | `""` | 子进程工作目录 | `StdioClientTransport.hpp:21` |
| `inherit_environment_variables` | `bool` | `true` | 是否继承父进程环境 | `StdioClientTransport.hpp:22` |
| `environment_variables` | `std::map<std::string, std::string>` | `{}` | 追加环境变量 | `StdioClientTransport.hpp:23` |
| `stderr_path` | `std::string` | `""` | 子进程 stderr 落盘路径 | `StdioClientTransport.hpp:24` |
| `on_stderr` | `std::function<void(std::string_view)>` | 未设置 | 捕获 stderr 到回调（独立读线程） | `StdioClientTransport.hpp:25`；`src/transport/StdioClientTransport.cpp:203`、`57-61` |
| `shutdown_timeout` | `std::chrono::milliseconds` | `{2000}` | 关闭时 SIGTERM→SIGKILL 的宽限窗口，传给 `ProcessHandle::Terminate` | `StdioClientTransport.hpp:26`；`src/transport/StdioClientTransport.cpp:68-69` |
| `terminate_process_tree` | `bool` | `false` | 连带终止整个进程树（POSIX 进程组 / Windows 作业对象） | `StdioClientTransport.hpp:27`；`src/transport/detail/posix_platform.cpp:350`、`159-161`；`src/transport/detail/win32_platform.cpp:511`、`534-536` |
| `max_frame_bytes` | `std::optional<std::size_t>` | `detail::kMaxStdioFrameBytes`（8 MiB） | 单帧上限，超限经 `SetOnError` 报 `stdio frame size exceeds maximum allowed size` | `StdioClientTransport.hpp:28`；`src/transport/StdioClientTransport.cpp:138-141`、`151-154` |

::: warning
`stderr_path` 与 `on_stderr` 互斥：两者同时设置时 `Connect()` 抛 `McpError(McpErrorCode::InvalidParams, "stderr_path and on_stderr are mutually exclusive")`（`src/transport/StdioClientTransport.cpp:189-192`）。
:::

## Streamable HTTP 服务端

### `StreamableHttpServerTransport`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| 构造 | `explicit StreamableHttpServerTransport(StreamableHttpServerOptions options = {})` | 默认构造选项 | 构造即建 `HttpServer` 并注册路由 | `include/mcp/transport/StreamableHttpServerTransport.hpp:89-90` |
| `Start` / `Close` / `SendMessageAsync` | `override` | — | 启停底层 HTTP 监听 | `StreamableHttpServerTransport.hpp:94-96` |
| `BufferRequestStreamNotification` | `bool BufferRequestStreamNotification(JsonRpcMessage) override` | — | 缓冲进持开的请求流 | `StreamableHttpServerTransport.hpp:97` |
| `RequiresRequestScopedNotifications` | `bool ... const override` | `true` | 恒为真（硬编码） | `StreamableHttpServerTransport.hpp:98` |
| `IsStateless` | `bool IsStateless() const override` | 取 `options_.stateless` | 无状态模式的判定源 | `StreamableHttpServerTransport.hpp:99` |
| `ValidateMcpHeaders` | `static bool ValidateMcpHeaders(const std::string&, const std::string&, const JsonValue&, std::string& error_out)` | — | SEP-2243 头与请求体一致性校验 | `StreamableHttpServerTransport.hpp:101-105` |
| `AuthResult` | `struct { bool ok{false}; std::vector<std::string> scopes; std::optional<std::string> resource; }` | `ok=false` | `BearerAuthConfig::verify` 的返回载体 | `StreamableHttpServerTransport.hpp:31-35` |

### `StreamableHttpServerOptions`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `port` | `uint16_t` | `kDefaultPort` = `3001` | 监听端口 | `StreamableHttpServerTransport.hpp:40`、`29` |
| `endpoint` | `std::string` | `"/mcp"` | MCP 请求路径 | `StreamableHttpServerTransport.hpp:41` |
| `stateless` | `bool` | `true` | 无状态模式（2026-07-28 起） | `StreamableHttpServerTransport.hpp:43` |
| `enable_legacy_sse` | `bool` | `false` | 关闭时不注册 GET，监听流与事件回放不可用 | `StreamableHttpServerTransport.hpp:45` |
| `listen_hold_open` | `bool` | `false` | 现代版本下让 `subscriptions/listen` 持开 POST 响应流，改由 POST 流承载监听 | `StreamableHttpServerTransport.hpp:47`；`src/http/StreamableHttpServerTransport.cpp:1286-1293` |
| `sse_keep_alive_ms` | `int` | `15000` | SSE 注释帧间隔，`0` 禁用 | `StreamableHttpServerTransport.hpp:49` |
| `event_store` | `std::shared_ptr<EventStore>` | `nullptr` | 空时用内置进程内 `EventStore` | `StreamableHttpServerTransport.hpp:51` |
| `session_store` | `std::shared_ptr<SessionStore>` | `nullptr` | 仅会话模式生效；可注入 `InMemorySessionStore` 等实现跨实例接管 | `StreamableHttpServerTransport.hpp:53` |
| `server_name` | `std::string` | `"mcp-server"` | 发现信息用名 | `StreamableHttpServerTransport.hpp:55` |
| `server_version` | `std::string` | `kSdkVersion` | 发现信息用版本 | `StreamableHttpServerTransport.hpp:56` |
| `host` | `std::string` | `""` | 绑定地址；空 = IPv4 `INADDR_ANY`，接受 IPv4/IPv6 字面量，非法值抛异常 | `StreamableHttpServerTransport.hpp:58`；`src/http/HttpServerImpl.cpp:173-197` |
| `allowed_hosts` | `std::vector<std::string>` | `{}` | Host 白名单；空时仅放行 localhost/回环，缺 `Host` 头直接拒绝 | `StreamableHttpServerTransport.hpp:60`；`src/http/HttpServerImpl.cpp:766-785` |
| `allowed_origins` | `std::vector<std::string>` | `{}` | Origin 白名单；无 `Origin` 头时放行 | `StreamableHttpServerTransport.hpp:62`；`src/http/HttpServerImpl.cpp:787-797` |
| `max_request_body_bytes` | `std::optional<std::size_t>` | `detail::kMaxHttpBodyBytes`（4 MiB） | 请求体上限 | `StreamableHttpServerTransport.hpp:64` |
| `max_sse_event_bytes` | `std::optional<std::size_t>` | `detail::kMaxSseEventBytes`（1 MiB） | 单个 SSE 事件上限 | `StreamableHttpServerTransport.hpp:65` |
| `bearer_auth` | `std::optional<BearerAuthConfig>` | `std::nullopt` | 未设置时无鉴权门，行为不变 | `StreamableHttpServerTransport.hpp:77` |
| `resolve_param_annotations` | `std::function<std::vector<McpParamAnnotationInfo>(const std::string& method, const std::string& name)>` | 未设置 | 按方法/工具名解析 `Mcp-Param-*` 头注解；未设置时跳过参数头校验 | `StreamableHttpServerTransport.hpp:79-81`；`src/http/StreamableHttpServerTransport.cpp:1016-1023` |
| `enable_server_card` | `bool` | `false` | 注册 `GET /server-card` 发现端点 | `StreamableHttpServerTransport.hpp:83`；`src/http/StreamableHttpServerTransport.cpp:564-568` |
| `server_card` | `std::optional<ext::ServerCard>` | `std::nullopt` | 未提供时由 `BuildDefaultServerCard()` 生成 | `StreamableHttpServerTransport.hpp:84`；`src/http/StreamableHttpServerTransport.cpp:751` |

### `StreamableHttpServerOptions::BearerAuthConfig`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `verify` | `std::function<AuthResult(const std::string& token)>` | 未设置（必填） | 令牌校验；`ok=false` → 401 `invalid_token` | `StreamableHttpServerTransport.hpp:67` |
| `resource_metadata_url` | `std::string` | `""` | 受保护资源元数据 URL，同时写入 `WWW-Authenticate` | `StreamableHttpServerTransport.hpp:68` |
| `scopes_supported` | `std::vector<std::string>` | `{}` | 元数据声明的作用域集合 | `StreamableHttpServerTransport.hpp:69` |
| `required_scopes` | `std::vector<std::string>` | `{}` | 空则不校验作用域；不足 → 403 `insufficient_scope` | `StreamableHttpServerTransport.hpp:70` |
| `authorization_servers` | `std::vector<std::string>` | `{}` | 写入元数据文档 | `StreamableHttpServerTransport.hpp:71` |
| `serve_metadata_endpoint` | `bool` | `true` | 注册免鉴权的 `GET /.well-known/oauth-protected-resource` | `StreamableHttpServerTransport.hpp:72` |
| `expected_resource` | `std::string` | `""` | 期望 audience；空则不校验 | `StreamableHttpServerTransport.hpp:73` |
| `validate_token_resource` | `bool` | `true` | audience 校验开关 | `StreamableHttpServerTransport.hpp:74` |
| `enable_dpop` | `bool` | `false` | DPoP 证明校验（实验特性，需 OpenSSL） | `StreamableHttpServerTransport.hpp:75` |

## Streamable HTTP 客户端

### `StreamableHttpClientTransport`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| 构造 | `explicit StreamableHttpClientTransport(const HttpClientTransportOptions& options)` | — | 只存配置 | `include/mcp/transport/StreamableHttpClientTransport.hpp:65` |
| `Name` | `std::string_view Name() const override` | `"streamable-http"` | `name` 为空时回退 | `StreamableHttpClientTransport.hpp:68`；`src/http/StreamableHttpClientTransport.cpp:2279` |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | 返回会话传输 | `StreamableHttpClientTransport.hpp:69` |

### `HttpClientTransportOptions`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `endpoint` | `std::string` | 空（必填） | 服务端 URL；`https` 走 TLS | `StreamableHttpClientTransport.hpp:45` |
| `transport_mode` | `HttpTransportMode` | `AutoDetect` | 声明字段：`src/` 内无任何读取点，实际探测由 `McpClient` 版本协商完成 | `StreamableHttpClientTransport.hpp:46`；`src/client/McpClient.cpp:490` |
| `name` | `std::string` | `""` | 空回退 `"streamable-http"` | `StreamableHttpClientTransport.hpp:47` |
| `known_session_id` | `std::string` | `""` | 恢复既有会话 | `StreamableHttpClientTransport.hpp:48` |
| `additional_headers` | `std::map<std::string, std::string>` | `{}` | 静态附加头 | `StreamableHttpClientTransport.hpp:49` |
| `auth_challenge_handler` | `std::function<std::string(std::string_view www_authenticate)>` | 未设置 | 401 必调、含 `insufficient_scope` 的 403 才调；返回非空则恰好重试一次 | `StreamableHttpClientTransport.hpp:50` |
| `apply_per_request_headers` | `std::function<void(const HttpRequestHeaderContext&, std::map<std::string, std::string>&)>` | 未设置 | 每次发送前调用，写进可写 `headers` | `StreamableHttpClientTransport.hpp:51-53` |
| `observe_response_challenge` | `std::function<void(const HttpResponseChallengeContext&)>` | 未设置 | 每个响应（含 2xx 与 SSE）回调一次 | `StreamableHttpClientTransport.hpp:54-55` |
| `enable_listen_stream` | `std::optional<bool>` | 未设置 → `!stateless` | GET 监听流开关 | `StreamableHttpClientTransport.hpp:56`；`src/http/StreamableHttpClientTransport.cpp:153` |
| `stateless` | `bool` | `false` | 纯无状态对端：不发 `Mcp-Session-Id`/`Last-Event-ID`，不发 DELETE | `StreamableHttpClientTransport.hpp:57` |
| `max_sse_event_bytes` | `std::optional<std::size_t>` | `detail::kMaxSseEventBytes`（1 MiB） | 单事件上限 | `StreamableHttpClientTransport.hpp:58` |
| `redirect_policy` | `detail::HttpRedirectPolicy` | `SameOriginOnly` | 重定向策略（枚举在 `mcp::detail` 命名空间） | `StreamableHttpClientTransport.hpp:59`；`include/mcp/transport/detail/Limits.hpp:8-12` |
| `max_redirects` | `int` | `detail::kMaxHttpRedirects` = `5` | 最大跳数 | `StreamableHttpClientTransport.hpp:60`；`Limits.hpp:20` |

### 请求用途与回调上下文

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `HttpRequestPurpose` | `enum class { MessagePost, ListenStream, ResumeStream, SessionDelete, ToolAnnotationsRefresh }` | `MessagePost` | 区分五类出站请求 | `StreamableHttpClientTransport.hpp:22-28`、`33` |
| `HttpRequestHeaderContext` | `struct { std::string_view http_method; std::string_view url; HttpRequestPurpose purpose; std::optional<RequestId> request_id; int attempt_index; }` | `attempt_index = 0` | 逐请求追加头时的上下文 | `StreamableHttpClientTransport.hpp:30-36` |
| `HttpResponseChallengeContext` | `struct { int status_code; std::string_view www_authenticate; std::string_view dpop_nonce; }` | `status_code = 0` | 挑战观察回调入参 | `StreamableHttpClientTransport.hpp:38-42` |

### `HttpTransportMode`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `AutoDetect` / `StreamableHttp` / `Sse` | `enum class HttpTransportMode` | `AutoDetect` | 声明于 `include/mcp/transport/HttpTransportMode.hpp:7-11`，为 `transport_mode` 的取值类型 | `HttpTransportMode.hpp:7-11` |

::: info
`transport_mode` 与 `HttpTransportMode` 目前只出现在公共头文件与测试里（`tests/unit/StreamableHttpTransportTests.cpp:68`、`93`），生产代码路径未消费；不要据此判断连接行为。
:::

## SSE 与 WebSocket 客户端

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `SseClientTransport` | `class MCP_API SseClientTransport : public IClientTransport` | — | 传统 SSE 客户端传输 | `include/mcp/transport/SseClientTransport.hpp:21` |
| 构造 | `explicit SseClientTransport(std::string_view server_url, std::string_view name = {})` / `explicit SseClientTransport(SseClientTransportOptions options)` | `name` 空 → `"sse"` | 两个重载都把空 `name` 归一为 `"sse"` | `SseClientTransport.hpp:23-24`；`src/transport/SseClientTransport.cpp:369`、`375` |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | 建立 SSE 长连接 | `SseClientTransport.hpp:28` |
| `SseClientTransportOptions::server_url` | `std::string` | 空（必填） | SSE 端点 | `SseClientTransport.hpp:14` |
| `SseClientTransportOptions::name` | `std::string` | `"sse"` | 唯一显式带默认值的名字字段 | `SseClientTransport.hpp:15` |
| `max_sse_event_bytes` | `std::optional<std::size_t>` | `detail::kMaxSseEventBytes` | 单事件上限 | `SseClientTransport.hpp:16` |
| `redirect_policy` | `detail::HttpRedirectPolicy` | `SameOriginOnly` | 重定向策略 | `SseClientTransport.hpp:17` |
| `max_redirects` | `int` | `5` | 最大跳数 | `SseClientTransport.hpp:18` |
| `WebSocketClientTransport` | `class WebSocketClientTransport : public IClientTransport` | — | WebSocket 客户端传输；**没有 Options 结构体**，只有两个构造参数 | `include/mcp/transport/WebSocketClientTransport.hpp:11`、`13` |
| 构造 | `WebSocketClientTransport(std::string url, std::string name = "websocket")` | `name = "websocket"` | `Name()` 原样返回构造名，无回退逻辑 | `WebSocketClientTransport.hpp:13`、`16` |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | 只接受 `ws`/`wss`，`wss` 走 TLS，连接超时 30s | `WebSocketClientTransport.hpp:17`；`src/transport/detail/net/WebSocketClient.cpp:67-76` |

## 进程内传输

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `InMemoryTransport` | `class MCP_API InMemoryTransport` | — | 类体只有嵌套 `Pair` 与静态 `CreatePair()`，无任何实例成员：把它当命名空间用 | `include/mcp/transport/InMemoryTransport.hpp:10-18` |
| `InMemoryTransport::Pair` | `struct { std::shared_ptr<ITransport> client; std::shared_ptr<ITransport> server; }` | — | **两条端点句柄的数据载体**，本身不建通道 | `InMemoryTransport.hpp:12-15` |
| `CreatePair` | `static Pair CreatePair()` | — | **工厂函数**：交叉连接两条 `MessageChannel` 并返回已成对的 `Pair`；实现体 `InMemoryTransportImpl` 在源文件匿名 namespace 内，非公共 API | `InMemoryTransport.hpp:17`；`src/transport/InMemoryTransport.cpp:42-53` |

::: tip
区别一句话：`Pair` 只是返回类型（不是工厂、也不能自建通道），唯一入口是静态 `InMemoryTransport::CreatePair()`。
:::

## 请求取消

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `IRequestCanceller` | `class MCP_API IRequestCanceller` | — | 独立接口，不改 `ITransport` 虚表 | `include/mcp/transport/IRequestCanceller.hpp:10` |
| `CancelRequest` | `virtual void CancelRequest(const RequestId& id, std::string_view reason) = 0` | — | 现代 Streamable HTTP 用它关闭请求作用域流 | `IRequestCanceller.hpp:13` |

## HTTP 服务端底座

`HttpServer`/`EventStore`/`SessionStore` 是 Streamable HTTP 服务端的底座，也可独立使用；`FileEventStore`、`FileTaskStore`、`Process` 见 [进程与存储参考](/zh/reference/process-and-storage)。

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `HttpRequest` | `struct { std::string method; std::string path; std::unordered_map<std::string, std::string> headers; std::string body; }` | — | 入站请求视图 | `include/mcp/http/HttpServer.hpp:20-25` |
| `HttpResponse` | `struct`，含 `status_code`/`status_text`/`headers`/`body`/`is_sse`/`sse_close_after_write`/`sse_client_tag` | `200`、`"OK"`、其余空/`false` | 后两字段控制 SSE 定向与写完即断 | `HttpServer.hpp:27-35` |
| `HttpHandler` | `std::function<void(const HttpRequest&, HttpResponse&)>` | — | 路由处理体 | `HttpServer.hpp:37` |
| `HttpServerOptions` | `struct`，含 `on_request`/`on_connect`/`on_disconnect`/`allowed_hosts`/`allowed_origins`/`sse_keep_alive_ms`/`bind_host`/`max_request_body_bytes` | `0`、`""`、`kMaxHttpBodyBytes` | 传输层把 `host`/`allowed_*` 原样转发到此 | `HttpServer.hpp:43-56`；`src/http/StreamableHttpServerTransport.cpp:500-507` |
| `HttpServer` | `HttpServer(uint16_t port, const HttpServerOptions& options = {})`，配 `void Start()` / `void Stop()` | 选项默认构造 | 非 `ITransport`，独立生命周期 | `HttpServer.hpp:61-62`、`65`、`67` |
| `SetHandler` | `void SetHandler(std::string_view method, std::string_view path, HttpHandler handler)` | — | `(method, path)` 精确匹配 | `HttpServer.hpp:69-70` |
| `AddSseClient` / `RemoveSseClient` / `SseClientCount` | `SseClientId AddSseClient(send_fn[, std::string tag])` / `void RemoveSseClient(SseClientId)` / `std::size_t SseClientCount() const` | 无 tag | `SseClientId = uint64_t`；带 tag 版本供定向广播与过滤 | `HttpServer.hpp:72-76`、`80` |
| `BroadcastSse` | `void BroadcastSse(std::string_view event[, const std::function<bool(const std::string&)>& accept])` | 全发 | 谓词版按 tag 过滤 | `HttpServer.hpp:77-79` |

### `EventStore`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `Append` | `virtual uint64_t Append(std::string_view session_id, std::string event_data)` | — | 返回单调递增事件 id（起始 `1`） | `include/mcp/http/EventStore.hpp:20`、`37` |
| `GetEventsSince` | `virtual std::vector<std::pair<uint64_t, std::string>> GetEventsSince(std::string_view, uint64_t) const` | — | 回放 `Last-Event-ID` 之后的事件 | `EventStore.hpp:22-23` |
| `Clear` | `virtual void Clear(std::string_view session_id)` | — | 会话结束回收 | `EventStore.hpp:25` |
| `kMaxEventsPerSession` | `static constexpr size_t` | `10000` | 每会话环形上限 | `EventStore.hpp:27` |

### `SessionStore` 与 `InMemorySessionStore`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `SessionRecord` | `struct { std::string protocol_version; int64_t created_at_ms{0}; int64_t last_seen_ms{0}; }` | `0` | 跨进程接管的最小载荷 | `include/mcp/http/SessionStore.hpp:17-21` |
| `Save` / `Load` / `Remove` | 纯虚 `void` / `std::optional<SessionRecord>` / `void` | — | 三个必备接口 | `SessionStore.hpp:27-29` |
| `TrySave` | `virtual bool TrySave(const std::string&, const SessionRecord&)` | 转 `Save` 返 `true` | 满容时的非抛出路径 | `SessionStore.hpp:31-34` |
| `Size` / `SweepExpired` | `virtual size_t() const` / `virtual size_t()` | `0` | 可选实现，基类返回 `0` | `SessionStore.hpp:36`、`38` |
| `InMemorySessionStore` | `class InMemorySessionStore : public SessionStore` | — | 进程内实现；`Save` 满时抛 `std::runtime_error("session store is full")` | `SessionStore.hpp:41`、`43-46` |
| 过期规则 | `now - max(last_seen_ms, created_at_ms) > detail::kSessionIdleTimeoutMs` | 30 分钟 | `Load` 命中过期即删并返回 `nullopt` | `SessionStore.hpp:62-73`、`96-105`；`Limits.hpp:24` |

## 限额与重定向策略

`include/mcp/transport/detail/Limits.hpp` 位于 `mcp::detail` 命名空间：被公共 Options 直接引用，但不属于稳定 API，改值前先对照本表默认值出处。

| 符号 | 类型 | 值 | 说明 | 证据 |
|---|---|---|---|---|
| `kMaxHttpBodyBytes` | `size_t` | 4 MiB | HTTP 请求体默认上限 | `Limits.hpp:15` |
| `kMaxSseEventBytes` | `size_t` | 1 MiB | SSE 单事件默认上限 | `Limits.hpp:17` |
| `kMaxStdioFrameBytes` | `size_t` | 8 MiB | stdio 单帧默认上限 | `Limits.hpp:19` |
| `kMaxHttpRedirects` | `int` | `5` | 重定向跳数默认值 | `Limits.hpp:20` |
| `kChannelCapacity` | `size_t` | `64` | `MessageChannel` 深度 | `Limits.hpp:22` |
| `kMaxHttpSessions` | `size_t` | `10000` | 会话存储上限（满 → 503 / `TrySave` 失败） | `Limits.hpp:23` |
| `kSessionIdleTimeoutMs` | `int64_t` | `1800000` | 会话闲置过期 | `Limits.hpp:24` |
| `HttpRedirectPolicy` | `enum class { SameOriginOnly, FollowAll, NoFollow }` | `SameOriginOnly` | 必须写全 `mcp::detail::HttpRedirectPolicy` | `Limits.hpp:8-12` |

## TLS 与 OpenSSL（可选）

| 符号 | 形式 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `MCP_WITH_OPENSSL` | CMake 缓存项 `AUTO|ON|OFF` | `AUTO` | `AUTO` 按 `find_package` 结果；`ON` 找不到即 configure 失败；`OFF` 强制禁用 | `cmake/FetchDependencies.cmake:7-16`、`27-43` |
| `MCP_HAVE_OPENSSL` | 编译宏 | 未定义 = 无 TLS | 由 INTERFACE 目标 `mcp-openssl` 统一施加到 `mcp-transport`/`mcp-client`/`mcp-ext` | `CMakeLists.txt:226`、`229-237` |
| `MCP_OPENSSL_EXPECTED_MAJOR` | CMake 缓存项 | `""` | CI 锁定主版本用；支持 3.x/4.x，其他主版本 configure 失败 | `FetchDependencies.cmake:11-12`；`cmake/OpenSslVersion.cmake:8-12` |
| `https`/`wss` 连接 | 运行期行为 | — | 未启用 OpenSSL 时抛 `McpError(TlsHandshakeFailed, "TLS support not compiled in")` | `src/transport/detail/net/TlsSocket.cpp:279` |
| PKCE 随机源 | 编译期回退 | — | 有 OpenSSL 用 `RAND_bytes`，否则 Windows `BCryptGenRandom`、其他平台 `random_device`；SHA-256 恒为内置实现 | `src/client/auth/OAuthClientProvider.cpp:291-301` |
| TLS 参数 | 内部实现，非公共 API | TLS 1.2 下限 | 系统信任链校验；`TlsSocket`/`TcpSocket`/`HttpClient`/`WebSocketClient` 均在 `src/transport/detail/net/`，不可作为用户 API 引用 | `src/transport/detail/net/TlsSocket.cpp:54-55` |

## stateful 与 stateless

| 场景 | `IsStateless()` | 会话与通知 | 证据 |
|---|---|---|---|
| stdio / InMemory / SSE / WebSocket | `false`（`TransportBase` 恒 `false`） | 单会话长连接，通知走同一通道 | `Transport.hpp:40` |
| Streamable HTTP 服务端（默认 `stateless = true`） | `true` | 每请求独立、同步 JSON 响应（截止 `kStatelessTimeout` = 30s），零监听者时跳过等待 | `StreamableHttpServerTransport.hpp:43`、`99`；`src/http/StreamableHttpServerTransport.cpp:38`、`1263-1272`、`1319`、`1511` |
| Streamable HTTP 会话模式（`stateless = false`） | `false` | 发 `Mcp-Session-Id`、支持 GET 监听流与 `EventStore` 回放 | `StreamableHttpServerTransport.hpp:43`、`51` |

::: tip
概念叙述、模式选择与协商细节见 [传输层概念](/zh/learn/transports)；版本协商纠正重试规则见 [版本协商](/zh/learn/protocol-versions)。
:::

## 本页对应源码

- `include/mcp/Transport.hpp`、`include/mcp/protocol/MessageChannel.hpp`、`include/mcp/transport/IRequestCanceller.hpp`
- `include/mcp/transport/StdioServerTransport.hpp`、`include/mcp/transport/StdioClientTransport.hpp`
- `include/mcp/transport/StreamableHttpServerTransport.hpp`、`include/mcp/transport/StreamableHttpClientTransport.hpp`、`include/mcp/transport/HttpTransportMode.hpp`
- `include/mcp/transport/SseClientTransport.hpp`、`include/mcp/transport/WebSocketClientTransport.hpp`、`include/mcp/transport/InMemoryTransport.hpp`
- `include/mcp/http/HttpServer.hpp`、`include/mcp/http/EventStore.hpp`、`include/mcp/http/SessionStore.hpp`、`include/mcp/transport/detail/Limits.hpp`（末者被公共 Options 引用，非稳定 API）
- 行为佐证：`src/transport/StdioClientTransport.cpp`、`src/http/StreamableHttpServerTransport.cpp`、`src/http/StreamableHttpClientTransport.cpp`、`src/http/HttpServerImpl.cpp`、`src/transport/detail/net/TlsSocket.cpp`、`cmake/FetchDependencies.cmake`、`cmake/OpenSslVersion.cmake`、`CMakeLists.txt`
