---
type: Concept
title: DPoP 证明绑定令牌
description: DPoP（RFC 9449 / SEP-1932 草案）在本 SDK 的落点：dpop 原语的 JWS 签发与验签链（htm/htu/ath/nonce/iat 窗口 + jti 重放表）、Streamable HTTP 服务端 Bearer 链上的 DPoP 校验门、OAuthClientProvider 发送侧开关与 nonce 接口、传输层逐请求 proof 头注入口与 use_dpop_nonce 单次重试、OpenSSL 可选依赖缺失时的抛错降级语义。
tags: [dpop, oauth, 安全, sep-1932, rfc9449, 传输, openssl]
timestamp: 2026-10-13T14:20:00+08:00
resource: src/ext/DPoP.cpp
---

# DPoP 证明绑定令牌

DPoP（RFC 9449）用持有者私钥签发的 JWS 证明把 access token 绑定到密钥，泄密令牌无法被重放。MCP 侧对应 SEP-1932「DPoP Profile for MCP」，截至 2026-10 仍处 Draft（来源：`modelcontextprotocol/modelcontextprotocol#2753` 与 `go-sdk#1139` 均记 SEP-1932 in-flight / Draft）。

本 SDK 的落点分三层：`mcp::ext::dpop` 原语（密钥、JWS 签发、验签、重放表）、Streamable HTTP 服务端资源服务器的 DPoP 校验门、`OAuthClientProvider` 的发送侧开关与 nonce 接口。传输层已提供逐请求头注入口与响应侧观察入口（见「客户端发送侧」）。`detail::SignEs256JwtWithPem`（以 PEM 私钥签任意 ES256 JWT）同样落在本模块，OpenSSL 缺失时抛错（[DPoP.hpp:62-67](../../include/mcp/ext/DPoP.hpp)）。

## 开关与依赖

- 服务端门：`StreamableHttpServerOptions::bearer_auth->enable_dpop`（默认 false，[StreamableHttpServerTransport.hpp:74](../../include/mcp/transport/StreamableHttpServerTransport.hpp)）——**唯一实际生效的门**，且须先配置 `bearer_auth`。
- `ServerOptions::enable_dpop`（[ServerOptions.hpp:90](../../include/mcp/server/ServerOptions.hpp)）已声明但 `src/server` 无读取点，McpServer 不消费、也不透传给传输构造，置 `true` 无任何效果；有效门控见 [/concepts/experimental-flags.md](/concepts/experimental-flags.md)。
- 客户端门：`OAuthClientOptions::enable_dpop`（默认 false，[OAuthClientProvider.hpp:34](../../include/mcp/client/auth/OAuthClientProvider.hpp)），provider 构造期拷贝取值；两端开关互不影响，需各自显式开启。
- **OpenSSL 可选依赖**：`MCP_HAVE_OPENSSL` 未定义时退而用 `__has_include` 探测 ec/ecdsa/evp/obj_mac/rand 头（[DPoP.cpp:8-16](../../src/ext/DPoP.cpp)），二者皆无则整条密码学路径不可用——`GenerateKey`/`CreateProof`/`VerifyProof` 三个函数**一律抛** `McpError(InternalError, "DPoP requires OpenSSL EC support, which is not available in this build")`（[DPoP.cpp:475-495](../../src/ext/DPoP.cpp)）。
  - 服务端在 `AuthorizeRequest` 内用 `try/catch` 包住 `VerifyProof`，异常按「校验失败」处理回 401，不中断 HTTP 服务。
  - 客户端 `BuildDPoPProof` 无 catch，无 OpenSSL 构建下开启开关会把异常抛给调用方（授权流直接失败）。
  - 与 OpenSSL 无关的部分：SHA-256 为内置 FIPS 180-4 实现（[sha256.hpp](../../include/mcp/detail/sha256.hpp)），Base64url 为自研。
  - `GenerateNonce` 有 OpenSSL 走 `RAND_bytes`，否则逐字节取 `std::random_device`（[DPoP.cpp:151-166](../../src/ext/DPoP.cpp)）——无 OpenSSL 构建的 nonce 随机源不等同 OS CSPRNG。

## 服务端校验链

`StreamableHttpServerTransport::AuthorizeRequest`（[StreamableHttpServerTransport.cpp:509-582](../../src/http/StreamableHttpServerTransport.cpp)）。未配置 `bearer_auth` 时直接放行（`:512`），DPoP 分支在其内部，与 Bearer 分支互斥：

