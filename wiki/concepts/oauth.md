---
type: Concept
title: OAuth 授权流程
description: 客户端授权码 + PKCE（S256）、CIMD 客户端标识、private_key_jwt 客户端断言（ES256，aud 取 issuer）、RFC 8707 资源指示符与 audience 绑定、RFC 9207 授权响应 iss 校验与 token 响应「存在即校验」、发行者绑定、刷新合并与重入短路、吊销/提权、令牌缓存；服务端 Bearer 资源服务器（RFC 6750/9728，expectedResource audience 校验与 403 分流）。
tags: [oauth, pkce, 安全, rfc8707, rfc9207, rfc7523, cimd, bearer, audience, private_key_jwt]
timestamp: 2026-11-25T14:20:00+08:00
resource: src/client/auth/OAuthClientProvider.cpp
---

# OAuth 授权流程

0.3.13 候选把发现与出站策略默认收紧（`require_iss_when_advertised`、`require_pkce_metadata` 为 `true`，`allow_legacy_metadata_fallback` 为 `false`，resource 逐字比较，origin/DNS/重定向/peer 全链路策略，DCR secret 单一入口）；T-21 的 `DiscoverWithPolicy` legacy fallback 补 OIDC discovery 候选属 A 类缺陷修复，默认语义不变。旧行为/新行为/调用方改法见在线文档 `/zh/migration/0.3.13` 与 `/en/migration/0.3.13`，逐条证据见 `.handoff/evidence/T-04/`、`T-05/`、`T-21/`。

`OAuthClientProvider`（[OAuthClientProvider.cpp](../../src/client/auth/OAuthClientProvider.cpp)）——`GetAccessToken` 缓存命中路径**不持任何锁**；`GetAccessToken`、`Authenticate`、公开 `RefreshTokens` 三个入口共用一组 `refresh_mutex_` / `refresh_cv_` / `refresh_in_flight_` / `refresh_owner_` single-flight（通用 `RunSingleFlight`）：抢到标志的线程记录 owner 后释放锁执行实际工作（网络 I/O **不在锁内**），其余线程在条件变量上**有界等待**（30s），醒来后读最新令牌并按入口语义返回，不重复发请求；同线程重入（如 `token_update_handler` 回调里再调 `GetAccessToken`）直接读当前值而不等待，避免死锁；`SingleFlightGuard` 析构在正常与异常路径都清标志并 `notify_all`。`GetAccessToken` 刷新失败仍抛 `InternalError`，`Authenticate` / `RefreshTokens` 按 `RefreshOutcome` 区分 `invalid_grant` 与瞬时失败。metadata 与动态注册状态由 `state_mutex_`（`std::recursive_mutex`）保护，读取一律走快照；并发授权流由 `auth_flow_mutex_` 串行化，授权状态为局部变量。无 token_cache 时默认 `InMemoryTokenCache`。

## 流程（`Authenticate()`）

1. `DiscoverMetadata`：默认严格——PRM-first，先 RFC 9728 `/.well-known/oauth-protected-resource`，PRM 文档必须提供与配置资源标识（`OAuthClientOptions::resource`，未设则 `server_url`）逐字相等的 `resource`，再经 `authorization_servers[0]` 取 RFC 8414 元数据；任一环节失败即失败关闭。只有调用方显式开启 `allow_legacy_metadata_fallback` 才回退到 RFC 8414 直连与硬编码 `/authorize` + `/token` + issuer=server_url（[OAuthClientProvider.cpp](../../src/client/auth/OAuthClientProvider.cpp)）。T-21 续轮（2026-11-25）在该回退中补上 OIDC discovery 候选：RFC 8414 `/.well-known/oauth-authorization-server` 不可达时再尝试 `/.well-known/openid-configuration`（首个 2xx 且 JSON 对象即采用），修复 T-05 引入的 OIDC-only AS 回退缺失（官方 conformance `auth/metadata-var1`）。迁移（A 类，默认行为不变）：old——只认 OAuth AS 元数据端点，OIDC-only AS 需调用方自行准备元数据；new——显式开启 `allow_legacy_metadata_fallback` 后自动补 OIDC 回退，调用方无需改动；未开启该开关者行为逐字不变。全局 0.3.13 迁移页由 T-24 负责，证据见 `.handoff/evidence/T-21/client-fix.md`）
2. `client_id` 已配置且形态为 URL 时先 `ValidateClientId`（**CIMD**：必须是 HTTPS、含非根 path、无 fragment/userinfo；违规在所有认证入口统一抛 `InvalidParams`，[OAuthClientProvider.cpp](../../src/client/auth/OAuthClientProvider.cpp)），再 `FetchClientMetadataDocument` GET 文档；文档超 1MB（`kMaxDcrBytes`）、非对象、所载 `client_id` 与请求 URL 不等、`grant_types`/`redirect_uris` 超 128 项、已配置 `redirect_uri` 非空却不在文档 `redirect_uris` 之列时记 Error 并回退已配置值（文档不可达时同样回退，不中断授权流）；文档中的 `token_endpoint_auth_method`（非空字符串）会被采纳进注册信息）；仅当**无** `client_id` 且 metadata 有 registration_endpoint 时才 `RegisterClient`（POST `{redirect_uris, client_name:"mcp-cpp-client", application_type}`，`application_type` 取 `OAuthClientOptions::application_type`，空时回退 `"native"`）
3. 缓存 token 未过期直接成功（**先做 audience 检查**：缓存 `resource` 与当前 `ResolveResourceIndicator()` 失配时清空缓存并视为无 token）；过期但有 refresh_token → `RefreshTokens`；否则授权码流

