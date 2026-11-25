# 传输层参考

本页是传输层与 HTTP 服务端底座公共符号的**唯一权威字段表**，字段、类型、默认值逐条对照头文件；概念叙述与选型理由见 [传输层概念](/zh/learn/transports)，服务端/客户端门面签名见 [服务端参考](/zh/reference/server) 与 [客户端参考](/zh/reference/client)，持久化与进程见 [进程与存储参考](/zh/reference/process-and-storage)。本页不含操作步骤。0.3.13 的 HTTP 绑定、边界、AutoDetect 与会话收紧见 [0.3.13 发布说明](/zh/migration/0.3.13)。

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
| `TransportState` | `enum class TransportState { Initial, Connected, Disconnected, Closing }` | `Initial` | 四态状态机（新增 `Closing` 追加在末尾，旧枚举值不变），底层用 `std::atomic<int>` |
| `TransportBase` | `class TransportBase : public ITransport, public std::enable_shared_from_this<TransportBase>` | — | 所有会话侧传输的公共基类，持有 `channel_`/`session_id_`，并持有 `detail::SessionLifecycle` 与 generation |
| `SetConnected` | `void SetConnected()` | — | 仅 `Initial → Connected` 生效（CAS），否则记 Warning 日志；成功后完成生命周期 `Connecting → Open` |
| `SetDisconnected` | `void SetDisconnected()` | — | 重复调用只触发一次 `on_close_`；`Closing` 期间由关闭 owner 在 join 后统一触发 |
| `GetState` | `TransportState GetState() const` | `Initial` | 原子读取当前状态 |
| `SetOnClose` / `SetOnError` | `void SetOnClose(std::function<void()>)` / `void SetOnError(std::function<void(std::string_view)>)` | 未设置 | 关闭与出错回调，加锁写入；读循环失败经 `SetOnError` 上报 |

### 关闭顺序与 generation（0.3.13）

所有客户端会话传输与流式 HTTP 服务端统一使用 `detail::SessionLifecycle`（`include/mcp/detail/SessionLifecycle.hpp`）管理 `Initial → Connecting → Open → Closing → Closed` 与 generation。一次关闭严格按：进入 `Closing` 停接收 → 关 `MessageChannel` 唤醒等待者 → 经既有失败路径结算 pending → 关 socket/pipe 并中断在途请求 → `detail::JoinThreadSafely` join（回调线程内关闭自动 detach，不 self-join）→ 一次性 `on_close_` 通知。

回调捕获创建时 generation；`Connect` 与 `Close` 按 generation 串行，旧 generation 的消息、错误与关闭回调在新 generation 建立后丢弃。

::: warning 旧行为 / 新行为 / 如何改（源码兼容，行为收紧）
- 旧行为：同一客户端工厂重复 `Connect()` 返回互不干扰的多个会话，旧会话可继续收发。
- 新行为：重复 `Connect()` 会先关闭并等待上一 generation 完成，再开启新 generation；旧会话回调不再投递消息。
- 受影响方：需要并存多个会话时，为每个会话单独创建传输工厂实例；单会话重连无需改动。
:::

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