1. `Authorization` 必须是 `DPoP ` 方案（前缀大小写不敏感匹配，`:211`）；缺头 / 方案不符 / 令牌去空白后为空 → **401** + `error="invalid_token"` + `DPoP-Nonce`
2. `verify(token)` 回调失败（SDK 不解析 JWT，令牌校验全权委托调用方）→ **401** + `DPoP-Nonce`
3. `DPoP` 证明头验签：构造 `DPoPVerifyOptions`——`htm` 取请求方法；`htu` = `http://<Host><path 去掉 query>`；`alternate_htu` = `https://<Host><path>`（http/https 两种都对）；`access_token` 取本次携带的令牌（`ath` 绑定）；`expected_nonce` 取进程级服务端 nonce（首次访问经 `GenerateNonce` 生成后常驻，`DPoPCurrentServerNonce` 带静态 mutex，`:237`）。`VerifyProof` 外层 `try/catch` 吞异常按失败算；失败 → **401** + `DPoP-Nonce`
4. audience 门（`validate_token_resource && expected_resource` 非空，归一化后不等）→ **401** + `invalid_token` + `DPoP-Nonce`
5. `required_scopes` 未被令牌 scopes 全覆盖 → **403** + `insufficient_scope` + `scope="…"` + `DPoP-Nonce`
6. 放行：响应恒带 `DPoP-Nonce`（成功路径也回，`:580`），客户端据此预取 nonce

一处口径细节：DPoP 分支的 `WWW-Authenticate` 挑战串仍写 `Bearer resource_metadata="…"`（`RespondUnauthorized` 硬编码 `Bearer`，`:185-188`），未改写为 DPoP 方案；`DPoP-Nonce` 头与 401 语义不受影响。

## 证明校验（`dpop::VerifyProof`）

[DPoP.cpp:365-471](../../src/ext/DPoP.cpp)，失败一律返回 false（不抛），校验顺序：

- 结构：JWS compact 恰好三段，签名段 base64url 解码须恰 64 字节（P-256 的 `r‖s` 定长）
- 头：`alg` 必须 `ES256`；`typ` 必须 `dpop+jwt`（大小写不敏感）；`jwk` 须 `kty=EC`、`crv=P-256`，`x`/`y` 非空且各解出 32 字节；**`jwk.d` 存在即拒**（证明中夹带私钥材料不接受）
- 载荷：`htm` 精确相等；`htu` 与期望值（或 `alternate_htu`）比较前各自剥掉 `?`/`#` 之后的部分（`StripHtuSuffix`），故 query 差异不致命；`iat` 必须存在且经 `IsNonceFresh` 判新鲜（窗口默认 300s，未来时间戳拒）；`jti` 非空；`ath` 必须等于 `base64url(SHA-256(access_token))`；`nonce` 若出现必须与期望一致，但**不强制出现**——服务端首个不带 nonce 的证明可通过，之后带错 nonce 即拒
- 签名：对 `header.payload` 的 SHA-256 摘要做 `ECDSA_do_verify`
- 重放：验签通过后才进 `replay.CheckAndStore(jti, iat, now)`——先在互斥量内按 `now - seen > window` 淘汰过期项，再查重，重复 jti 直接 false。服务端用的是**进程级单例** `DPoPReplayCache()`（`static`，窗口 300s，`:222`），跨全部会话共享

守护测试：`DPoPBoundTokenRequiresMatchingProof`、`DPoPProofReplayRejected`、`DPoPDisabledSkipsProofChecks`（[StreamableHttpTransportTests.cpp:1865/1921/1969](../../tests/unit/StreamableHttpTransportTests.cpp)）；`DPoPDisabledByDefaultSkipsProof`、`DPoPEnabledBindsSchemeAndProof`、`DPoPStaleProofRejectedByWindow`（[OAuthTests.cpp:263/282/320](../../tests/unit/OAuthTests.cpp)）。

## 客户端发送侧（已接线部分）

- `GetAuthorizationHeader()`：开关开返回 `DPoP <access_token>`，否则 `Bearer <access_token>`（[OAuthClientProvider.cpp:760-767](../../src/client/auth/OAuthClientProvider.cpp)）
- `BuildDPoPProof(htm, htu, access_token)`：开关关直接返回空串；开则在 `dpop_mutex_` 保护下懒生成进程内单份 P-256 密钥（`GenerateKey`），`dpop_nonce_` 非空时写入 `nonce` claim（`:769-779`）
- `SetDPoPNonce(nonce)`：写入 `dpop_nonce_`，后续签发的证明携带该 nonce（`:781-784`）
- provider 自己发往授权服务器的 `HttpPost`（令牌交换 / 刷新 / 动态注册等）在开关开时自动附 `DPoP` 头（`:278-280`）；该请求**不带 `ath`**（`BuildDPoPProof` 未传 access_token）——按 RFC 9449，`ath` 只用于绑资源请求，授权服务器请求不绑
- `ComputeThumbprint(x, y)` 提供 P-256 JWK 规范化指纹（SHA-256 of `{"crv","kty","x","y"}` 规范化 JSON），但 `RegisterClient` 的注册体未发送 `dpop_jkt`（注册字段为 `redirect_uris` / `client_name` / `application_type`，见 [/concepts/oauth.md](/concepts/oauth.md)）

