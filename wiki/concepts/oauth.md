---
type: Concept
title: OAuth 授权流程
description: 客户端授权码 + PKCE（S256）、CIMD 客户端标识、RFC 8707 资源指示符与 audience 绑定、RFC 9207 iss 强制校验与发行者绑定、刷新/吊销/提权、令牌缓存；服务端 Bearer 资源服务器（RFC 6750/9728，expectedResource audience 校验与 403 分流）。
tags: [oauth, pkce, 安全, rfc8707, rfc9207, cimd, bearer, audience]
timestamp: 2026-10-13T11:03:35+08:00
resource: src/client/auth/OAuthClientProvider.cpp
---

# OAuth 授权流程

`OAuthClientProvider`（[OAuthClientProvider.cpp](../../src/client/auth/OAuthClientProvider.cpp)）——整体按单线程使用设计：除 token 刷新路径外无同步保护。`GetAccessToken` 缓存命中路径**不持任何锁**；仅在需要刷新时经 `RefreshTokensCoalesced` 用 `refresh_mutex_` + `refresh_cv_` + `refresh_in_flight_` 标志做**一次性刷新协调**：抢到标志的线程释放锁后执行 `RefreshTokens`（网络 I/O **不在锁内**），其余线程在条件变量上等待，完成后复查缓存——已新鲜则直接复用，否则自己再抢刷新（失败同样抛错）。因此 N 路并发 `GetAccessToken` 只触发一次刷新请求。`Authenticate`、`Revoke`、`StepUpAuthorization` 以及 metadata/registration 状态不能据此视为整体线程安全。无 token_cache 时默认 `InMemoryTokenCache`。

## 流程（`Authenticate()`）

1. `DiscoverMetadata`：PRM-first——先 RFC 9728 `/.well-known/oauth-protected-resource`，经 `authorization_servers[0]` 定位授权服务器后再取其 RFC 8414 元数据（AS 文档缺 `resource` 时沿用 PRM 文档的值）；逐级失败记 Warning 回退（PRM→RFC 8414 直连→硬编码 `/authorize` + `/token` + issuer=server_url），全程不硬挂
2. `client_id` 已配置时先 `FetchClientMetadataDocument`（**CIMD**：仅 `http(s)://` 形态的 `client_id` 视为文档 URL（[OAuthClientProvider.cpp:154](../../src/client/auth/OAuthClientProvider.cpp)），先过 `IsSafeHttpUrl` 再 GET，文档超 1MB（`kMaxDcrBytes`）、非对象、所载 `client_id` 与请求 URL 不等、`grant_types`/`redirect_uris` 超 128 项、已配置 `redirect_uri` 非空却不在文档 `redirect_uris` 之列时一律记 Warning/Error 并回退已配置值、不中断授权流；文档中的 `token_endpoint_auth_method`（非空字符串）会被采纳进注册信息）；仅当**无** `client_id` 且 metadata 有 registration_endpoint 时才 `RegisterClient`（POST `{redirect_uris, client_name:"mcp-cpp-client", application_type}`，`application_type` 取 `OAuthClientOptions::application_type`，空时回退 `"native"`）
3. 缓存 token 未过期直接成功（**先做 audience 检查**：缓存 `resource` 与当前 `ResolveResourceIndicator()` 失配时清空缓存并视为无 token）；过期但有 refresh_token → `RefreshTokens`；否则授权码流

另提供 `AuthenticateClientCredentials()`（client_credentials grant）：要求 `client_id` + `client_secret`，POST `grant_type=client_credentials`，同样强制 `iss` 校验（[OAuthClientProvider.cpp:275](../../src/client/auth/OAuthClientProvider.cpp)）。

## 授权码流

- **PKCE S256**：`GenerateCodeVerifier` 32 随机字节 → Base64url（OpenSSL `RAND_bytes`；无 OpenSSL 时 Win32 走 `BCryptGenRandom`，仅 POSIX 回退 `random_device + mt19937`，该回退不等同于操作系统 CSPRNG）；`ComputeCodeChallenge` 经内置 SHA-256（[sha256.hpp](../../include/mcp/detail/sha256.hpp)，FIPS 180-4 独立实现，不依赖 OpenSSL）
- **CSRF 校验**（RFC 6749 §10.12）：回调返回的 state 必须等于发送的 state
- **授权响应 iss 校验**（RFC 9207）：`AuthorizationCodeResult::iss` 有值且与 metadata issuer 不等即拒绝，位置在 state 校验之后、`ExchangeCodeForToken` 之前；未携带 `iss` 则不校验
- `ValidateTokenIssuer`（**RFC 9207 强制**）：token/refresh 响应缺 `iss` 或与 metadata issuer 不匹配即拒绝

## 资源指示符与 audience 绑定（RFC 8707）

`ResolveResourceIndicator()` 取值优先级：显式 `OAuthClientOptions::resource` → 已发现 metadata 的 `resource` → `server_url`。解析结果非空时注入授权 URL（`&resource=`）与三个令牌请求表单（client_credentials、授权码交换、refresh_token），为空则**完全不发送**该参数。

`TokenContainer` 新增 `resource`（audience）字段，随令牌响应落盘（`FileTokenCache` 读写 `resource` 键，空值不写）：`ParseAndStoreTokenResponse` 与 `RefreshTokens` 把解析空格分隔的 `scope`（缺 `scope` 的刷新沿用旧值）连同当前 `ResolveResourceIndicator()` 一并存入（指示符为空时刷新沿用缓存旧值）。`TokenResourceMatches` 在四处守门——指示符变化即视为无有效 token：`Authenticate`（失配先 `ClearTokens`）、`GetAccessToken`（失配返回空串）、`IsAuthenticated`/`HasToken`/`GetAuthorizationHeader`（失配返回 false/空串）。