另提供 `AuthenticateClientCredentials()`（client_credentials grant）：要求 `client_id`，并要求 `client_secret` 或可用的 `private_key_jwt` 断言二者之一，POST `grant_type=client_credentials`，同样强制 `iss` 校验（[OAuthClientProvider.cpp](../../src/client/auth/OAuthClientProvider.cpp)）。

## 授权码流

- **PKCE S256**：`GenerateCodeVerifier` 32 随机字节 → Base64url（OpenSSL `RAND_bytes`；无 OpenSSL 时 Win32 走 `BCryptGenRandom`，POSIX 直接采样 `random_device`，不再经 mt19937 中转）；`ComputeCodeChallenge` 经内置 SHA-256（[sha256.hpp](../../include/mcp/detail/sha256.hpp)，FIPS 180-4 独立实现，不依赖 OpenSSL）。`require_pkce_metadata` 默认 `true`：元数据缺失 `code_challenge_methods_supported` 或未列 `S256` 时在回调前拒绝，不省略 challenge、不退回无 challenge 流程；显式关闭该开关才恢复旧行为
- **CSRF 校验**（RFC 6749 §10.12）：回调返回的 state 必须等于发送的 state
- **授权响应 iss 校验**（RFC 9207 §2.4）：`require_iss_when_advertised` 默认 `true`；元数据广告 `authorization_response_iss_parameter_supported` 时，`iss` 缺失、空串或与 metadata issuer 不等均在 state 校验之后、`ExchangeCodeForToken` 之前拒绝；未广告时不强制，显式关闭开关仅用于兼容旧流程
- `ValidateTokenIssuer`（token/refresh 响应）：**存在即校验**——带字符串 `iss` 且与 metadata issuer 不符即拒绝（Error）；带非字符串 `iss` 同样拒绝；**不带 `iss` 则放行**（Warning）。依据：MCP 规范的 Authorization Response Validation 四态表与 RFC 9207 §2.4 只对**授权响应**的 `iss` 施加义务，token 响应的 `iss` 并非必需（RFC 9207 §2.4 明示「clients MAY accept 授权响应不带 iss，由本地策略决定」）；原实现把授权响应的义务错加到 token 响应上，导致 provider 流在换 token 阶段恒失败

## 资源指示符与 audience 绑定（RFC 8707）

`ResolveResourceIndicator()` 取值优先级：显式 `OAuthClientOptions::resource` → 已发现 metadata 的 `resource` → `server_url`。解析结果非空时注入授权 URL（`&resource=`）与三个令牌请求表单（client_credentials、授权码交换、refresh_token），为空则**完全不发送**该参数。

`TokenContainer` 新增 `resource`（audience）字段，随令牌响应落盘（`FileTokenCache` 读写 `resource` 键，空值不写）：`ParseAndStoreTokenResponse` 与 `RefreshTokens` 把解析空格分隔的 `scope`（缺 `scope` 的刷新沿用旧值）连同当前 `ResolveResourceIndicator()` 一并存入（指示符为空时刷新沿用缓存旧值）。`TokenResourceMatches` 在四处守门——指示符变化即视为无有效 token：`Authenticate`（失配先 `ClearTokens`）、`GetAccessToken`（失配返回空串）、`IsAuthenticated`/`HasToken`/`GetAuthorizationHeader`（失配返回 false/空串）。