::: warning
POSIX 环境与 PATH 行为收紧（源码兼容）：旧行为是覆盖项直接追加到继承环境之后（可能重复），可执行文件搜索读父进程的 `getenv("PATH")`，目录也会因可执行位被误当可执行文件。新行为是继承环境按 key 去重、覆盖项按 key 最后生效；`inherit_environment_variables=false` 完全不读父环境；PATH 取自最终有效环境，空段表示当前目录，目录与不可执行文件被拒绝。无自定义环境仍走系统 `execvp` 语义，`argv[0]` 与显式工作目录不变。受影响的下游若依赖重复键或父环境 PATH，请在 `environment_variables` 中显式设置 `PATH`，并用 `inherit_environment_variables=true` 保留需要的继承项。
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
| `AuthResult` | `struct { bool ok{false}; std::string principal; std::vector<std::string> scopes; std::optional<std::string> resource; }` | `ok=false`、`principal` 空 | `BearerAuthConfig::verify` 的返回载体；`principal` 是服务端唯一可信身份来源，不从请求头以外的参数读取 |

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
| `host` | `std::string` | `""` | 绑定地址；空值转发给 `bind_host`，0.3.13 起等价于 IPv4 loopback `127.0.0.1`（旧行为是 `INADDR_ANY`），显式 `0.0.0.0`/`::` 仍可全网监听；接受 IPv4/IPv6 字面量，非法值抛异常 |
| `allowed_hosts` | `std::vector<std::string>` | `{}` | Host 白名单；空时仅放行 localhost/回环，缺 `Host` 头直接拒绝 |
| `allowed_origins` | `std::vector<std::string>` | `{}` | Origin 白名单；无 `Origin` 头时放行 |
| `max_request_body_bytes` | `std::optional<std::size_t>` | `detail::kMaxHttpBodyBytes`（4 MiB） | 请求体上限 |
| `max_sse_event_bytes` | `std::optional<std::size_t>` | `detail::kMaxSseEventBytes`（1 MiB） | 单个 SSE 事件上限 |
| `max_sse_replay_bytes` | `std::optional<std::size_t>` | `detail::kMaxSseEventBytes`（1 MiB） | 会话模式 GET 流按 `Last-Event-ID` 回放时的字节预算，从最新事件向前截取 |
| `request_timeout` | `std::optional<std::chrono::milliseconds>` | `nullopt` → 30s（`kDefaultHttpRequestTimeout`） | POST 等待处理器响应的截止；`0` 无限期等待，超时回 504 |
| `bearer_auth` | `std::optional<BearerAuthConfig>` | `std::nullopt` | 未设置时无鉴权门，行为不变 |
| `on_authenticated` | `ServerAuthenticatedCallback`（`void(const ServerRequestContext&)`） | 未设置 | 认证与会话解析成功后回调；会话模式下若同 ID 绑定不同 principal 则拒绝（403），回调不触发。用于把 principal 传给 `McpServer::SetSessionPrincipal` |
| `resolve_param_annotations` | `std::function<std::vector<McpParamAnnotationInfo>(const std::string& method, const std::string& name)>` | 未设置 | 按方法/工具名解析 `Mcp-Param-*` 头注解；未设置时跳过参数头校验 |
| `enable_server_card` | `bool` | `false` | 注册 `GET /server-card`，并另注册 `GET <endpoint>/server-card` 以匹配客户端发现路径 |
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

卡片响应为 `application/json` 与规范化 body 的强 ETag；`If-None-Match` 支持弱比较、列表与 `*`，命中回 304 空 body 并保留 ETag、`cache-control` 与 CORS 头。CORS 不再由卡片处理器重复写 `Access-Control-Allow-Origin`，统一由 `HttpServerImpl` 的 Host/Origin 准入、OPTIONS 预检与 `Access-Control-Expose-Headers`（含 `ETag`）提供；不新增凭据型 CORS。

接口与 Options 声明在 `include/mcp/transport/StreamableHttpServerTransport.hpp`；路由、头校验与 server-card 端点在 `src/http/StreamableHttpServerTransport.cpp`，Host/Origin 与绑定行为在 `src/http/HttpServerImpl.cpp`。

启用 `bearer_auth` 时，POST/GET 先完成令牌校验，再由 `ServerRequestContext` 统一携带会话状态（`sessionful`/`stateless`）、principal 与授权作用域；同一会话绑定第二个 principal 返回 403，缺失/过期/撤销会话返回 404。pending 登记以「会话 + owner + 请求 id」为原子键，取消、超时、完成与并发同 ID 均按该键移除，避免串线。DELETE 在查询 store、判断会话存在与执行删除之前完成鉴权与 owner 校验；无凭证/错凭证返回 401，跨 owner 与未知会话返回同一 404 `Session expired` 响应，不泄露存在性。`MCP_LOG_LEVEL>=3` 时生命周期日志只写脱敏字段 `correlation=cid-<hex>` 与 `session=sid-<hex>`（stateless 为 `session=stateless`），不写令牌、授权头或正文。未配置 `bearer_auth` 的传输保持旧行为。

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
| `transport_mode` | `HttpTransportMode` | `AutoDetect` | `AutoDetect` 首次初始化收到 400/404/405 且 body 非合法 JSON-RPC 错误信封时恰好回退一次 legacy SSE；`StreamableHttp`/`Sse` 直选实现，不回退 |
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
`AutoDetect` 由 `StreamableHttpClientTransport::Connect()` 返回的稳定 facade 消费：facade 持有唯一 `MessageChannel`，内层 Streamable HTTP/SSE 只负责 IO，替换内层不改变调用方持有的 channel。仅首次 `initialize` 响应参与回退判定；`McpClient` 在 `ConnectMode::Pin/Legacy` 下通过 `ITransportFallbackControl::SetFallbackEnabled(false)` 关闭传输回退（协商语义不变）。
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
| `additional_headers` | `std::map<std::string, std::string>` | `{}` | 静态附加头（GET 与 POST） |
| `auth_challenge_handler` | `std::function<std::string(std::string_view)>` | 未设置 | 401 必调、403 含 `insufficient_scope` 才调；返回非空用于下一次请求 |
| `apply_per_request_headers` | `std::function<void(const HttpRequestHeaderContext&, std::map<std::string, std::string>&)>` | 未设置 | GET/POST 每次发送前调用，可覆盖附加头 |
| `observe_response_challenge` | `std::function<void(const HttpResponseChallengeContext&)>` | 未设置 | GET/POST 响应到达后各调用一次 |
| `on_establish_result` | `std::function<void(bool)>` | 未设置 | 内部回退接线用：首个有效 SSE 事件为 `true`，重试预算耗尽仍未建立为 `false`；普通用户无需设置 |
| `WebSocketClientTransport` | `class WebSocketClientTransport : public IClientTransport` | — | WebSocket 客户端传输；**没有 Options 结构体**，只有两个构造参数 |
| 构造 | `WebSocketClientTransport(std::string url, std::string name = "websocket")` | `name = "websocket"` | `Name()` 原样返回构造名，无回退逻辑 |
| `Connect` | `std::shared_ptr<ITransport> Connect() override` | — | 只接受 `ws`/`wss`，`wss` 走 TLS，连接超时 30s |

