# 传输层参考

本页是传输层与 HTTP 服务端底座公共符号的**唯一权威字段表**，字段、类型、默认值逐条对照头文件；概念叙述与选型理由见 [传输层概念](/zh/learn/transports)，服务端/客户端门面签名见 [服务端参考](/zh/reference/server) 与 [客户端参考](/zh/reference/client)，持久化与进程见 [进程与存储参考](/zh/reference/process-and-storage)。本页不含操作步骤。

## 传输契约

### `ITransport`

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `SessionId` | `virtual std::string_view SessionId() const = 0` | — | 会话标识；无会话实现返回空串 |
| `GetMessageChannel` | `virtual MessageChannel& GetMessageChannel() = 0` | — | 入站消息队列引用 |
| `SendMessageAsync` | `virtual void SendMessageAsync(JsonRpcMessage message) = 0` | — | 按值接收出站消息，非阻塞语义由实现决定 |
| `BufferRequestStreamNotification` | `virtual bool BufferRequestStreamNotification(JsonRpcMessage)` | `false` | 把通知缓冲进当前请求流；返回是否已吞下该消息 |
| `RequiresRequestScopedNotifications` | `virtual bool RequiresRequestScopedNotifications() const` | `false` | 是否要求通知按请求作用域投递 |
| `Close` | `virtual void Close() = 0` | — | 幂等关闭；IO 线程回调内调用须走 `detail::JoinThreadSafely` |
| `IsStateless` | `virtual bool IsStateless() const` | `false` | 会话语义判定入口 |
| `Start` | `virtual void Start()` | 空实现 | 连接后显式启动读线程 |

### `TransportState` 与 `TransportBase`

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `TransportState` | `enum class TransportState { Initial, Connected, Disconnected }` | `Initial` | 三态状态机，底层用 `std::atomic<int>` |
| `TransportBase` | `class TransportBase : public ITransport, public std::enable_shared_from_this<TransportBase>` | — | 所有会话侧传输的公共基类，持有 `channel_`/`session_id_` |
| `SetConnected` | `void SetConnected()` | — | 仅 `Initial → Connected` 生效（CAS），否则记 Warning 日志 |
| `SetDisconnected` | `void SetDisconnected()` | — | 重复调用只触发一次 `on_close_` |
| `GetState` | `TransportState GetState() const` | `Initial` | 原子读取当前状态 |
| `SetOnClose` / `SetOnError` | `void SetOnClose(std::function<void()>)` / `void SetOnError(std::function<void(std::string_view)>)` | 未设置 | 关闭与出错回调，加锁写入；读循环失败经 `SetOnError` 上报 |

### `IClientTransport`

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `Name` | `virtual std::string_view Name() const = 0` | — | 传输名；各实现名为空时回退内置默认名 |
| `Connect` | `virtual std::shared_ptr<ITransport> Connect() = 0` | — | 连接工厂，返回会话对象（不是 `unique_ptr`） |

上面三节的接口全部声明在 `include/mcp/Transport.hpp`。

### `MessageChannel`

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| 构造 | `explicit MessageChannel(size_t max_buffer = 64)` | `64` | 传输基类实际用 `detail::kChannelCapacity` |
| `AsyncReceive` | `template <typename Callback> void AsyncReceive(Callback&& cb)` | — | 阻塞取消息，关闭后回 `operation_canceled` |
| `Send` | `bool Send(JsonRpcMessage message)` | — | 队列满则阻塞等待 |
| `TrySend` | `bool TrySend(JsonRpcMessage message)` | — | 满或关闭立即返回 `false` |
| `Close` / `IsOpen` / `Empty` | `void Close()` / `bool IsOpen() const` / `bool Empty() const` | 打开 | 关闭唤醒两侧条件变量 |

声明在 `include/mcp/protocol/MessageChannel.hpp`。

## stdio

### `StdioServerTransport`

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| 构造 | `StdioServerTransport()` / `explicit StdioServerTransport(std::optional<std::size_t> max_frame_bytes)` | `detail::kMaxStdioFrameBytes`（8 MiB） | 进程自身 stdin/stdout 即管道，无 Options 结构体 |
| `Start` / `Close` / `SendMessageAsync` | `override` | — | `Start()` 拉起读线程；`SendMessageAsync` 写单行 JSON |