## 发行者绑定与元数据加固

- `DiscoverMetadata`：发现结果经 `ValidateMetadataEndpoints` 强制 https（见下节「端点策略」）；**发现失败默认失败关闭**——仅当显式开启 `allow_legacy_metadata_fallback` 才使用 `issuer = server_url`、`authorization_endpoint = server_url + "/authorize"`、`token_endpoint = server_url + "/token"`（`server_url` 为空仍失败），legacy 兜底路径不做 issuer 锚定校验；发现成功时先经 `IssuerBindingAllows` 校验——**锚定持久化到令牌缓存**（`ITokenCache::StoreBoundIssuer/GetBoundIssuer`，`FileTokenCache` 落 `<cache>.issuer` 旁挂文件，清除令牌不影响锚点），锚定 issuer 与本次发现不符即拒绝并保留已存令牌（防授权服务器迁移劫持，跨 provider 实例同样生效）；首次发现经 `EnforceIssuerBinding` 写入锚点。锚点读取失败关闭：拒绝符号链接/重解析点、非普通文件、越权权限（POSIX 属主 + group/other 位全空）、超 4096 字节、控制字符与短读；失败只返回「无锚点」，不清令牌也不删已有锚点
- **端点策略**：metadata 的 authorization/token/revocation/registration 端点与动态注册 URL、挑战元数据 URL 一律要求 https（loopback 主机例外，`IsSecureEndpoint`），违规抛 `McpError(InvalidParams)`；`HandleAuthChallenge` 的挑战授权服务器须与 `server_url` **同源**（`IsSameOrigin`，scheme/host/端口归一化）或命中 `OAuthClientOptions::allowed_authorization_server_origins` 白名单，否则拒绝且不清令牌
- `ScopeFilter`（`OAuthClientOptions::scope_filter`，空即原样发送）：返回值替换授权 URL（`StartAuthorizationFlow`）与 `client_credentials` 令牌请求中的 `scope`；刷新请求不带 `scope`（缺落盘值沿用旧值），提权（`StepUpAuthorization`）合并后同样经该钩子生效
- `expectedResource` 比对：客户端 `VerifyResourceMatch` 现在要求元数据 `resource` 与配置资源标识逐字相等（缺失/空放行），不再做归一化或 scheme+host 前缀放行；服务端 audience 门（`validate_token_resource && expected_resource 非空`）沿用既有归一化比较，要求令牌 `resource` 非空且归一化后相等（[StreamableHttpServerTransport.cpp](../../src/http/StreamableHttpServerTransport.cpp)，DPoP 路径同门在 505 行），失配 401 `invalid_token`；方向不对称（服务端缺 audience 拒绝，客户端缺字段放行）
- 客户端认证方法（发送侧）：`none`、`client_secret_post`（`client_secret` 随表单）与 `private_key_jwt`——`UsesClientAssertion()` 要求 `OAuthClientOptions::private_key_pem` 非空**且**已发现元数据的 `token_endpoint_auth_methods_supported`（新字段，`ParseMetadataJson` 提取、`HandleAuthChallenge` 回填上限 128）含 `private_key_jwt`；命中时 `ApplyClientAuthentication` 向 `client_credentials`、授权码交换、`refresh_token` 三个令牌请求追加 `client_assertion_type=urn:ietf:params:oauth:client-assertion-type:jwt-bearer` 与 ES256 签名的 `client_assertion`，且不再发送 `client_secret`（`AuthenticateClientCredentials` 的 `client_secret` 必填校验同步放宽，见 [OAuthClientProvider.cpp](../../src/client/auth/OAuthClientProvider.cpp)）。断言由 `CreateClientAssertion` 构造：头 `{alg:ES256,typ:JWT}`，载荷 `{iss,sub,aud,jti,iat,exp}`（`iss`/`sub` = `client_id`，**`aud` = metadata `issuer`，缺失时才回退 `token_endpoint`**——RFC 7523 §3 第 3 条规定 `aud` 的确切字符串由 AS 与 JWT 颁发方带外约定、token endpoint URL 仅为 MAY 选项，官方 conformance referee 以 issuer identifier 为约定值；`exp = iat+60`，`jti` 取 `ext::dpop::GenerateNonce(16)`，`iat` 走可注入钩子 `client_assertion_clock_seconds`、缺省取 `ext::dpop::NowSeconds()`），载荷经 `JsonValue` 构造（键有序、转义可靠），签名复用 `mcp::ext::dpop::detail::SignEs256JwtWithPem`（PEM 私钥解析 → P-256 校验 → ES256 签名，见 [DPoP.hpp](../../include/mcp/ext/DPoP.hpp)）。`signing_algorithm` 非 `ES256`、`client_id`/`audience`/`private_key_pem` 缺失、PEM 不可解析或非 P-256 一律抛 `McpError(InvalidParams)`；无 OpenSSL 时签名抛 `InternalError`（与 DPoP 同语义，详见 [/concepts/dpop.md](/concepts/dpop.md)）。`jwt-bearer`（授权方断言）仍不支持，`token_endpoint_auth_method` 仍仅信息采纳不构造断言
- `VerifyResourceMatch` 改为逐字精确比较：元数据 `resource` 字段缺失/空放行，存在时必须与配置资源标识（`OAuthClientOptions::resource`，未设则 `server_url`）**完全相等**；scheme+host 前缀、子串、通配符、尾斜杠与 fragment 差异一概拒绝并抛 `McpError(InternalError)`；`OAuthMetadata::Discover` 同样要求 PRM `resource` 逐字匹配，否则不采用该 PRM 路径
- `HandleAuthChallenge`（RFC 9728）不再抛异常、失败一律返回 false：长度上限（整头 4096、metadata URL 2048、响应体 64KB）、`IsSafeHttpUrl`（http/https、≤2048 字符、无控制字符）卡住 metadata URL 与回填的 issuer/authorization_endpoint/token_endpoint（非法直接拒绝），`VerifyResourceMatch` 失败转 false，数组字段设上限（scopes/response_types/grant_types ≤128、code_challenge_methods ≤8），全程 try/catch 兜底

