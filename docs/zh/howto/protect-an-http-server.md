# 给 HTTP 服务端加防护

把一个 `StreamableHttpServerTransport` 从「本地能跑」变成「能被公网打」：Bearer 校验、`WWW-Authenticate` 挑战与元数据端点、DPoP、Host/Origin 白名单、体积上限，以及 `Mcp-Param-*` 头与 body 的一致性。协议层会话行为见 [/zh/howto/run-a-streamable-http-server](/zh/howto/run-a-streamable-http-server)，客户端侧的 OAuth 见 [/zh/howto/add-oauth-to-a-client](/zh/howto/add-oauth-to-a-client)。

## 前置条件

- 头文件：`mcp/transport/StreamableHttpServerTransport.hpp`、`mcp/http/SessionStore.hpp`、`mcp/http/EventStore.hpp`、`mcp/storage/FileEventStore.hpp`、`mcp/server/McpServer.hpp`、`mcp/server/ServerOptions.hpp`。
- 链接 `mcp-http` 与 `mcp-server`（依赖关系见 [/zh/start/installation](/zh/start/installation)）。
- HTTPS 只在编译期开启 OpenSSL 时可用：`MCP_WITH_OPENSSL=AUTO` 未找到则**整站只能明文**，此时必须靠前置反向代理做 TLS 终结（`OFF` 会禁用 TLS）。
- 防护配置全部在传输层，与协议时代无关；`bearer_auth` 缺省即**完全不校验令牌**，别把「没配」当成「拒得严」。

## Bearer 校验：verify 是唯一必选项

`StreamableHttpServerOptions::bearer_auth` 是 `std::optional<BearerAuthConfig>`（`include/mcp/transport/StreamableHttpServerTransport.hpp`）。`verify` 收到解析出的令牌串，返回 `AuthResult{ok, scopes, resource}`。其余字段决定挑战内容而不是校验强度：`required_scopes` 用于 403 提示，`scopes_supported`/`authorization_servers`/`resource_metadata_url` 用于元数据端点，`expected_resource` + `validate_token_resource`（默认开）做受众绑定。

<<< @/snippets/protect-an-http-server.cpp#bearer-auth

::: warning verify 必须自己挡住过期与伪造
SDK 不看令牌内容：`ok` 为假就 401，仅此而已。签名验证、过期判断、吊销检查都在你的 `verify` 里——把它当成唯一信任边界来写，并且**不要在里面做同步阻塞的远程调用**（它在请求线程上执行）。
:::

## 挑战帧与元数据端点

未授权时服务端回 `401`，`WWW-Authenticate` 形如 `Bearer resource_metadata="<url>"`，令牌被判无效时追加 `error="invalid_token"`，有 scope 信息时追加 `scope="..."`，body 是 JSON-RPC 错误 `-32000` + `unauthorized`（`src/http/StreamableHttpServerTransport.cpp`）。scope 不足时回 `403` + `Bearer error="insufficient_scope", scope="..."`，body 为 `-32000` + `insufficient_scope`。

`serve_metadata_endpoint = true`（默认）时服务端自己提供受保护资源元数据端点，供客户端做发现；把它关掉就得由你的网关提供同一个 URL，否则合规客户端会卡在发现阶段。

## DPoP：把令牌绑到本次连接

`enable_dpop = true` 后服务端接受 `Authorization: DPoP <token>`（scheme 判定大小写不敏感，`src/http/StreamableHttpServerTransport.cpp`）并核对随请求附带的 DPoP 证明。`authorization_servers` 声明签发该令牌的授权服务器，用于跨服务器场景的信任判定。

<<< @/snippets/protect-an-http-server.cpp#dpop-server

配套的客户端写法（`apply_per_request_headers` + `observe_response_challenge`）见 [/zh/howto/add-oauth-to-a-client](/zh/howto/add-oauth-to-a-client)——**服务端开 DPoP 而客户端不挂这两个钩子，表现是持续 401**，因为 nonce 永远拿不到。

## Host 与 Origin：默认只放行回环