### `StdioClientTransport`

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| 构造 | `explicit StdioClientTransport(const StdioClientTransportOptions& options)` | — | 只存配置，不启动子进程 |
| `Name` | `std::string_view Name() const override` | `"stdio"` | `options.name` 为空时回退 |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | 拉起子进程并返回会话传输；此处才校验选项互斥 |
| `Pid` | `std::optional<int64_t> Pid() const` | `std::nullopt` | 仅 `Connect()` 成功后有值 |

### `StdioClientTransportOptions`

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `command` | `std::string` | 空（必填） | 子进程可执行文件 |
| `arguments` | `std::vector<std::string>` | `{}` | 命令行参数 |
| `name` | `std::string` | `""` | 空时 `Name()` 回退 `"stdio"` |
| `working_directory` | `std::string` | `""` | 子进程工作目录 |
| `inherit_environment_variables` | `bool` | `true` | 是否继承父进程环境 |
| `environment_variables` | `std::map<std::string, std::string>` | `{}` | 追加环境变量 |
| `stderr_path` | `std::string` | `""` | 子进程 stderr 落盘路径 |
| `on_stderr` | `std::function<void(std::string_view)>` | 未设置 | 捕获 stderr 到回调（独立读线程） |
| `shutdown_timeout` | `std::chrono::milliseconds` | `{2000}` | 关闭时 SIGTERM→SIGKILL 的宽限窗口，传给 `ProcessHandle::Terminate` |
| `terminate_process_tree` | `bool` | `false` | 连带终止整个进程树（POSIX 进程组 / Windows 作业对象） |
| `max_frame_bytes` | `std::optional<std::size_t>` | `detail::kMaxStdioFrameBytes`（8 MiB） | 单帧上限，超限经 `SetOnError` 报 `stdio frame size exceeds maximum allowed size` |

::: warning
`stderr_path` 与 `on_stderr` 互斥：两者同时设置时 `Connect()` 抛 `McpError(McpErrorCode::InvalidParams, "stderr_path and on_stderr are mutually exclusive")`。
:::

接口声明在 `include/mcp/transport/StdioServerTransport.hpp` 与 `include/mcp/transport/StdioClientTransport.hpp`；子进程与帧读取的行为在 `src/transport/StdioClientTransport.cpp`，进程树终止在 `src/transport/detail/posix_platform.cpp` 与 `src/transport/detail/win32_platform.cpp`。

## Streamable HTTP 服务端

### `StreamableHttpServerTransport`

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| 构造 | `explicit StreamableHttpServerTransport(StreamableHttpServerOptions options = {})` | 默认构造选项 | 构造即建 `HttpServer` 并注册路由 |
| `Start` / `Close` / `SendMessageAsync` | `override` | — | 启停底层 HTTP 监听 |
| `BufferRequestStreamNotification` | `bool BufferRequestStreamNotification(JsonRpcMessage) override` | — | 缓冲进持开的请求流 |
| `RequiresRequestScopedNotifications` | `bool ... const override` | `true` | 恒为真（硬编码） |
| `IsStateless` | `bool IsStateless() const override` | 取 `options_.stateless` | 无状态模式的判定源 |
| `ValidateMcpHeaders` | `static bool ValidateMcpHeaders(const std::string&, const std::string&, const JsonValue&, std::string& error_out)` | — | SEP-2243 头与请求体一致性校验 |
| `AuthResult` | `struct { bool ok{false}; std::vector<std::string> scopes; std::optional<std::string> resource; }` | `ok=false` | `BearerAuthConfig::verify` 的返回载体 |

