# 给客户端加上 OAuth

让 `StreamableHttpClientTransport` 带着可用的 access token 出门，并在服务端甩 `401`/`403` 挑战时自己补齐：发现授权服务器元数据、走授权码 + PKCE 或客户端凭据、把 token 存进你自己的缓存、必要时升权（step-up）或换 DPoP 证明。本页只讲客户端侧装配；服务端如何校验 Bearer 见 [/zh/howto/protect-an-http-server](/zh/howto/protect-an-http-server)。

::: info 认证与协议时代无关
OAuth、PKCE、DPoP 全在 HTTP 传输层，双时代（legacy / modern）行为一致；时代只影响这条连接上跑的**协议方法集合**，见 [/zh/learn/protocol-versions](/zh/learn/protocol-versions)。
:::

## 前置条件

- 头文件：`mcp/client/auth/OAuthClientProvider.hpp`、`mcp/client/auth/TokenCache.hpp`、`mcp/transport/StreamableHttpClientTransport.hpp`、`mcp/client/ClientOptions.hpp`、`mcp/Implementation.hpp`；DPoP 类型来自 `mcp/ext/DPoP.hpp`（被 provider 头文件带入）。
- 认证代码在 `mcp-client` 与 `mcp-ext` 两个目标里，按需链接（库清单见 [/zh/start/installation](/zh/start/installation)）。
- **TLS 需要 OpenSSL**：`-DMCP_WITH_OPENSSL=ON` 未找到会直接 configure 失败；`AUTO`（默认）按 `find_package(OpenSSL)` 结果决定，未启用则整条 HTTPS 不可用（PKCE 随机源会回退，见下文）。
- 端点必须是 `https`，回环地址例外；否则相关调用抛 `McpError(InvalidParams)`，消息形如 `OAuth <kind> endpoint must use https (loopback hosts excepted)`（`src/client/auth/OAuthClientProvider.cpp` 与其后的 `RequireSecureEndpoint`）。

## 建 provider 与选流

`OAuthClientProvider` 是**独立于 MCP 客户端**的对象：它负责取 token，传输层负责用它。最小配置只有三项——`server_url`、`redirect_uri`、`token_cache`（`include/mcp/client/auth/OAuthClientProvider.hpp`）。

<<< @/snippets/add-oauth-to-a-client.cpp#provider-options

两条主流：

- **授权码 + PKCE**：填 `authorization_redirect_handler`（拿到授权 URL 后你自己去开浏览器）与 `authorization_code_callback`（把回调里拿到的 `code`/`state` 交回来），然后调 `Authenticate()`。
- **客户端凭据**：无浏览器场景，直接 `AuthenticateClientCredentials()`；`application_type` 写 `"confidential"`，并可给出 `client_secret`。

<<< @/snippets/add-oauth-to-a-client.cpp#client-credentials

`client_id` 留空且元数据里有 `registration_endpoint` 时走**动态注册**（`include/mcp/client/auth/TokenCache.hpp`）；需要 `private_key_jwt` 一类的客户端断言时给 `private_key_pem` + `client_assertion_clock_seconds`（时钟注入点，便于测试）。

## 把它接到传输层

传输层只暴露三个钩子（`include/mcp/transport/StreamableHttpClientTransport.hpp`）：

| 钩子 | 时机 | 你要返回/做什么 |
|---|---|---|
| `auth_challenge_handler(www_authenticate)` | 收到 `401`，或带 `insufficient_scope` 的 `403` | 返回**新的 `Authorization` 头值**；返回空串表示放弃重试 |
| `apply_per_request_headers(context, headers)` | 每个出站请求 | 加 DPoP 证明等随请求变化的头 |
| `observe_response_challenge(context)` | 每个响应 | 记下 `DPoP-Nonce` 之类的新挑战值 |

<<< @/snippets/add-oauth-to-a-client.cpp#wire-transport

重试策略是硬编码的，值得逐条知道（`src/http/StreamableHttpClientTransport.cpp`）：