这道检查在通用 HTTP 层，任何路径都受它管（`src/http/HttpServerImpl.cpp`，判定失败即 `403 Forbidden` 并记一条 Warning）：

| 情况 | 行为 |
|---|---|
| 请求没有 `Host` 头 | 直接拒绝 |
| `allowed_hosts` 非空 | 与 `Host` 头**逐字符相等**才算通过 |
| `allowed_hosts` 为空 | 只允许回环类 host（默认 fail-closed） |
| 没有 `Origin` 头 | 放行（非浏览器客户端不受影响） |
| 有 `Origin` 且白名单非空 | 逐字符相等，否则拒绝 |
| 有 `Origin` 且白名单为空 | 必须与 `Host` 同源 |

<<< @/snippets/protect-an-http-server.cpp#host-and-origin

::: danger 相等匹配带端口
`Host` 头通常含端口，所以白名单要写 `localhost:3010` 而不是 `localhost`。写漏端口不会「宽松匹配」，而是**全部 403**——这是最常见的上线首日故障。
:::

## 体积与频率上限

<<< @/snippets/protect-an-http-server.cpp#size-limits

- `max_request_body_bytes` 超出直接 `413`（`src/http/StreamableHttpServerTransport.cpp`），默认取 `kMaxHttpBodyBytes`；显式设小能挡住大 payload 打满内存。
- `max_sse_event_bytes` 限制单条 SSE 事件；`sse_keep_alive_ms`（默认 15000）决定注释帧的心跳间隔，用来穿过激进的中间层空闲超时。

## stateless 与 stateful 的防护差异

<<< @/snippets/protect-an-http-server.cpp#stateless-mode

`stateless` 默认 `true`：不建会话、`session_store` 传 `nullptr`，断线重放靠 `event_store`（`FileEventStore` 落盘可跨进程）。`stateless = false` 时才需要 `session_store`，此时**会话 ID 是能力凭证**——一旦泄漏，对端可以在**空闲窗口内**继续用同一个会话（内置常量 `kSessionIdleTimeoutMs` = 30 分钟无活动即失效，`include/mcp/transport/detail/Limits.hpp`，不可配置），因此必须与 Bearer 同时启用，并在登出或撤销时主动 `Remove` 会话。`listen_hold_open` 让 modern 的 `GET` 流保持打开，进度与日志会写进所属请求流（详见 [/zh/howto/report-progress](/zh/howto/report-progress)）。

## 参数头一致性

`Mcp-Param-*` 头用于把某些参数同时暴露在 header 与 body 里（如网关做路由与审计）。一致性校验在服务端完成，四种不一致各有其拒绝理由（`src/http/StreamableHttpServerTransport.cpp`）：

| 拒绝理由 | 含义 |
|---|---|
| `Mcp-Param-<n> header present but the parameter is absent from arguments` | 头给了、body 没这个参数 |
| `missing Mcp-Param-<n> header for tools/call argument` | body 里有参数但头没带 |
| `Mcp-Param-<n> header value is malformed` | 头值解码失败 |
| `Mcp-Param-<n> header value does not match the request body` | 头值与 body 值不等 |

注解来自你的工具 `input_schema` 里的 `x-mcp-header`，经 `resolve_param_annotations` 回调向服务端解析（`include/mcp/transport/StreamableHttpServerTransport.hpp`）。回调需要 `McpServer` 实例，而传输对象先建——用「先声明空指针、建完服务端再赋值」的写法，注意别在服务端就绪前处理请求：

<<< @/snippets/protect-an-http-server.cpp#param-headers

## 完整代码

Bearer + Host/Origin 白名单 + 体积上限 + 参数头一致性 + 日志能力声明：

<<< @/snippets/protect-an-http-server.cpp#full

## 验证

片段只做语法校验（clang-cl 走 `/Zs`，GCC/Clang 走 `-fsyntax-only`）：

```bash
cmake -S . -B build/p3a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3a --target mcp-doc-snippet-protect-an-http-server
```