### `StreamableHttpServerOptions`

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `port` | `uint16_t` | `kDefaultPort` = `3001` | 监听端口 |
| `endpoint` | `std::string` | `"/mcp"` | MCP 请求路径 |
| `stateless` | `bool` | `true` | 无状态模式（2026-07-28 起） |
| `enable_legacy_sse` | `bool` | `false` | 关闭时不注册 GET，监听流与事件回放不可用 |
| `listen_hold_open` | `bool` | `false` | 现代版本下让 `subscriptions/listen` 持开 POST 响应流，改由 POST 流承载监听 |
| `sse_keep_alive_ms` | `int` | `15000` | SSE 注释帧间隔，`0` 禁用 |
| `event_store` | `std::shared_ptr<EventStore>` | `nullptr` | 空时用内置进程内 `EventStore` |
| `session_store` | `std::shared_ptr<SessionStore>` | `nullptr` | 仅会话模式生效；可注入 `InMemorySessionStore` 等实现跨实例接管 |
| `server_name` | `std::string` | `"mcp-server"` | 发现信息用名 |
| `server_version` | `std::string` | `kSdkVersion` | 发现信息用版本 |
| `host` | `std::string` | `""` | 绑定地址；空 = IPv4 `INADDR_ANY`，接受 IPv4/IPv6 字面量，非法值抛异常 |
| `allowed_hosts` | `std::vector<std::string>` | `{}` | Host 白名单；空时仅放行 localhost/回环，缺 `Host` 头直接拒绝 |
| `allowed_origins` | `std::vector<std::string>` | `{}` | Origin 白名单；无 `Origin` 头时放行 |
| `max_request_body_bytes` | `std::optional<std::size_t>` | `detail::kMaxHttpBodyBytes`（4 MiB） | 请求体上限 |
| `max_sse_event_bytes` | `std::optional<std::size_t>` | `detail::kMaxSseEventBytes`（1 MiB） | 单个 SSE 事件上限 |
| `bearer_auth` | `std::optional<BearerAuthConfig>` | `std::nullopt` | 未设置时无鉴权门，行为不变 |
| `resolve_param_annotations` | `std::function<std::vector<McpParamAnnotationInfo>(const std::string& method, const std::string& name)>` | 未设置 | 按方法/工具名解析 `Mcp-Param-*` 头注解；未设置时跳过参数头校验 |
| `enable_server_card` | `bool` | `false` | 注册 `GET /server-card` 发现端点 |
| `server_card` | `std::optional<ext::ServerCard>` | `std::nullopt` | 未提供时由 `BuildDefaultServerCard()` 生成 |

### `StreamableHttpServerOptions::BearerAuthConfig`

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `verify` | `std::function<AuthResult(const std::string& token)>` | 未设置（必填） | 令牌校验；`ok=false` → 401 `invalid_token` |
| `resource_metadata_url` | `std::string` | `""` | 受保护资源元数据 URL，同时写入 `WWW-Authenticate` |
| `scopes_supported` | `std::vector<std::string>` | `{}` | 元数据声明的作用域集合 |
| `required_scopes` | `std::vector<std::string>` | `{}` | 空则不校验作用域；不足 → 403 `insufficient_scope` |
| `authorization_servers` | `std::vector<std::string>` | `{}` | 写入元数据文档 |
| `serve_metadata_endpoint` | `bool` | `true` | 注册免鉴权的 `GET /.well-known/oauth-protected-resource` |
| `expected_resource` | `std::string` | `""` | 期望 audience；空则不校验 |
| `validate_token_resource` | `bool` | `true` | audience 校验开关 |
| `enable_dpop` | `bool` | `false` | DPoP 证明校验（实验特性，需 OpenSSL） |

接口与 Options 声明在 `include/mcp/transport/StreamableHttpServerTransport.hpp`；路由、头校验与 server-card 端点在 `src/http/StreamableHttpServerTransport.cpp`，Host/Origin 与绑定行为在 `src/http/HttpServerImpl.cpp`。

## Streamable HTTP 客户端

### `StreamableHttpClientTransport`

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| 构造 | `explicit StreamableHttpClientTransport(const HttpClientTransportOptions& options)` | — | 只存配置 |
| `Name` | `std::string_view Name() const override` | `"streamable-http"` | `name` 为空时回退 |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | 返回会话传输 |