## 出站网络安全策略与 DCR secret（T-05）

- **统一策略入口**：`mcp::detail::net::UrlSecurityPolicy`（[UrlSecurityPolicy.hpp](../../include/mcp/transport/detail/UrlSecurityPolicy.hpp)）挂在 `HttpRequestSpec::security_policy` 上，被 PRM / AS 元数据 / CIMD / DCR / token / refresh / 吊销 / 挑战元数据全部复用；JWKS 仅解析进 `OAuthMetadata::jwks_uri`，本 SDK 不发起 JWKS 请求。allowlist 由调用方配置（`server_url`、`redirect_uri`、`allowed_authorization_server_origins`、`outbound_origin_allowlist`）派生，元数据响应无法自我扩张；按 scheme 规范化默认端口后精确比较，拒绝 userinfo、`%` 编码 host、纯数字/十六进制编码地址与非法端口。
- **DNS 与地址策略**：连接前解析全部 A/AAAA（上限 16），逐条拒绝 loopback / private / link-local / multicast / unspecified / 保留地址与 IPv4-mapped IPv6，任一答案非法整组拒绝；回环仅在 `loopback_port_exceptions`（含由 `server_url` 自动登记项）的精确 host+port 下允许。`TcpSocket::PeerAddress()` / `TlsSocket::PeerAddress()` 提供 `getpeername()`，连接在通过校验后、发送任何字节前复核 peer 是否仍在已校验集合中，消除重绑定窗口；`HttpRequestSpec::peer_address_override` 是测试注入点。本 HTTP 客户端没有代理路径，不存在代理绕过分支。
- **逐跳重定向**：每跳重新执行 scheme + allowlist + DNS + 地址策略；默认拒绝跨 origin，预算 5 跳（`kMaxHttpRedirects`），跨 origin 时删除 `Authorization` / `Cookie` / `client_secret` / `registration_access_token` 等敏感头。旧行为只删 `Authorization` 且可跟随跨 origin，新行为失败关闭；显式授权需把目标 origin 加入 allowlist 并设置 `allow_cross_origin_redirect = true`。
- **DCR secret 统一解析**：`OAuthClientProvider::ResolveClientSecret()` 是唯一入口，优先级为显式 `client_secret` → DCR 注册响应 → 按 `SecretCacheKey(issuer, client_id, resource)` 隔离的既有缓存值；`client_secret_post` / `client_secret_basic` 与 `client_credentials` 的必填判定都走该入口（旧实现只读显式值，DCR secret 解析后未参与认证）。空值/控制字符/超 4096 字节抛 `InvalidParams`，异常与日志只写来源类别，临时副本经 `detail::SecureClear` 清零；旧 API 与 T-04 三个兼容开关均未改动，`Discover` / `Register` 原签名保留并新增带 policy 的同名异名入口 `DiscoverWithPolicy` / `RegisterWithPolicy`。