- 只在**第一次尝试**上做认证重试——补齐后仍失败就直接把错误交回上层，不会无限循环。
- `401` 一律 eligible；`403` 只有当 `WWW-Authenticate` 是 `insufficient_scope` 挑战才 eligible。
- handler 返回空串即放弃。此外 `use_dpop_nonce` 类错误会独立触发一次重试，`400` 且响应体是参数头不匹配时先刷新工具注解再重试（见 [/zh/howto/protect-an-http-server](/zh/howto/protect-an-http-server)）。

## token 的取用与自动续期

`GetAccessToken()` 不只是读缓存：命中且**临近过期且有 refresh_token** 时它会先合并式刷新（同一时刻只跑一次刷新，其余调用等待）再返回（`src/client/auth/OAuthClientProvider.cpp`）。两种情况它会返回**空串**：缓存里没有 token，或 token 的 `resource` 与你配置的 `resource` 不一致（RFC 8707 受众绑定，`TokenResourceMatches`）。

<<< @/snippets/add-oauth-to-a-client.cpp#step-up

- `RefreshTokens()` 失败分两类：`invalid_grant`/`invalid_client` 属凭据被拒（要重走授权流），其余算临时故障。
- `StepUpAuthorization(scopes)` 用于 403 之后追加权限；`scope_filter` 让你在授权 URL 生成前裁剪对方支持的 scope。
- `Revoke()` 会尽力调撤销端点并清空缓存。

## DPoP

三个选项开：`enable_dpop = true`、`private_key_pem`（EC P-256 私钥）、`signing_algorithm`（默认 `"ES256"`）。之后 `Authorization` 用 `"DPoP <token>"` 而不是 `"Bearer ..."`，并**每个请求**都要挂一次证明：

<<< @/snippets/add-oauth-to-a-client.cpp#dpop

`ApplyDPoPProofHeader(context, headers)` 按 `context.http_method` / `context.url` / `context.purpose` 造 `htm`/`htu`；`ObserveDPoPNonce(context.dpop_nonce)` 把服务端下发的 nonce 存回 provider，下次自动带上。需要手工造证明时（例如自己发 HTTP）用 `BuildDPoPProof(htm, htu, access_token)`。

## 别把授权服务器指错

`allowed_authorization_server_origins` 是白名单：元数据的 `issuer`（缺失时退到 `token_endpoint`）的 origin 必须等于 `server_url` 的 origin，或在白名单里，否则不被接受（`src/client/auth/OAuthClientProvider.cpp` 的 `IsAllowedAuthorizationServer`）。同源判定用 `IsSameOrigin`，端点安全性判定用 `IsSecureEndpoint`（回环例外）。

<<< @/snippets/add-oauth-to-a-client.cpp#endpoint-hygiene

令牌响应里的 `iss` 若缺失，SDK **只记一条 Warning**、按本地策略放行（RFC 9207 里它本就是可选的）；要强制约束请自己校验或收紧白名单。

## 缓存由你实现

`ITokenCache` 只有四个必选方法 + 两个可选（绑定 issuer 的持久化，用于跨进程复用同一发行方），另给一个 `InMemoryTokenCache` 做默认（`include/mcp/client/auth/TokenCache.hpp`）。`TokenContainer` 含 `access_token`、`refresh_token`、`scopes`、`expires_at`（**绝对秒数**，不是剩余秒）、`token_type`、`resource`，以及 `IsExpired()` / `WillExpireSoon(margin_ms = 60000)`。

::: warning 别把 token 落进日志
`access_token`、`refresh_token`、`client_secret`、`private_key_pem` 全属机密：`MCP_LOG_LEVEL=4` 下排查时也不要打印它们；实现 `ITokenCache` 时持久化位置要按你自己项目的密钥管理规则来。
:::

## 完整代码

装配一个「启动即认证、挑战时续期」的客户端：

<<< @/snippets/add-oauth-to-a-client.cpp#full

## 验证

片段只做语法校验（clang-cl 走 `/Zs`，GCC/Clang 走 `-fsyntax-only`），不起进程、不联网：

```bash
cmake -S . -B build/p3a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3a --target mcp-doc-snippet-add-oauth-to-a-client
```

期望输出：configure 阶段打印 `[mcp-docs] syntax-checking 15 snippet(s) with '/Zs'`，构建阶段打印

