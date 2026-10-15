# 传输层

| 传输层            | 客户端 | 服务器 | 描述                                        |
|------------------|--------|--------|---------------------------------------------|
| Stdio           | 是     | 是     | stdin/stdout 管道，子进程通信                 |
| Streamable HTTP | 是     | 是     | HTTP POST 配合 JSON/会话模式响应             |
| SSE             | 是     | 是¹   | 用于推送通知的服务器发送事件                   |
| WebSocket       | 是     | 否     | 基于 TCP 的双向通信（自研 WebSocketClient）  |
| InMemory        | 是     | 是     | 进程内通信，用于测试                          |

## Streamable HTTP

Streamable HTTP 传输实现了 MCP Streamable HTTP 规范。每个会话使用一个内部的 `StreamableHttpSessionTransport`（封装 `ITransport`），配备独立的消息通道和后台线程。

**请求流程**：客户端通过 HTTP POST 发送 JSON-RPC 消息。在**无状态**模式下，服务端同步处理请求并以 JSON 响应（通过 `std::promise` 关联响应，超时时间 30 秒）。在**会话**模式下，服务端返回 `202 Accepted`，并通过 SSE（通过独立的 GET 端点）异步交付结果。服务端验证 `Mcp-Method` 和 `Mcp-Name` 头是否与消息体匹配（SEP-2243），在响应中回显这些头，并将 `Mcp-Param-*` 头提取到 `_meta.x-mcp-headers`。`tasks/get`、`tasks/update`、`tasks/cancel` 的 `Mcp-Name` 取 `params.taskId`（缺失或与头不等时 400）。非空的 `MCP-Protocol-Version` 不在支持表时返回 400（`-32022`）；响应恒带 `mcp-protocol-version` 头（无值时回默认版本）。

**SSE 读取循环**：在客户端，当 POST 响应包含 SSE 事件时（例如会话模式结果），后台线程读取数据块，按 `\n\n` 分隔符拆分，解析 `data:` 行，并将 `JsonRpcMessage` 入队到 `MessageChannel`。

**请求头**（`StreamableHttpClientTransport`）：
- `MCP-Protocol-Version: 2026-07-28` — 始终发送
- `Mcp-Method` — 动态生成，从 JSON-RPC 消息体的 method 字段提取
- `Accept: application/json, text/event-stream` — 允许服务端选择响应模式

## 传输层接口

```
ITransport（会话连接）
  └── TransportBase（三状态：初始 → 已连接 → 已断开）
      ├── StdioServerTransport
      ├── StdioClientSessionTransport（内部类）
      ├── InMemoryTransportImpl
      ├── SseClientSessionTransport（内部类）
      ├── WebSocketSessionTransport（封装自研 WebSocketClient）
      ├── StreamableHttpServerTransport
      └── StreamableHttpSessionTransport（内部类）

IClientTransport（连接工厂）
  ├── StdioClientTransport（PlatformIO）
  ├── SseClientTransport（自研 HttpClient）
  ├── StreamableHttpClientTransport（自研 HttpClient / WinHTTP）
  └── WebSocketClientTransport（自研 WebSocketClient）
```

## 关键类型

### `HttpTransportMode`
控制 HTTP 客户端传输的连接方式：

| 值              | 描述                                                |
|-----------------|-----------------------------------------------------|
| `AutoDetect`    | 先尝试 Streamable HTTP（`server/discover`），失败回退到 SSE |
| `StreamableHttp`| 直接使用 Streamable HTTP（需要 2026-07-28+）        |
| `Sse`           | 仅使用传统 SSE POST                                 |

### `StdioClientTransportOptions`
| 字段                           | 类型                              | 默认值   | 描述                    |
|-------------------------------|-----------------------------------|----------|-------------------------|
| `command`                     | `std::string`                     | 必填     | 子进程命令              |
| `arguments`                   | `std::vector<std::string>`        | `{}`     | 命令行参数              |
| `name`                        | `std::string`                     | `""`    | 传输层名称（为空时回退到 `"stdio"`） |
| `working_directory`           | `std::string`                     | `""`     | 子进程工作目录          |
| `inherit_environment_variables` | `bool`                          | `true`   | 继承父进程环境          |
| `environment_variables`       | `std::map<std::string, std::string>` | `{}`  | 附加环境变量            |