### `HttpClientTransportOptions`

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `endpoint` | `std::string` | 空（必填） | 服务端 URL；`https` 走 TLS |
| `transport_mode` | `HttpTransportMode` | `AutoDetect` | 声明字段：`src/` 内无任何读取点，实际探测由 `McpClient` 版本协商完成 |
| `name` | `std::string` | `""` | 空回退 `"streamable-http"` |
| `known_session_id` | `std::string` | `""` | 恢复既有会话 |
| `additional_headers` | `std::map<std::string, std::string>` | `{}` | 静态附加头 |
| `auth_challenge_handler` | `std::function<std::string(std::string_view www_authenticate)>` | 未设置 | 401 必调、含 `insufficient_scope` 的 403 才调；返回非空则恰好重试一次 |
| `apply_per_request_headers` | `std::function<void(const HttpRequestHeaderContext&, std::map<std::string, std::string>&)>` | 未设置 | 每次发送前调用，写进可写 `headers` |
| `observe_response_challenge` | `std::function<void(const HttpResponseChallengeContext&)>` | 未设置 | 每个响应（含 2xx 与 SSE）回调一次 |
| `enable_listen_stream` | `std::optional<bool>` | 未设置 → `!stateless` | GET 监听流开关 |
| `stateless` | `bool` | `false` | 纯无状态对端：不发 `Mcp-Session-Id`/`Last-Event-ID`，不发 DELETE |
| `max_sse_event_bytes` | `std::optional<std::size_t>` | `detail::kMaxSseEventBytes`（1 MiB） | 单事件上限 |
| `redirect_policy` | `detail::HttpRedirectPolicy` | `SameOriginOnly` | 重定向策略（枚举在 `mcp::detail` 命名空间） |
| `max_redirects` | `int` | `detail::kMaxHttpRedirects` = `5` | 最大跳数 |

### 请求用途与回调上下文

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `HttpRequestPurpose` | `enum class { MessagePost, ListenStream, ResumeStream, SessionDelete, ToolAnnotationsRefresh }` | `MessagePost` | 区分五类出站请求 |
| `HttpRequestHeaderContext` | `struct { std::string_view http_method; std::string_view url; HttpRequestPurpose purpose; std::optional<RequestId> request_id; int attempt_index; }` | `attempt_index = 0` | 逐请求追加头时的上下文 |
| `HttpResponseChallengeContext` | `struct { int status_code; std::string_view www_authenticate; std::string_view dpop_nonce; }` | `status_code = 0` | 挑战观察回调入参 |

### `HttpTransportMode`

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `AutoDetect` / `StreamableHttp` / `Sse` | `enum class HttpTransportMode` | `AutoDetect` | 声明于 `include/mcp/transport/HttpTransportMode.hpp`，为 `transport_mode` 的取值类型 |

::: info
`transport_mode` 与 `HttpTransportMode` 目前只出现在公共头文件与 `tests/unit/StreamableHttpTransportTests.cpp` 里，生产代码路径未消费；不要据此判断连接行为。
:::

接口与 Options 声明在 `include/mcp/transport/StreamableHttpClientTransport.hpp`；POST、监听流与回放行为在 `src/http/StreamableHttpClientTransport.cpp`，重定向策略常量在 `include/mcp/transport/detail/Limits.hpp`。

## SSE 与 WebSocket 客户端

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `SseClientTransport` | `class MCP_API SseClientTransport : public IClientTransport` | — | 传统 SSE 客户端传输 |
| 构造 | `explicit SseClientTransport(std::string_view server_url, std::string_view name = {})` / `explicit SseClientTransport(SseClientTransportOptions options)` | `name` 空 → `"sse"` | 两个重载都把空 `name` 归一为 `"sse"` |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | 建立 SSE 长连接 |
| `SseClientTransportOptions::server_url` | `std::string` | 空（必填） | SSE 端点 |
| `SseClientTransportOptions::name` | `std::string` | `"sse"` | 唯一显式带默认值的名字字段 |
| `max_sse_event_bytes` | `std::optional<std::size_t>` | `detail::kMaxSseEventBytes` | 单事件上限 |
| `redirect_policy` | `detail::HttpRedirectPolicy` | `SameOriginOnly` | 重定向策略 |
| `max_redirects` | `int` | `5` | 最大跳数 |
| `WebSocketClientTransport` | `class WebSocketClientTransport : public IClientTransport` | — | WebSocket 客户端传输；**没有 Options 结构体**，只有两个构造参数 |
| 构造 | `WebSocketClientTransport(std::string url, std::string name = "websocket")` | `name = "websocket"` | `Name()` 原样返回构造名，无回退逻辑 |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | 只接受 `ws`/`wss`，`wss` 走 TLS，连接超时 30s |