期望：configure 打印 `[mcp-docs] syntax-checking 18 snippet(s) with '/Zs'`，构建打印一行 `Building CXX object ...protect-an-http-server.cpp.obj`，返回码 0 且无 `error:` 行。

行为侧最省事的复现是用官方裁判打你的服务端：`CONFORMANCE_SPEC_VERSION=2026-07-28 ./scripts/run-conformance.sh server`（`MCP_BUILD_CONFORMANCE=ON` 起 fixture）。判据是 unexpected failures 为空；基线文件里已登记的条目属「已知未达标」，见 `tests/conformance/`。

## 时代差异

| 项目 | legacy（≤ 2025-11-25） | modern（2026-07-28） |
|---|---|---|
| Bearer / 401 / 403 挑战 | 一致 | 一致 |
| Host/Origin 检查 | 一致（在通用 HTTP 层） | 一致 |
| 413 / SSE 事件上限 | 一致 | 一致 |
| 通知落点 | `enable_legacy_sse` 的独立 SSE 流 | `listen_hold_open` 下按请求流路由，进度与日志同流 |
| 断线恢复 | 靠全局 SSE 流 | 靠 `event_store` + `Last-Event-ID` |

## 常见错误

| 对外表现 | 成因 | 处置 |
|---|---|---|
| 全部请求 403 Forbidden，日志写 `Host not allowed` | `allowed_hosts` 没带端口 | 按客户端实际 `Host` 头逐字符写全 |
| curl 能通、浏览器不通 | `allowed_origins` 非空但没放前端 origin | 加上前端的 scheme://host[:port]，或留空走同源判定 |
| 一直 401 且拿不到 nonce | 服务端开了 DPoP，客户端没挂 `observe_response_challenge` | 两端配套，见 [/zh/howto/add-oauth-to-a-client](/zh/howto/add-oauth-to-a-client) |
| 令牌有效但被拒 | `expected_resource` 与令牌 `resource` 不符 | 对齐受众（RFC 8707），或关掉 `validate_token_resource` 并自行承担风险 |
| 元数据 URL 404 | `serve_metadata_endpoint = false` 又没在网关提供 | 要么开内置，要么让网关提供同一 URL |
| 大 body 打满内存 | 没设 `max_request_body_bytes` | 显式收紧，观察 413 |
| 空闲后 SSE 流被中间层切断 | `sse_keep_alive_ms` 大于中间层空闲超时 | 调小心跳间隔 |
| 头与 body 不一致却通过 | `resolve_param_annotations` 返回空（服务端尚未赋好指针） | 确认回调命中真实 `McpServer` 实例后再 `Start()` |
| 会话 ID 被当密码用 | stateful 下只靠会话 ID 区分调用方 | 会话必须与 Bearer 同用；空闲失效是内置 30 分钟，需要立即失效就自己 `Remove` |

## 下一步

- 服务端会话与流的建立：[/zh/howto/run-a-streamable-http-server](/zh/howto/run-a-streamable-http-server)
- 客户端一侧的令牌获取：[/zh/howto/add-oauth-to-a-client](/zh/howto/add-oauth-to-a-client)
- 传输与端口选型：[/zh/learn/transports](/zh/learn/transports)
- 字段全表：[/zh/reference/transports](/zh/reference/transports)、[/zh/reference/auth-and-extensions](/zh/reference/auth-and-extensions)

## 本页对应源码

- `include/mcp/transport/StreamableHttpServerTransport.hpp`（`StreamableHttpServerOptions`、`BearerAuthConfig`、`AuthResult`、`resolve_param_annotations`）
- `include/mcp/http/SessionStore.hpp`、`include/mcp/http/EventStore.hpp`、`include/mcp/storage/FileEventStore.hpp`
- `src/http/StreamableHttpServerTransport.cpp`（挑战帧、413、参数头一致性、DPoP scheme）
- `src/http/HttpServerImpl.cpp`（Host/Origin 准入与 403）
- `src/http/McpParamHeaders.hpp`（`Mcp-Param-*` 编解码）
- `scripts/run-conformance.sh`、`tests/conformance/`