### `HttpClientTransportOptions`
| 字段                 | 类型                                     | 默认值        | 描述                    |
|---------------------|------------------------------------------|--------------|-------------------------|
| `endpoint`          | `std::string`                            | 必填         | 服务端端点 URL          |
| `transport_mode`    | `HttpTransportMode`                      | `AutoDetect` | 连接模式                |
| `name`              | `std::string`                            | `""`         | 传输层名称              |
| `known_session_id`  | `std::string`                            | `""`         | 用于恢复的会话 ID       |
| `additional_headers`| `std::map<std::string, std::string>`     | `{}`         | 附加 HTTP 头            |
| `auth_challenge_handler` | `function<string(string_view www_authenticate)>` | `nullptr` | 401 必回调、`WWW-Authenticate` 含 `insufficient_scope` 的 403 才回调（RFC 9728，其余 403 按错误体处理）；返回非空 `Authorization` 头则恰好重试一次 |
| `apply_per_request_headers` | `function<void(const HttpRequestHeaderContext&, map<string,string>&)>` | `nullptr` | 每次发请求前调用一次（含重试与内部补发），回调把追加头写进可写 `headers`（覆盖静态 `additional_headers` 与 `Authorization` 之后）；可直接绑定 `OAuthClientProvider::ApplyDPoPProofHeader` 逐请求附加 `DPoP` 证明 |
| `observe_response_challenge` | `function<void(const HttpResponseChallengeContext&)>` | `nullptr` | 每个响应调用一次（含 2xx 与 SSE），入参含 `status_code`/`www_authenticate`/`dpop_nonce`；可直接绑定 `OAuthClientProvider::ObserveDPoPNonce` 回灌 nonce |
| `enable_listen_stream` | `optional<bool>` | 未设置时 `!stateless` | GET SSE 监听流开关；未设置时 stateful 默认开、stateless 默认关 |
| `stateless` | `bool` | `false` | 纯无状态对端专用：不发送 `Mcp-Session-Id` / `Last-Event-ID`，不发送 DELETE |
| `max_sse_event_bytes` | `optional<size_t>` | 1 MB | SSE 单事件上限 |
| `redirect_policy` | `HttpRedirectPolicy` | `SameOriginOnly` | 重定向策略（`FollowAll`/`SameOriginOnly`/`NoFollow`） |
| `max_redirects` | `int` | `5` | 最大重定向次数 |

监听流状态判定：4xx（一律，含 404/405）与「200 但 content-type 非 SSE」视为服务器不支持监听流（`Unsupported`，静默放弃）；408/429 与 2xx 非 200 仍走退避重连。`use_dpop_nonce` 重试（401/400 且挑战或响应体前 4KB 含该标记时恰好一次）与 `auth_challenge_handler` 重试共用同一单次预算。

### `StreamableHttpServerOptions`
| 字段               | 类型                               | 默认值          | 描述                          |
|-------------------|------------------------------------|-----------------|-------------------------------|
| `port`            | `uint16_t`                         | `3001`          | HTTP 服务端口                 |
| `endpoint`        | `std::string`                      | `"/mcp"`        | HTTP 端点路径                 |
| `host`            | `std::string`                      | `""`            | 监听绑定地址；空 = 仅监听 IPv4 所有接口（INADDR_ANY），接受 IPv4/IPv6 字面量 |
| `stateless`       | `bool`                             | `true`          | 启用 2026-07-28 无状态模式    |
| `enable_legacy_sse` | `bool`                           | `false`         | 在 GET 上提供 SSE 流（关闭时不注册 GET，监听流与回放不可用） |
| `sse_keep_alive_ms` | `int`                            | `15000`         | SSE keepalive 注释帧间隔，0 禁用 |
| `event_store`     | `std::shared_ptr<EventStore>`      | `nullptr`       | 用于恢复的事件存储；不设置时使用进程内内存存储，可注入 `FileEventStore` 等自定义实现 |
| `session_store`   | `std::shared_ptr<SessionStore>`    | `nullptr`       | 外部会话存储（仅会话模式）；多实例共享同一存储时可互相接管会话，未知/过期的会话 id 返回 404 与 `-32009`（SessionExpired）；`initialize` 成功时落盘（满存上限 10000 时 503），闲置 30 分钟过期 |
| `bearer_auth`     | `std::optional<BearerAuthConfig>`  | `std::nullopt`  | Bearer 鉴权配置（RFC 6750/9728），见下文 |
| `server_name`     | `std::string`                      | `"mcp-server"`  | 用于发现的服务器名称          |
| `server_version`  | `std::string`                      | `kSdkVersion`   | 用于发现的服务器版本          |