接口声明在 `include/mcp/transport/SseClientTransport.hpp` 与 `include/mcp/transport/WebSocketClientTransport.hpp`；实现分别在 `src/transport/SseClientTransport.cpp` 与 `src/transport/detail/net/WebSocketClient.cpp`。

## 进程内传输

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `InMemoryTransport` | `class MCP_API InMemoryTransport` | — | 类体只有嵌套 `Pair` 与静态 `CreatePair()`，无任何实例成员：把它当命名空间用 |
| `InMemoryTransport::Pair` | `struct { std::shared_ptr<ITransport> client; std::shared_ptr<ITransport> server; }` | — | **两条端点句柄的数据载体**，本身不建通道 |
| `CreatePair` | `static Pair CreatePair()` | — | **工厂函数**：交叉连接两条 `MessageChannel` 并返回已成对的 `Pair`；实现体 `InMemoryTransportImpl` 在源文件匿名 namespace 内，非公共 API |

::: tip
区别一句话：`Pair` 只是返回类型（不是工厂、也不能自建通道），唯一入口是静态 `InMemoryTransport::CreatePair()`。
:::

接口声明在 `include/mcp/transport/InMemoryTransport.hpp`，通道交叉连接在 `src/transport/InMemoryTransport.cpp`。

## 请求取消

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `IRequestCanceller` | `class MCP_API IRequestCanceller` | — | 独立接口，不改 `ITransport` 虚表 |
| `CancelRequest` | `virtual void CancelRequest(const RequestId& id, std::string_view reason) = 0` | — | 现代 Streamable HTTP 用它关闭请求作用域流 |

声明在 `include/mcp/transport/IRequestCanceller.hpp`。

## HTTP 服务端底座

`HttpServer`/`EventStore`/`SessionStore` 是 Streamable HTTP 服务端的底座，也可独立使用；`FileEventStore`、`FileTaskStore`、`Process` 见 [进程与存储参考](/zh/reference/process-and-storage)。

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `HttpRequest` | `struct { std::string method; std::string path; std::unordered_map<std::string, std::string> headers; std::string body; }` | — | 入站请求视图 |
| `HttpResponse` | `struct`，含 `status_code`/`status_text`/`headers`/`body`/`is_sse`/`sse_close_after_write`/`sse_client_tag` | `200`、`"OK"`、其余空/`false` | 后两字段控制 SSE 定向与写完即断 |
| `HttpHandler` | `std::function<void(const HttpRequest&, HttpResponse&)>` | — | 路由处理体 |
| `HttpServerOptions` | `struct`，含 `on_request`/`on_connect`/`on_disconnect`/`allowed_hosts`/`allowed_origins`/`sse_keep_alive_ms`/`bind_host`/`max_request_body_bytes` | `0`、`""`、`kMaxHttpBodyBytes` | 传输层把 `host`/`allowed_*` 原样转发到此 |
| `HttpServer` | `HttpServer(uint16_t port, const HttpServerOptions& options = {})`，配 `void Start()` / `void Stop()` | 选项默认构造 | 非 `ITransport`，独立生命周期 |
| `SetHandler` | `void SetHandler(std::string_view method, std::string_view path, HttpHandler handler)` | — | `(method, path)` 精确匹配 |
| `AddSseClient` / `RemoveSseClient` / `SseClientCount` | `SseClientId AddSseClient(send_fn[, std::string tag])` / `void RemoveSseClient(SseClientId)` / `std::size_t SseClientCount() const` | 无 tag | `SseClientId = uint64_t`；带 tag 版本供定向广播与过滤 |
| `BroadcastSse` | `void BroadcastSse(std::string_view event[, const std::function<bool(const std::string&)>& accept])` | 全发 | 谓词版按 tag 过滤 |