## 逐请求 proof 与 nonce 重试（传输层接线）

传输层新增两个可选回调（[StreamableHttpClientTransport.hpp:51-55](../../include/mcp/transport/StreamableHttpClientTransport.hpp)），二者均**默认未设置**，未配置时零回归：

- `apply_per_request_headers(context, headers)`：**每次发请求前**（含重试与内部自发的补发）被调用一次，入参 `HttpRequestHeaderContext` 含 `http_method` / `url`（实际请求 URL，重定向后为新 URL）/ `purpose`（`MessagePost`/`ListenStream`/`ResumeStream`/`SessionDelete`/`ToolAnnotationsRefresh`）/ `request_id` / `attempt_index`；回调把要追加的头写进可写 `headers` 引用，**覆盖在静态 `additional_headers` 与 `Authorization` 之后**（后者先写），故回调可覆盖二者。Win32 与 POSIX 两实现共用 `streamable_http_client_impl::ResolvePerRequestHeaders` 判定，行为强制一致。
- `observe_response_challenge(context)`：**每个响应**（含 2xx 与 SSE）被调用一次，入参含 `status_code` / `www_authenticate` / `dpop_nonce`（`DPoP-Nonce` 响应头）。视图存活至本次调用返回，回调不得持有。

provider 侧适配器（可直接绑到上述两个回调）：

- `ApplyDPoPProofHeader(context, headers)`：开关关或 `enable_dpop` 为 false 时直接返回不写头；否则以 `htm = context.http_method`、`htu` = `context.url` **剥掉 query 与 fragment**（`DpopHtu`，防敏感信息落入证明）计算 proof，写入 `headers["DPoP"]`。`ath` 在能取到未过期且 resource 匹配的 access token 时按 RFC 9449 绑定传入（`BuildDPoPProof` 第三参），否则不带。每次调用内部重新签发 → `jti`/`iat` 天然逐请求全新，满足 referee `sep-1932-client-fresh-proof`。
- `ObserveDPoPNonce(dpop_nonce)`：非空且长度 ≤1024 时写入 `dpop_nonce_`，后续 proof 自动携带该 nonce claim。

`use_dpop_nonce` 重试（`RequiresDPoPNonceRetry`，Win32/POSIX 共用）：仅 `attempt == 0`（首轮）且状态码为 **401 或 400**、且 `WWW-Authenticate` 或响应体前 4KB 含 `use_dpop_nonce` 时触发**恰好一次**重试并重算 proof；重试与 `auth_challenge_handler`（401/`insufficient_scope`）及 400 `HeaderMismatch` 刷新共用同一「一次」预算，先命中者占用。响应体扫描做 4KB 上限，防大响应放大开销。

OpenSSL 缺失时 `GenerateKey`/`CreateProof` 抛错的既有失败语义保持不变——`ApplyDPoPProofHeader` **不静默跳过**，异常上抛（无 catch），与 `BuildDPoPProof` 一致；因此该能力**依赖 OpenSSL**。

守护测试：`PerRequestHeadersInjectedOnEveryPost`、`DPoPNonceChallengeRetriesOnceWithFreshProof`、`DPoPNonceRetryBudgetIsSpentOnce`（[StreamableHttpTransportTests.cpp](../../tests/unit/StreamableHttpTransportTests.cpp)）；`DPoPProofHeaderAdapterSkipsWhenDisabled`、`DPoPProofHeaderAdapterStripsQueryAndVariesPerCall`、`DPoPNonceObservationFeedsNextProof`（[OAuthTests.cpp](../../tests/unit/OAuthTests.cpp)）。

## 遗留

- `RegisterClient` 注册体仍未发送 `dpop_jkt`（见上节），DPoP 绑定的动态注册未覆盖
- conformance 的 `dpop` / `dpop-nonce` 场景夹具接线由另一任务负责（记账见 [/concepts/tier1-parity.md](/concepts/tier1-parity.md)）

## 相关页面

- [/concepts/oauth.md](/concepts/oauth.md) — 客户端授权流与服务端 Bearer 资源服务器
- [/transports/streamable-http.md](/transports/streamable-http.md) — 校验门所在的请求入口
- [/concepts/experimental-flags.md](/concepts/experimental-flags.md) — 开关两态语义
- [include/mcp/ext/DPoP.hpp](../../include/mcp/ext/DPoP.hpp) — 原语公开声明