接口声明在 `include/mcp/transport/SseClientTransport.hpp` 与 `include/mcp/transport/WebSocketClientTransport.hpp`；实现分别在 `src/transport/SseClientTransport.cpp` 与 `src/transport/detail/net/WebSocketClient.cpp`。

::: warning
WebSocket 握手行为收紧（源码兼容，旧 API 签名不变）：旧行为是 `Connect()` 立即把传输标为 Connected，握手完成前发送的消息无界且失败静默。新行为是 101 前保持 Connecting，仅在状态行、`Upgrade`、`Connection`、`Sec-WebSocket-Accept` 全部验证通过后进入 Open 并把传输标为 Connected；101 前消息按入队顺序延迟发送，条数上限 `kMaxWebSocketHandshakeQueueMessages`（64）与序列化字节上限 `kMaxWebSocketHandshakeQueueBytes`（1 MiB），满载或握手失败经 `SetOnError` 明确报错，Closing/Closed 拒绝新消息；失败、超时、关闭进入唯一终止态且只通知一次。总截止时间覆盖 DNS/TCP/TLS/写请求/读响应/重定向，关闭或超时中断阻塞读写并 join。重定向仅允许同主机同端口或 `ws:80 → wss:443` 升级，最多 `kMaxWebSocketRedirects`（5）跳，拒绝降级、跨主机、其他跨端口、带 userinfo 与畸形 Location，每次跳转更换 Sec-WebSocket-Key 且不放宽 TLS 校验。受影响的下游若依赖跨主机/降级重定向或 101 前无界发送，请改为显式配置合法 URL 并尊重有界队列的报错；构造与 `Connect()` 签名不变。
:::

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
| `HttpServerOptions` | `struct`，含 `on_request`/`on_connect`/`on_disconnect`/`allowed_hosts`/`allowed_origins`/`sse_keep_alive_ms`/`bind_host`/`max_request_body_bytes`/`header_read_budget` | `0`、`""`（= `127.0.0.1` loopback；显式 `0.0.0.0`/`::` 仍可全网监听）、`kMaxHttpBodyBytes`、`header_read_budget = nullopt` | 传输层把 `host`/`allowed_*` 原样转发到此 |
| `HttpServer` | `HttpServer(uint16_t port, const HttpServerOptions& options = {})`，配 `void Start()` / `void Stop()` | 选项默认构造 | 非 `ITransport`，独立生命周期 |
| `SetHandler` | `void SetHandler(std::string_view method, std::string_view path, HttpHandler handler)` | — | `(method, path)` 精确匹配 |
| `AddSseClient` / `RemoveSseClient` / `SseClientCount` | `SseClientId AddSseClient(send_fn[, std::string tag])` / `void RemoveSseClient(SseClientId)` / `std::size_t SseClientCount() const` | 无 tag | `SseClientId = uint64_t`；带 tag 版本供定向广播与过滤 |
| `BroadcastSse` | `void BroadcastSse(std::string_view event[, const std::function<bool(const std::string&)>& accept])` | 全发 | 谓词版按 tag 过滤 |

接口声明在 `include/mcp/http/HttpServer.hpp`，转发层在 `src/http/HttpServer.cpp`，实际调度在 `src/http/HttpServerImpl.cpp`。