接口声明在 `include/mcp/http/HttpServer.hpp`，转发层在 `src/http/HttpServer.cpp`，实际调度在 `src/http/HttpServerImpl.cpp`。

### `EventStore`

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `Append` | `virtual uint64_t Append(std::string_view session_id, std::string event_data)` | — | 返回单调递增事件 id（起始 `1`） |
| `GetEventsSince` | `virtual std::vector<std::pair<uint64_t, std::string>> GetEventsSince(std::string_view, uint64_t) const` | — | 回放 `Last-Event-ID` 之后的事件 |
| `Clear` | `virtual void Clear(std::string_view session_id)` | — | 会话结束回收 |
| `kMaxEventsPerSession` | `static constexpr size_t` | `10000` | 每会话环形上限 |

声明在 `include/mcp/http/EventStore.hpp`。

### `SessionStore` 与 `InMemorySessionStore`

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `SessionRecord` | `struct { std::string protocol_version; int64_t created_at_ms{0}; int64_t last_seen_ms{0}; }` | `0` | 跨进程接管的最小载荷 |
| `Save` / `Load` / `Remove` | 纯虚 `void` / `std::optional<SessionRecord>` / `void` | — | 三个必备接口 |
| `TrySave` | `virtual bool TrySave(const std::string&, const SessionRecord&)` | 转 `Save` 返 `true` | 满容时的非抛出路径 |
| `Size` / `SweepExpired` | `virtual size_t() const` / `virtual size_t()` | `0` | 可选实现，基类返回 `0` |
| `InMemorySessionStore` | `class InMemorySessionStore : public SessionStore` | — | 进程内实现；`Save` 满时抛 `std::runtime_error("session store is full")` |
| 过期规则 | `now - max(last_seen_ms, created_at_ms) > detail::kSessionIdleTimeoutMs` | 30 分钟 | `Load` 命中过期即删并返回 `nullopt` |

声明在 `include/mcp/http/SessionStore.hpp`，闲置超时与容量常量在 `include/mcp/transport/detail/Limits.hpp`。

## 限额与重定向策略

`include/mcp/transport/detail/Limits.hpp` 位于 `mcp::detail` 命名空间：被公共 Options 直接引用，但不属于稳定 API，改值前先对照本表默认值出处。

| 符号 | 类型 | 值 | 说明 |
|---|---|---|---|
| `kMaxHttpBodyBytes` | `size_t` | 4 MiB | HTTP 请求体默认上限 |
| `kMaxSseEventBytes` | `size_t` | 1 MiB | SSE 单事件默认上限 |
| `kMaxStdioFrameBytes` | `size_t` | 8 MiB | stdio 单帧默认上限 |
| `kMaxHttpRedirects` | `int` | `5` | 重定向跳数默认值 |
| `kChannelCapacity` | `size_t` | `64` | `MessageChannel` 深度 |
| `kMaxHttpSessions` | `size_t` | `10000` | 会话存储上限（满 → 503 / `TrySave` 失败） |
| `kSessionIdleTimeoutMs` | `int64_t` | `1800000` | 会话闲置过期 |
| `HttpRedirectPolicy` | `enum class { SameOriginOnly, FollowAll, NoFollow }` | `SameOriginOnly` | 必须写全 `mcp::detail::HttpRedirectPolicy` |

## TLS 与 OpenSSL（可选）