## 失败语义

- **刷新三态**（`RefreshTokensInternal`）：成功 `Refreshed`；授权服务器明确拒绝（HTTP 4xx 且 `error` 为 `invalid_grant`/`invalid_client`）→ `InvalidGrant`，清空令牌后 `Authenticate` 回落授权码流（重走完整授权）；传输失败、5xx、缺 `access_token`、issuer 校验失败等 → `TransientFailure`，**保留缓存令牌**直接失败。`HttpPost` 区分传输失败与 HTTP 状态码并透出 `error_code`，不再把二者混为「空 JSON」
- 响应缺 `access_token` → 失败，**不回退旧 token**
- 响应缺 `refresh_token` → 保留旧值（RFC 6749 非轮转）
- `GetAccessToken`：`WillExpireSoon()`（默认 margin 60s）且有 `refresh_token` 时触发自动刷新；无 `refresh_token` 时直接返回缓存 access_token 不报错；刷新失败抛 `McpError(InternalError, "OAuth token refresh failed")`（并发调用经单一 single-flight 合并为一次请求，等待者有界返回并读最新令牌）
- `Revoke`：RFC 7009 best-effort，本地缓存无论 HTTP 结果都清空
- `StepUpAuthorization`（SEP-2350）：合并 scopes → Revoke → 重新授权
- T11 遗留（见 [/concepts/tier1-parity.md](/concepts/tier1-parity.md)）：`StepUpAuthorization` 空元数据路径存在解引用风险待加固

## 服务端 Bearer 资源服务器（RFC 6750/9728）

`StreamableHttpServerOptions::bearer_auth`（[StreamableHttpServerTransport.hpp](../../include/mcp/transport/StreamableHttpServerTransport.hpp)）把服务端变成受保护资源：

- `verify` 回调必填（`AuthResult{ok, scopes, resource}`，`resource` 为 token 携带的 audience；缺失构造抛 `McpError(InvalidRequest)`），token 校验完全委托调用方（SDK 不解析 JWT）
- POST/GET 入口挑战：无头/非 Bearer → 401 + `WWW-Authenticate: Bearer resource_metadata="<url>"`；校验失败 → 401 + `error="invalid_token"`；`required_scopes` 未被 token scopes 全覆盖 → 403 + `error="insufficient_scope", scope="..."`；**audience 门**（`validate_token_resource` 默认为 true 且 `expected_resource` 非空时生效）：`result.resource` 与 `expected_resource` 归一化（去 `#fragment`/尾斜杠）不等 → 401 + `error="invalid_token"`（与 scope 缺口的 403 分流：audience 失配走 401）
- `serve_metadata_endpoint` 注册匿名 `GET /.well-known/oauth-protected-resource`（RFC 9728 文档：`resource`/`authorization_servers`/`scopes_supported`/`bearer_methods_supported`）
- 未配置时零回归（无任何鉴权路径）；wire 细节见 [/transports/streamable-http.md](../transports/streamable-http.md)；客户端侧对接入口为 `auth_challenge_handler`（401 必重试；403 仅 `WWW-Authenticate` 含 `insufficient_scope`（大小写不敏感）时重试，其余 403 不重试）
- 客户端 DPoP 发送侧：`OAuthClientProvider::ApplyDPoPProofHeader(context, headers)` 与 `ObserveDPoPNonce(nonce)` 可直接绑到传输的 `apply_per_request_headers` / `observe_response_challenge` 回调（每个 POST 签发全新 jti/iat/htu 的 proof，htu 剥 query；401/400 `use_dpop_nonce` 触发一次重试并重算 proof），依赖 OpenSSL，详见 [/concepts/dpop.md](/concepts/dpop.md)

## 相关页面

- [/modules/client.md](../modules/client.md) — 所属库
- [/transports/streamable-http.md](../transports/streamable-http.md) — 服务端 Bearer 挑战落点
- [/classes/file-token-cache.md](../classes/file-token-cache.md) — 持久化
- [/concepts/storage.md](storage.md) — 原子写入
- [/docs/en/client/oauth.md](../../docs/en/client/oauth.md) — 在线文档