::: warning HTTP/SSE/202/bind 行为收紧（源码兼容，旧 API 签名不变）
- 旧行为：`bind_host` 为空时绑定 `INADDR_ANY`（全网）；请求与响应的 `Content-Length` 用朴素乘加、重复头以最后值覆盖；SSE 各路径自行按 `\n\n` 分块，只有 CRLF/LF 被识别且跨块末尾 `\r` 处理不一致；HTTP 202 无论请求还是通知都只记日志，请求的 pending 可能悬挂。
- 新行为：`bind_host` 为空只绑定 IPv4 loopback `127.0.0.1`，默认卡片端点仍是 `localhost`；`Content-Length` 先做乘法溢出检查再比 body 上限，空值/非数字/重复冲突/超 `size_t` 确定拒绝，`Transfer-Encoding` 与 `Content-Length` 共存拒绝，chunk size 溢出与累计上限统一；SSE 共用增量解析器，CRLF/LF/单独 CR/混合换行与跨块末尾 `\r` 等价，空行派发、事件上限不变；202 仅确认通知或客户端 JSON-RPC 响应（空 body 直接完成），带 `method` 的请求收到 202 立即投递 `ProtocolViolation (-32005)` 并关闭/排空连接，不自动重放。
- 受影响怎么改：需要全网监听时显式设置 `HttpServerOptions::bind_host = "0.0.0.0"`（IPv6 用 `"::"`）；依赖请求收到 202 后由后续 GET/SSE 补响应的下游应改为让服务端返回正式 JSON-RPC 响应或错误；依赖重复/非法 framing 被宽容的下游应发送合规的单 `Content-Length` 且不同时出现 `Transfer-Encoding`；依赖裸 CR 或跨块尾 `\r` 旧行为的 SSE 服务端应发出标准 CRLF/LF 空行分帧。旧的 `SseClientTransport`/`HttpClientTransportOptions` 构造与 `HttpServerOptions` 字段签名均不变。
:::

::: warning AutoDetect 单次 legacy SSE 回退（源码兼容，旧 API 签名不变）
- 旧行为：`transport_mode` 无运行时读取点，`AutoDetect` 始终走 Streamable HTTP；命中 400/404/405 的非协议 body 时只报错，不尝试 legacy SSE。
- 新行为：`AutoDetect` 首次 `initialize` 响应为 400/404/405 且 body 不是合法 JSON-RPC 错误信封时，先停发并完整关闭旧 Streamable HTTP 会话（POST/listen/resume/DELETE/线程），再用相同 endpoint、附加头、每请求头、challenge 回调、重定向策略与大小上限建立 legacy SSE，并在新会话上恰好重放一次该初始化消息；其他状态、合法错误信封与后续响应不回退。SSE 建立失败时以 `ConnectionClosed` 有界结束 pending、只报一次错误，不循环重试。`StreamableHttp`/`Sse` 显式模式与 `ConnectMode::Pin/Legacy` 不回退。
- 附带 SSE 收紧（同源兼容面）：旧行为是 endpoint 事件到达前发出的消息被静默丢弃，且任意响应 body 分块都会重置重连预算；新行为是消息等待 endpoint 事件后按序发出（避免回退重放丢失），只有有效 `endpoint`/`message` 事件才重置重连预算，非 SSE 错误体不再无限重连。
- 受影响怎么改：不希望自动回退的下游把 `transport_mode` 显式设为 `StreamableHttp` 或 `Sse`，或使用 `ConnectMode::Pin/Legacy`（客户端自动关闭传输回退），也可对返回的传输调用 `ITransportFallbackControl::SetFallbackEnabled(false)`；依赖 400/404/405 文本 body 直接报错的下游需改为预期一次 legacy SSE 初始化重放；依赖 endpoint 前发送消息被丢弃或靠错误体重置重连预算的下游需改为等待 endpoint 事件后再发送、并让服务端返回合法 SSE 帧。`SseClientTransportOptions` 新增字段全部有默认值，旧构造与聚合初始化不变。
:::

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
| `kMaxWebSocketRedirects` | `int` | `5` | WebSocket 重定向跳数上限 |
| `kMaxWebSocketHandshakeQueueMessages` | `size_t` | `64` | 101 前排队条数上限 |
| `kMaxWebSocketHandshakeQueueBytes` | `size_t` | 1 MiB | 101 前排队序列化字节上限 |
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
| Streamable HTTP 服务端（默认 `stateless = true`） | `true` | 每请求独立、同步 JSON 响应（截止 `kDefaultHttpRequestTimeout` = 30s，`request_timeout` 可覆盖），零监听者时跳过等待 |
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
- 行为佐证：`src/transport/StdioClientTransport.cpp`、`src/http/StreamableHttpServerTransport.cpp`、`src/http/StreamableHttpClientTransport.cpp`、`src/http/HttpServerImpl.cpp`、`src/transport/detail/net/TlsSocket.cpp`、`cmake/FetchDependencies.cmake`、`cmake/OpenSslVersion.cmake`、`CMakeLists.txt`

相关任务写法：[/zh/howto/run-a-streamable-http-server](/zh/howto/run-a-streamable-http-server)、[/zh/howto/connect-to-a-server](/zh/howto/connect-to-a-server)、[/zh/howto/manage-subprocesses](/zh/howto/manage-subprocesses)。