## 发行者绑定与元数据加固

- `DiscoverMetadata`：`issuer` 为必填字段（缺失的服务端实现应补齐），当前实现缺失时记 Warning 并继续回退链（未来收紧为直接失败）；成功后经 `EnforceIssuerBinding`——首次绑定 `bound_issuer_`，后续发现 issuer 变化时清空动态注册信息与本地令牌再重绑（防授权服务器迁移劫持）
- `ScopeFilter`（`OAuthClientOptions::scope_filter`，空即原样发送）：返回值替换授权 URL（`StartAuthorizationFlow`）与 `client_credentials` 令牌请求中的 `scope`；刷新请求不带 `scope`（缺落盘值沿用旧值），提权（`StepUpAuthorization`）合并后同样经该钩子生效
- `expectedResource` 比对（双端同归一化 `NormalizeResourceUrl`：去 `#fragment`、去尾斜杠）：客户端 `VerifyResourceMatch` 要求元数据 `resource` 与 `server_url` 精确相等或匹配 scheme+host 前缀（缺失/空放行）；服务端 audience 门（`validate_token_resource && expected_resource 非空`）要求令牌 `resource` 非空且相等（[StreamableHttpServerTransport.cpp:542](../../src/http/StreamableHttpServerTransport.cpp)，DPoP 路径同门在 505 行），失配 401 `invalid_token`；方向不对称（服务端缺 audience 拒绝，客户端缺字段放行）
- 客户端认证方法声明（仅文档层）：发送侧仅 `none`/`client_secret_post`；`private_key_jwt`/`jwt-bearer` 断言暂不支持，`token_endpoint_auth_method` 仅信息采纳不构造断言
- `VerifyResourceMatch` 归一化后比较（`NormalizeResourceUrl`：去 `#fragment`、去尾斜杠）：`OAuthMetadata.resource` 字段须与 `options_.server_url` **精确相等**或匹配其 **scheme+host** 前缀；字段缺失/空则放行；不匹配抛 `McpError(InternalError)`
- `HandleAuthChallenge`（RFC 9728）不再抛异常、失败一律返回 false：长度上限（整头 4096、metadata URL 2048、响应体 64KB）、`IsSafeHttpUrl`（http/https、≤2048 字符、无控制字符）卡住 metadata URL 与回填的 issuer/authorization_endpoint/token_endpoint（非法直接拒绝），`VerifyResourceMatch` 失败转 false，数组字段设上限（scopes/response_types/grant_types ≤128、code_challenge_methods ≤8），全程 try/catch 兜底

## 失败语义

- 响应缺 `access_token` → 失败，**不回退旧 token**（`RefreshTokens` 亦然）
- 响应缺 `refresh_token` → 保留旧值（RFC 6749 非轮转）
- `GetAccessToken`：`WillExpireSoon()`（默认 margin 60s）且有 `refresh_token` 时触发自动刷新；无 `refresh_token` 时直接返回缓存 access_token 不报错；刷新失败抛 `McpError(InternalError, "OAuth token refresh failed")`（并发调用经 `RefreshTokensCoalesced` 合并为一次请求）
- `Revoke`：RFC 7009 best-effort，本地缓存无论 HTTP 结果都清空
- `StepUpAuthorization`（SEP-2350）：合并 scopes → Revoke → 重新授权
- T11 遗留（见 [/concepts/tier1-parity.md](/concepts/tier1-parity.md)）：`StepUpAuthorization` 空元数据路径存在解引用风险待加固；README 的 PKCE/DCR 公开表述待统一（`README.md:96` 与 `README.md:276` 并存）

## 服务端 Bearer 资源服务器（RFC 6750/9728）

`StreamableHttpServerOptions::bearer_auth`（[StreamableHttpServerTransport.hpp](../../include/mcp/transport/StreamableHttpServerTransport.hpp)）把服务端变成受保护资源：

- `verify` 回调必填（`AuthResult{ok, scopes, resource}`，`resource` 为 token 携带的 audience；缺失构造抛 `McpError(InvalidRequest)`），token 校验完全委托调用方（SDK 不解析 JWT）
- POST/GET 入口挑战：无头/非 Bearer → 401 + `WWW-Authenticate: Bearer resource_metadata="<url>"`；校验失败 → 401 + `error="invalid_token"`；`required_scopes` 未被 token scopes 全覆盖 → 403 + `error="insufficient_scope", scope="..."`；**audience 门**（`validate_token_resource` 默认为 true 且 `expected_resource` 非空时生效）：`result.resource` 与 `expected_resource` 归一化（去 `#fragment`/尾斜杠）不等 → 401 + `error="invalid_token"`（与 scope 缺口的 403 分流：audience 失配走 401）
- `serve_metadata_endpoint` 注册匿名 `GET /.well-known/oauth-protected-resource`（RFC 9728 文档：`resource`/`authorization_servers`/`scopes_supported`/`bearer_methods_supported`）
- 未配置时零回归（无任何鉴权路径）；wire 细节见 [/transports/streamable-http.md](../transports/streamable-http.md)；客户端侧对接入口为 `auth_challenge_handler`（401 必重试；403 仅 `WWW-Authenticate` 含 `insufficient_scope`（大小写不敏感）时重试，其余 403 不重试）

## 相关页面

- [/modules/client.md](../modules/client.md) — 所属库
- [/transports/streamable-http.md](../transports/streamable-http.md) — 服务端 Bearer 挑战落点
- [/classes/file-token-cache.md](../classes/file-token-cache.md) — 持久化
- [/concepts/storage.md](storage.md) — 原子写入
- [/docs/en/client/oauth.md](../../docs/en/client/oauth.md) — 在线文档