| 符号 | 形式 | 默认值 | 说明 |
|---|---|---|---|
| `MCP_WITH_OPENSSL` | CMake 缓存项 `AUTO\|ON\|OFF` | `AUTO` | `AUTO` 按 `find_package` 结果；`ON` 找不到即 configure 失败；`OFF` 强制禁用 |
| `MCP_HAVE_OPENSSL` | 编译宏 | 未定义 = 无 TLS | 由 INTERFACE 目标 `mcp-openssl` 统一施加到 `mcp-transport`/`mcp-client`/`mcp-ext` |
| `MCP_OPENSSL_EXPECTED_MAJOR` | CMake 缓存项 | `""` | CI 锁定主版本用；支持 3.x/4.x，其他主版本 configure 失败 |
| `https`/`wss` 连接 | 运行期行为 | — | 未启用 OpenSSL 时抛 `McpError(TlsHandshakeFailed, "TLS support not compiled in")` |
| PKCE 随机源 | 编译期回退 | — | 有 OpenSSL 用 `RAND_bytes`，否则 Windows `BCryptGenRandom`、其他平台 `random_device`；SHA-256 恒为内置实现 |
| TLS 参数 | 内部实现，非公共 API | TLS 1.2 下限 | 系统信任链校验；`TlsSocket`/`TcpSocket`/`HttpClient`/`WebSocketClient` 均在 `src/transport/detail/net/`，不可作为用户 API 引用 |

配置项与探测在 `cmake/FetchDependencies.cmake`、`cmake/OpenSslVersion.cmake` 与 `CMakeLists.txt`；运行期 TLS 握手在 `src/transport/detail/net/TlsSocket.cpp`，PKCE 随机源回退在 `src/client/auth/OAuthClientProvider.cpp`。

## stateful 与 stateless

| 场景 | `IsStateless()` | 会话与通知 |
|---|---|---|
| stdio / InMemory / SSE / WebSocket | `false`（`TransportBase` 恒 `false`） | 单会话长连接，通知走同一通道 |
| Streamable HTTP 服务端（默认 `stateless = true`） | `true` | 每请求独立、同步 JSON 响应（截止 `kStatelessTimeout` = 30s），零监听者时跳过等待 |
| Streamable HTTP 会话模式（`stateless = false`） | `false` | 发 `Mcp-Session-Id`、支持 GET 监听流与 `EventStore` 回放 |

判定入口在 `include/mcp/Transport.hpp` 与 `include/mcp/transport/StreamableHttpServerTransport.hpp`，无状态分支的实现在 `src/http/StreamableHttpServerTransport.cpp`。

::: tip
概念叙述、模式选择与协商细节见 [传输层概念](/zh/learn/transports)；版本协商纠正重试规则见 [版本协商](/zh/learn/protocol-versions)。
:::

## 本页对应源码

- `include/mcp/Transport.hpp`、`include/mcp/protocol/MessageChannel.hpp`、`include/mcp/transport/IRequestCanceller.hpp`
- `include/mcp/transport/StdioServerTransport.hpp`、`include/mcp/transport/StdioClientTransport.hpp`
- `include/mcp/transport/StreamableHttpServerTransport.hpp`、`include/mcp/transport/StreamableHttpClientTransport.hpp`、`include/mcp/transport/HttpTransportMode.hpp`
- `include/mcp/transport/SseClientTransport.hpp`、`include/mcp/transport/WebSocketClientTransport.hpp`、`include/mcp/transport/InMemoryTransport.hpp`
- `include/mcp/http/HttpServer.hpp`、`include/mcp/http/EventStore.hpp`、`include/mcp/http/SessionStore.hpp`、`include/mcp/transport/detail/Limits.hpp`（末者被公共 Options 引用，非稳定 API）
- 行为佐证：`src/transport/StdioClientTransport.cpp`、`src/http/StreamableHttpServerTransport.cpp`、`src/http/StreamableHttpClientTransport.cpp`、`src/http/HttpServerImpl.cpp`、`src/transport/detail/net/TlsSocket.cpp`、`cmake/FetchDependencies.cmake`、`cmake/OpenSslVersion.cmake`、`CMakeLists.txt`；相关任务写法：[/zh/howto/run-a-streamable-http-server](/zh/howto/run-a-streamable-http-server)、[/zh/howto/connect-to-a-server](/zh/howto/connect-to-a-server)、[/zh/howto/manage-subprocesses](/zh/howto/manage-subprocesses)