```text
[1/4] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-add-oauth-to-a-client.dir\__\__\docs\snippets\add-oauth-to-a-client.cpp.obj
```

返回码 0 且无 `error:` 行。端到端行为请跑仓库里的 conformance 客户端（需 `-DMCP_BUILD_CONFORMANCE=ON`），它同时演示 Bearer 与 DPoP 两条链路：`examples/conformance/client/main.cpp`（Bearer 段，与靠后的 DPoP 段），配套 `scripts/run-conformance.sh`。

## 时代差异

认证不参与协议时代门控，但连接上跑的协议方法集合参与：

| 项目 | legacy（≤ 2025-11-25） | modern（2026-07-28） |
|---|---|---|
| OAuth / PKCE / DPoP 行为 | 完全一致（传输层） | 完全一致（传输层） |
| 401/403 重试 | 一致，且只在首次尝试 | 同左 |
| 与 `Mcp-Param-*` 头的相互作用 | 不适用 | `400` 头不匹配会触发工具注解刷新重试，与认证重试互不占用 |
| 会话头 | 需要 `mcp-session-id` | 同左；认证失败导致的重连不影响会话恢复 |

## 常见错误

| 对外表现 | 成因 | 处置 |
|---|---|---|
| `OAuth <kind> endpoint must use https (loopback hosts excepted)` | 端点是明文 http 且非回环 | 换 https；本地联调用 `localhost` |
| configure 阶段就失败并说找不到 OpenSSL | 显式 `-DMCP_WITH_OPENSSL=ON` 但系统无 OpenSSL | 装 OpenSSL 3.x/4.x，或改回 `AUTO`/`OFF`（`OFF` 会禁用 TLS，届时 HTTPS 端点全部不可用） |
| `GetAccessToken()` 返回空串 | 缓存无 token，或 `resource` 与令牌上的受众不匹配 | 先 `Authenticate()`；`resource` 要与令牌签发时一致（RFC 8707） |
| 403 之后没有任何重试 | `WWW-Authenticate` 不带 `insufficient_scope` | 这是按规范故意不重试；需要升权时自己调 `StepUpAuthorization` |
| 反复 401 打爆授权服务器 | 误以为认证重试可多轮 | 只有首次尝试会补一次；仍失败就回到上层，请检查 scope/audience |
| DPoP 请求一直被拒并提示 nonce | 没接 `observe_response_challenge` | 三个钩子都要接，nonce 由 provider 记住 |
| `iss` 不匹配也没报错 | 缺 `iss` 只记 Warning（RFC 9207 可选） | 用 `allowed_authorization_server_origins` 收紧，或自行校验 |
| 每次进程重启都要重新授权 | 用了 `InMemoryTokenCache` | 实现自己的 `ITokenCache` 并持久化，顺带实现 `StoreBoundIssuer` |

## 下一步

- 服务端侧怎么校验 Bearer 与 DPoP：[/zh/howto/protect-an-http-server](/zh/howto/protect-an-http-server)
- 传输与端点选型：[/zh/learn/transports](/zh/learn/transports)、[/zh/reference/transports](/zh/reference/transports)
- 失败与错误码怎么读：[/zh/reference/errors](/zh/reference/errors)
- 连接装配与关闭时机：[/zh/howto/connect-to-a-server](/zh/howto/connect-to-a-server)

## 本页对应源码

- `include/mcp/client/auth/OAuthClientProvider.hpp`（`OAuthClientOptions`、公开方法、`pkce::*`）
- `include/mcp/client/auth/TokenCache.hpp`（`TokenContainer`、`ITokenCache`、`OAuthMetadata`、`ClientRegistrationInfo`）
- `include/mcp/ext/DPoP.hpp`（EC P-256 JWK）
- `include/mcp/transport/StreamableHttpClientTransport.hpp`（三个认证钩子与 `HttpRequestHeaderContext`、`HttpResponseChallengeContext`）
- `src/client/auth/OAuthClientProvider.cpp`（端点校验、元数据发现、PKCE 随机源、刷新合并、受众绑定）
- `src/http/StreamableHttpClientTransport.cpp`（401/403 与 nonce 的重试路径）
- `examples/conformance/client/main.cpp`（Bearer 与 DPoP 的真实装配）