### `BearerAuthConfig`
设置 `bearer_auth` 后，`StreamableHttpServerTransport` 在 POST/GET 处理顶部执行 Bearer 鉴权门（先于任何会话处理）：

| 字段                       | 类型                                     | 描述                                             |
|---------------------------|------------------------------------------|--------------------------------------------------|
| `verify`                  | `function<AuthResult(const std::string&)>` | 必填；`AuthResult{ok, scopes, resource}`（`resource` 为令牌携带的 audience），`ok=false` 拒绝并返回 401 `error="invalid_token"` |
| `resource_metadata_url`   | `std::string`                            | 受保护资源元数据文档的完整 URL，同时被 `WWW-Authenticate` 挑战引用 |
| `scopes_supported`        | `vector<string>`                         | 声明支持的作用域                                  |
| `required_scopes`         | `vector<string>`                         | 令牌作用域须全部覆盖；为空则禁用作用域检查（不足返回 403） |
| `expected_resource`       | `std::string`                            | 期望的 audience；为空则禁用 audience 检查         |
| `validate_token_resource` | `bool`                                   | 是否校验 audience（默认 `true`）                  |
| `authorization_servers`   | `vector<string>`                         | 授权服务器列表，写入元数据文档                    |
| `serve_metadata_endpoint` | `bool`                                   | 注册 `GET /.well-known/oauth-protected-resource`（免鉴权），默认 `true` |
| `enable_dpop` | `bool` | DPoP 证明校验开关（默认 `false`）：先配置 `bearer_auth` 才生效，开启后要求 `DPoP` 方案与证明头，失败返回 401 并携带 `DPoP-Nonce` |

POST 与 GET（SSE）两条路径上的结果一致：缺失或格式错误的 `Authorization: Bearer` 头 → `401` 挑战并引用 `resource_metadata_url`；`verify` 返回 `ok=false` → `401` 携带 `error="invalid_token"`；audience 失配（启用校验且 `expected_resource` 非空）→ `401` 携带 `error="invalid_token"`；令牌未覆盖任一 `required_scopes` → `403` 携带 `error="insufficient_scope"`。未配置 `bearer_auth` 时行为与之前完全一致，零回归。客户端侧对接见 [OAuth 鉴权](/zh/client/oauth)。

### `InMemoryTransport::Pair`
```cpp
struct Pair {
    std::shared_ptr<ITransport> client;
    std::shared_ptr<ITransport> server;
};
```

> ¹ 服务端 SSE 通过 `StreamableHttpServerTransport` 提供，当 `enable_legacy_sse` 为 `true`（默认 `false`）时，在同一端点上通过 HTTP GET 提供 SSE 流。

## 无状态模式

`StreamableHttpServerTransport` 支持无状态模式，由 `StreamableHttpServerOptions::stateless` 控制（默认 `true`，对应 2026-07-28）。当为 `true` 时，`IsStateless()` 返回 `true`，并且：

- **无会话**：每个请求独立；通过 `std::promise` 同步关联响应，超时时间 30 秒。
- **无 SSE**：不进行 SSE 广播或 `EventStore` 追加；仅与待处理请求关联的响应通过 JSON 交付。
- **监听者等待**：服务端在 server-initiated 请求与通知广播前判断有无 GET 监听者——仅当 `!stateless || enable_legacy_sse` 时零监听者等待至多 2s（`stateless` 持开 POST 流模式跳过等待）；持开请求流通知由缓冲裁决先行过滤，不进等待。
