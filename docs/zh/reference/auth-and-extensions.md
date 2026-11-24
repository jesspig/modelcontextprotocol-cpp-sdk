# 授权与扩展参考

本页是授权层（OAuth、PKCE、DPoP、token 缓存）与扩展层（server card、apps、triggers、技能校验）公共符号的**唯一权威字段表**，逐条对照头文件；门面签名见 [服务端参考](/zh/reference/server) 与 [客户端参考](/zh/reference/client)，传输选项见 [传输层参考](/zh/reference/transports)，扩展开关语义见 [扩展开关概念](/zh/learn/extensions)，双时代归属的唯一权威表是 [协议版本](/zh/learn/protocol-versions)，`Skill`/`SkillResource` 等字段见 [类型总表](/zh/reference/types)。本页不含操作步骤；`detail/` 下的类型与函数不属于用户 API，只在解释行为时点名文件，不列入接口表。

## OAuth 客户端提供者

### `OAuthClientOptions` 字段

声明在 `include/mcp/client/auth/OAuthClientProvider.hpp`，实现 `src/client/auth/OAuthClientProvider.cpp`；`AuthorizationCodeResult` 含 `code`、`state`（`std::string`）与 `iss`（`std::optional<std::string>`，`nullopt`）。

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `server_url` | `std::string` | 空串 | MCP 服务端 URL，元数据发现与资源指示的基准 |
| `redirect_uri` | `std::string` | 空串 | 授权回跳地址，注册、授权 URL、码兑换三处都用 |
| `client_id` | `std::optional<std::string>` | `nullopt` | 为空且服务端广告注册端点时自动动态注册；值本身是 URL 时必须是含非根 path、无 fragment/userinfo 的 HTTPS URL 才会按客户端元数据文档去取，否则认证入口拒绝 |
| `client_secret` | `std::optional<std::string>` | `nullopt` | 仅 `client_credentials` 分支在无私钥断言时必需 |
| `scopes` | `std::vector<std::string>` | 空 | 请求范围；`StepUpAuthorization` 就地追加 |
| `scope_filter` | `std::function<std::vector<std::string>(const std::vector<std::string>&)>` | 未设置 | 每次解析范围都以 `scopes` 为入参回调；未设置即原样使用 |
| `token_cache` | `std::shared_ptr<ITokenCache>` | `nullptr` | 构造时为空则自建 `InMemoryTokenCache` |
| `resource` | `std::optional<std::string>` | `nullopt` | RFC 8707 资源指示符；未设置回退 `server_url`，与元数据 `resource` 只做逐字精确比较 |
| `application_type` | `std::string` | `"native"` | 动态注册请求体字段；空串按 `native` 送出 |
| `enable_dpop` | `bool` | `false` | 打开后 token 端点请求带 `DPoP` 头，`Authorization` 改用 `DPoP` 方案 |
| `private_key_pem` | `std::optional<std::string>` | `nullopt` | `private_key_jwt` 断言签名密钥；仅当服务端广告该认证方式才生效 |
| `signing_algorithm` | `std::string` | `"ES256"` | 断言签名算法名；非 `ES256` 时生成断言抛 `InvalidParams` |
| `client_assertion_clock_seconds` | `std::function<int64_t()>` | 未设置 | 断言 `iat`/`exp` 的时钟源；未设置用 `dpop::NowSeconds` |
| `allowed_authorization_server_origins` | `std::vector<std::string>` | 空 | 授权服务器 origin 白名单，比对 issuer 与各端点；空表示不收窄 |
| `require_iss_when_advertised` | `bool` | `true` | 服务端广告 `authorization_response_iss_parameter_supported` 时生效：默认授权响应缺 `iss`、空 `iss` 或值不匹配均在换码前失败；显式设为 `false` 仅用于兼容旧流程 |
| `require_pkce_metadata` | `bool` | `true` | 默认为真时元数据缺失 `code_challenge_methods_supported` 或未列 `S256` 即拒绝授权码流（不省略 challenge）；显式设为 `false` 才恢复旧流程 |
| `allow_legacy_metadata_fallback` | `bool` | `false` | 默认为假时 PRM/AS 元数据发现失败即拒绝；显式设为 `true` 才允许 RFC 8414 直连回退与 `server_url` 硬编码端点 |
| `authorization_redirect_handler` | `std::function<void(std::string_view url)>` | 未设置 | 拿到完整授权 URL 后回调，供外部打开浏览器；不影响流程判定 |
| `authorization_code_callback` | `std::function<std::optional<AuthorizationCodeResult>()>` | 未设置 | 必须提供，否则授权流程直接 `false`；返回空、`state` 不符、`iss` 不符均失败 |
| `token_update_handler` | `std::function<void(const TokenContainer&)>` | 未设置 | 每次令牌落缓存后回调 |
| `outbound_origin_allowlist` | `std::optional<std::vector<std::string>>` | `nullopt` | 显式追加授权 origin（scheme://host[:port]）；与 `server_url`、`redirect_uri`、`allowed_authorization_server_origins` 一同构成出站 allowlist，元数据响应不能自行扩张 |
| `loopback_port_exceptions` | `std::vector<std::pair<std::string, std::uint16_t>>` | 空 | 追加精确的回环 host+port 例外；由配置派生出的回环 origin 会自动登记对应例外，其他回环端口一律拒绝 |
| `dns_resolver` | `std::shared_ptr<mcp::detail::net::IUrlSecurityResolver>` | 空 | 出站连接前的 DNS resolver 注入点；空则用系统 `getaddrinfo`。测试用 fake resolver，不访问公网 |
| `allow_cross_origin_redirect` | `bool` | `false` | 默认拒绝跨 origin 重定向；显式打开后仍需命中 allowlist，且跨 origin 时删除 `Authorization`/`Cookie`/secret 头 |

### 出站网络安全策略（SSRF）

所有 OAuth 出站请求（PRM、AS 元数据、CIMD、DCR 注册、token、refresh、吊销、挑战元数据；JWKS 仅解析不发起请求）都走统一策略入口：

1. **origin allowlist**：只接受调用方显式配置的 scheme/host/port，按 scheme 规范化默认端口（http 80、https 443）后精确比较；拒绝 userinfo、`%` 编码 host、纯数字/十六进制编码地址、非法端口。
2. **DNS 后地址策略**：连接前解析全部 A/AAAA 答案（上限 16），逐条拒绝 loopback、private、link-local、multicast、unspecified、保留地址与 IPv4-mapped IPv6；答案中任一地址非法即整组拒绝。回环仅在精确 host+port 例外下允许。
3. **防重绑定**：把校验通过的地址集合绑定到实际连接；连接建立后在发送任何字节前以 `getpeername()` 复核 peer 是否仍在该集合中，不符即关闭连接并抛 `ProtocolViolation`。`HttpRequestSpec::peer_address_override` 是测试用的 peer 观察注入点。
4. **逐跳重定向**：每跳重新执行 scheme + origin allowlist + DNS + 地址策略；默认拒绝跨 origin，超预算（5 跳）失败关闭；跨 origin 删除 `Authorization`、`Cookie`、`client_secret`、`registration_access_token` 等敏感头。当前 HTTP 客户端无代理路径，因此不存在绕过已验证地址的代理分支。

旧行为 → 新行为 → 受影响调用方改法：以前未知 origin/私网地址可被请求，现在连接前拒绝；需要显式放行的部署把 origin 加进 `allowed_authorization_server_origins` 或 `outbound_origin_allowlist`，本地回环 fixture 用 `loopback_port_exceptions` 或由 `server_url` 自动登记。跨域重定向从「同源外放行且只删 Authorization」收紧为默认全拒绝；依赖跨域跳转的调用方需同时命中 allowlist 并显式设置 `allow_cross_origin_redirect = true`。

### DCR `client_secret` 统一解析

`ResolveClientSecret()` 是唯一解析入口，固定优先级：显式 `client_secret` → DCR 注册响应中的 `client_secret` → 按 issuer/client_id/resource 作用域隔离的既有缓存值。空值、控制字符或超过 4096 字节即失败关闭并抛 `InvalidParams`，异常与日志只写来源类别不写值；临时副本用 `detail::SecureClear` 在失败路径清零。缓存键由 `SecretCacheKey(issuer, client_id, resource)` 生成，不同 issuer 或 principal 不会复用同一 secret。

旧行为 → 新行为 → 受影响调用方改法：以前 DCR 响应里的 `client_secret` 被解析进注册信息却未用于客户端认证，现在会参与 `client_secret_post`/`client_secret_basic`；同时设置显式 secret 时仍以显式值为准。非法 secret 以前以空值发送，现在抛 `InvalidParams`；依赖空 secret 的旧注册响应需提供合法值或改走 `private_key_jwt`。

### `OAuthClientProvider` 公开方法

声明在同一头文件；私有嵌套类型与私有成员不属于用户 API，此处不列。

| 方法 | 签名 | 返回语义 | 说明 |
|---|---|---|---|
| 构造/析构 | `explicit OAuthClientProvider(OAuthClientOptions options)` / `~OAuthClientProvider()` | — | 选项按值接管；令牌缓存缺省自建 |
| `IsSecureEndpoint` | `static bool IsSecureEndpoint(std::string_view url)` | `bool` | https 判定，回环地址例外 |
| `IsSameOrigin` | `static bool IsSameOrigin(std::string_view lhs, std::string_view rhs)` | `bool` | 源（scheme+host+port）比对，任一侧取不出源即 `false` |
| `SecretCacheKey` | `static std::string SecretCacheKey(std::string_view issuer, std::string_view client_id, std::string_view resource)` | 隔离键 | 按 issuer / client_id / resource 生成 secret 缓存键 |
| `ResolveClientSecret` | `std::optional<std::string> ResolveClientSecret() const` | 解析后的 secret，无来源则 `nullopt` | 统一入口；非法值抛 `InvalidParams`，错误文本不含 secret |
| `Authenticate` | `bool Authenticate()` | 是否拿到可用令牌 | 严格发现元数据（默认无 legacy 回退）→ 按需注册/取 CIMD 文档 → 未过期缓存直接 true → 有过期令牌则刷新，`invalid_grant`/`invalid_client` 回落完整授权流；资源不符的缓存先清掉 |
| `AuthenticateClientCredentials` | `bool AuthenticateClientCredentials()` | 令牌是否入库 | 需 `client_id`；无断言时还需 `client_secret`；不发授权码流 |
| `GetAccessToken` | `std::string GetAccessToken()` | 令牌串，无令牌为空串 | 临期且有 refresh token 时走合并刷新（并发共享一次刷新）；刷新失败抛 `InternalError` |
| `RefreshTokens` | `bool RefreshTokens()` | 是否刷新成功 | 与 `GetAccessToken` / `Authenticate` 共用同一个 single-flight；`invalid_grant` 清令牌并返回 `false`，瞬时失败保留缓存并等待者读最新令牌 |
| `Revoke` | `void Revoke()` | void | 有 revocation 端点才 POST（非 https 抛 `InvalidParams`），无论成败都清缓存 |
| `IsAuthenticated` / `HasToken` | `bool IsAuthenticated() const` / `bool HasToken() const` | `bool` | 前者要求未过期，后者只要求 access token 非空；两者都校验资源匹配 |
| `GetAuthorizationHeader` | `std::string GetAuthorizationHeader() const` | `DPoP <token>` 或 `Bearer <token>`，无令牌为空串 | 只读，不触发刷新；方案名由 `enable_dpop` 决定 |
| `BuildDPoPProof` | `std::string BuildDPoPProof(std::string_view htm, std::string_view htu, std::optional<std::string_view> access_token = std::nullopt)` | 证明 JWT 串；`enable_dpop` 为 false 时空串 | 启动时从 token cache 恢复已校验 P-256 密钥（无则懒生成并持久化），nonce 按 `htu` 的规范化 origin 与 token_type 分桶读取 |
| `ApplyDPoPProofHeader` | `void ApplyDPoPProofHeader(const HttpRequestHeaderContext& context, std::map<std::string, std::string>& headers)` | void | 按上下文的方法与 URL 造证明写入 `headers["DPoP"]`，同时把当前 nonce 桶切到该 URL / token_type；未开 dpop、缺方法或 URL 不可解析则原样不动 |
| `ObserveDPoPNonce` / `SetDPoPNonce` | `void ObserveDPoPNonce(std::string_view dpop_nonce)` / `void SetDPoPNonce(std::string nonce)` | void | 旧入口委托到当前桶（由最近一次 proof/上下文设置决定），空串或超过 1024 字节的观察值忽略；Setter 无条件覆盖且不看 dpop 开关 |
| 带上下文的 nonce 入口 | `void ObserveDPoPNonce(std::string_view url, std::string_view token_type, std::string_view dpop_nonce)` / `void SetDPoPNonceContext(std::string_view url, std::string_view token_type)` / `std::optional<std::string> GetDPoPNonce(std::string_view url, std::string_view token_type) const` | void / `optional<string>` | 显式按规范化 origin + token_type 分桶观察与读取；跨桶不复用 nonce，host 大小写与默认端口归一化 |
| `StepUpAuthorization` | `bool StepUpAuthorization(const std::vector<std::string>& additional_scopes)` | 是否重新授权成功 | 追加范围 → `Revoke()` → 重跑授权流，会清掉当前令牌 |
| `HandleAuthChallenge` | `bool HandleAuthChallenge(std::string_view www_authenticate)` | 是否据此重建了元数据 | 只认 `resource_metadata="…"`；头部超 4096 字节、URL 非 https、文档超 65536 字节、元数据 `resource` 与配置资源标识不逐字相等、授权服务器不在白名单，一律 `false` |

### PKCE 辅助与 OpenSSL 依赖边界

同一头文件的 `pkce` 命名空间：

| 符号 | 签名 | 说明 |
|---|---|---|
| `GenerateCodeVerifier` | `std::string GenerateCodeVerifier()` | 32 字节随机 → base64url；授权流程另用它生成 `state` |
| `ComputeCodeChallenge` | `std::string ComputeCodeChallenge(std::string_view code_verifier)` | `S256`：SHA-256 后 base64url |
| `Base64UrlEncode` | `std::string Base64UrlEncode(std::string_view input)` | 无填充 base64url |

| 能力 | 启用 OpenSSL | 未启用 OpenSSL |
|---|---|---|
| PKCE 随机源 | `RAND_bytes`，失败抛 `std::runtime_error` | Windows 用 `BCryptGenRandom`，其余平台逐字节取 `std::random_device` |
| PKCE 摘要 | 内置 SHA-256，不经 OpenSSL | 同左，恒为内置实现 |
| DPoP 密钥、证明、验证 | OpenSSL EC/ECDSA | 抛 `McpError`（见 DPoP 小节） |
| `private_key_jwt` 断言签名 | OpenSSL EC | 同路径抛 `McpError` |
| DPoP nonce 与断言 `jti` 随机源 | `RAND_bytes` | `std::random_device`（无 Windows 分支） |
| TLS | 启用 | 禁用 TLS |

随机源与摘要的分支在 `src/client/auth/OAuthClientProvider.cpp`，DPoP 侧在 `src/ext/DPoP.cpp`，TLS 开关在 `cmake/FetchDependencies.cmake`。

构建侧：`MCP_WITH_OPENSSL` 取 `AUTO`（默认，找到才用）/`ON`（找不到即 configure 失败）/`OFF`（强制关闭），见 `cmake/FetchDependencies.cmake`；`MCP_HAVE_OPENSSL` 宏与链接统一由 INTERFACE 目标 `mcp-openssl` 施加到 `mcp-transport`/`mcp-client`/`mcp-ext`，见 `CMakeLists.txt`；仅支持 OpenSSL 3.x/4.x，主版本不符即 configure 失败，不静默关闭，见 `cmake/OpenSslVersion.cmake`。

### 与 HTTP 客户端传输的接线点

provider 不被其他公共头引用，三个钩子由调用方在 `HttpClientTransportOptions` 里接线；本仓库示例只接了两个（`examples/conformance/client/main.cpp`），`auth_challenge_handler` 无示例接线点。

| 传输选项字段 | 对应 provider 方法 |
|---|---|
| `auth_challenge_handler` | `HandleAuthChallenge` |
| `apply_per_request_headers` | `ApplyDPoPProofHeader` |
| `observe_response_challenge` | `ObserveDPoPNonce` |

三个字段都声明在 `include/mcp/transport/StreamableHttpClientTransport.hpp`。

## DPoP

### 公开符号

`include/mcp/ext/DPoP.hpp`，命名空间 `mcp::ext::dpop`；实现 `src/ext/DPoP.cpp`。

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `EcP256Jwk` | `struct { std::string x; std::string y; std::string d; }` | 三个空串 | P-256 JWK，`d` 为私钥标量；`CreateProof` 要求三者齐备 |
| `GenerateKey` | `EcP256Jwk GenerateKey()` | — | 生成 P-256 密钥（需 OpenSSL） |
| `ValidateKey` | `bool ValidateKey(const EcP256Jwk& key)` | — | 不依赖 OpenSSL：校验 x/y/d 各为 32 字节、标量在 `[1,n-1]`、点在 P-256 曲线上且 `d·G == (x,y)`；失配返回 `false` |
| `ComputeThumbprint` | `std::string ComputeThumbprint(std::string_view x, std::string_view y)` | — | 对 `{"crv":"P-256","kty":"EC","x":…,"y":…}` 取 SHA-256 再 base64url；x 或 y 为空抛 `InvalidParams`；不依赖 OpenSSL |
| `PublicJwkJson` | `std::string PublicJwkJson(const EcP256Jwk& key)` | — | 只输出 `crv`/`kty`/`x`/`y`，不含 `d`；不依赖 OpenSSL |
| `CreateProof` | `std::string CreateProof(const EcP256Jwk& key, std::string_view htm, std::string_view htu, std::optional<std::string_view> access_token = std::nullopt, std::optional<std::string_view> nonce = std::nullopt, std::optional<int64_t> iat_override_seconds = std::nullopt)` | 三个 `nullopt` | 产出 DPoP JWT：头部 `typ=dpop+jwt`、`alg=ES256`、内嵌 `jwk`，载荷 `htm`/`htu`/`jti`/`iat`，给了 access token 就写 `ath` |
| `VerifyProof` | `bool VerifyProof(std::string_view proof, const DPoPVerifyOptions& options, JtiReplayCache& replay)` | — | 校 `typ`/`alg`、`htm`/`htu`（可用 `alternate_htu`）、`ath`、nonce、时间窗、ECDSA 与 jti 回放；不匹配返回 `false`，无 OpenSSL 时改为抛 |
| `IsNonceFresh` / `NowSeconds` | `bool IsNonceFresh(int64_t iat_seconds, int64_t now_seconds, int64_t window_seconds = 300)` / `int64_t NowSeconds()` | `300` | `iat` 晚于 now 或窗口为负即 `false`；时间源为 `system_clock` 秒；均不依赖 OpenSSL |
| `GenerateNonce` | `std::string GenerateNonce(std::size_t random_bytes = 16)` | `16`（0 视作 16，超 64 钳为 64） | base64url 随机串 |

### `DPoPVerifyOptions` 字段与 `JtiReplayCache`

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `htm` / `htu` / `access_token` | `std::string` ×3 | 空串 | 期望的方法与 URL，以及用来比对 `ath` 的 access token |
| `alternate_htu` | `std::optional<std::string>` | `nullopt` | 允许证明对上第二个 URL |
| `expected_nonce` | `std::optional<std::string>` | `nullopt` | 非空则证明必须携带同一 nonce |
| `window_seconds` | `int64_t` | `300` | `iat` 新鲜度窗口 |
| `now_seconds` | `int64_t` | `0` | `0` 表示改用 `NowSeconds()` |
| 构造 | `explicit JtiReplayCache(int64_t window_seconds = 300)` | `300`（负数钳为 0） | 每次查表顺带清掉超窗条目 |
| `CheckAndStore` / `Clear` | `bool CheckAndStore(std::string_view jti, int64_t iat_seconds, int64_t now_seconds)` / `void Clear()` | — | 空 `jti` 或已见过返回 `false`，否则记录并 `true`；`Clear` 清空回放表；两者加锁 |

### 无 OpenSSL 构建的行为

::: danger
`enable_dpop` 与 `private_key_pem` 在无 OpenSSL 的构建里**不是降级可用**：`GenerateKey`、`CreateProof`、`VerifyProof` 直接抛 `McpError(InternalError, "DPoP requires OpenSSL EC support, which is not available in this build")`，客户端断言签名走同一族 EC 例程亦然。不存在「纯内置实现跑通 DPoP」这条路。
:::

| 分组 | 行为 |
|---|---|
| `GenerateKey`、`CreateProof`、`VerifyProof` | 抛 `McpError`，无降级路径 |
| `ComputeThumbprint`、`PublicJwkJson`、`IsNonceFresh`、`NowSeconds`、`JtiReplayCache`、`GenerateNonce` | 定义在 EC 段之外，仍可调用；摘要用内置 SHA-256，随机源回退 |

两种构建写在同一个 `src/ext/DPoP.cpp`，按 `MCP_HAVE_OPENSSL` 分段编译。

## Token 缓存

### `TokenContainer` 与 `ITokenCache`

`include/mcp/client/auth/TokenCache.hpp`（授权缓存契约随 client/auth 提供），实现 `src/client/auth/OAuthClientProvider.cpp`。

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `access_token` / `refresh_token` / `scopes` | `std::string` / `std::string` / `std::vector<std::string>` | 空串/空串/空 | refresh token 为空即无法刷新 |
| `expires_at` + 判定 | `int64_t`；`bool IsExpired() const` / `bool WillExpireSoon(int64_t margin_ms = 60000) const` | `0`；`60000` | 绝对过期时刻（毫秒），默认提前 60 秒判临期 |
| `token_type` / `resource` | `std::string` / `std::optional<std::string>` | `"Bearer"` / `nullopt` | provider 用 `resource` 做资源匹配，不符即当作无令牌 |
| `StoreTokens` / `ClearTokens` | `virtual void StoreTokens(const TokenContainer&) = 0` / `virtual void ClearTokens() = 0` | — | 落令牌 / 清空 |
| `GetTokens` | `virtual std::optional<TokenContainer> GetTokens() = 0` | — | 非 const 成员 |
| `StoreBoundIssuer` | `virtual void StoreBoundIssuer(const std::string& issuer)` | 空实现（可覆写） | 按 issuer 绑定的持久锚点；provider 首次发现 issuer 即写入，之后 issuer 变更一律拒绝 |
| `GetBoundIssuer` | `virtual std::optional<std::string> GetBoundIssuer()` | `nullopt`（可覆写） | 读回绑定 issuer；默认实现表示不持久化 |
| DPoP 密钥扩展 | `virtual void StoreDpopKey(const ext::dpop::EcP256Jwk& key)` / `virtual std::optional<ext::dpop::EcP256Jwk> GetDpopKey()` / `virtual void ClearDpopKey()` | 空实现 / `nullopt` / 空实现（均可覆写） | 三个方法都有默认实现，旧自定义 `ITokenCache` 无需改动；provider 重启时优先恢复这里的可信密钥 |
| `OAuthMetadata` | `struct`：issuer、authorization/token/revocation/registration/jwks/resource 端点与四类 `*_supported` 列表；`static std::optional<OAuthMetadata> Discover(std::string_view server_url, bool allow_legacy_fallback = false, std::optional<std::string_view> expected_resource = std::nullopt)` | 端点串空、列表空 | 后四个端点与 `resource` 为 `optional`；PRM 必须提供与期望资源标识逐字相等的 `resource`，否则不采用；RFC 8414 直连回退默认关闭 |
| `OAuthTokenResponse` | `struct`：`access_token`/`refresh_token`/`expires_in`/`token_type`/`scope` | `expires_in = 3600`、`token_type = "Bearer"` | 令牌端点响应载荷 |
| `ClientRegistrationInfo::Register` | `static std::optional<ClientRegistrationInfo> Register(std::string_view registration_endpoint, std::string_view redirect_uri, std::string_view client_name, std::string_view application_type = "native")` | `"native"` | DCR 出口 |

### 内置实现

| 符号 | 签名/类型 | 说明 |
|---|---|---|
| `InMemoryTokenCache` | `class : public ITokenCache` | 进程内实现，五个接口全覆写、含 issuer 绑定，内部加锁 |
| `FileTokenCache` 构造/析构 | `explicit FileTokenCache(std::filesystem::path cache_path)` / `~FileTokenCache() override` | 构造即读盘（文件不存在视作空缓存）；析构落盘失败只记 Error 日志，不外抛 |
| `StoreTokens` / `GetTokens` / `ClearTokens` | `override` | 每次写入同步落盘；令牌为空时写盘即删除文件 |
| `StoreBoundIssuer` / `GetBoundIssuer` | `override` | 锚点写在 `<cache_path>.issuer`；传空串删除该文件，写失败记 Error；读取时拒绝符号链接/非普通文件/越权权限/超 4096 字节/控制字符/短读，失败只返回 `nullopt`，不清令牌也不删锚点 |
| DPoP 密钥持久化 | `StoreDpopKey` / `GetDpopKey` / `ClearDpopKey` `override` | 与令牌写入同一份 JSON/dpapi 文件；加载时完整校验 x/y/d 曲线与公私钥匹配，损坏即拒载且后续写入保留原始字段不覆盖；Windows 同样经 DPAPI |
| 权限语义（POSIX） | 加载前 `stat`：属主须为当前 uid 且 group/other 权限位必须全空，否则拒载并记 Error | 拒载等价空缓存，不抛异常 |
| 权限语义（Windows） | 整份缓存经 DPAPI（`CryptProtectData`/`CryptUnprotectData`）按当前用户加解密；解密失败忽略缓存 | 磁盘上不是明文 JSON；落盘走内部原子安全写（仅属主可访问） |

`InMemoryTokenCache` 与 `TokenContainer` 在同一头文件；`FileTokenCache` 声明在 `include/mcp/storage/FileTokenCache.hpp`，落盘与权限判定在 `src/client/FileTokenCache.cpp`。

## 扩展类型

### 扩展与标识常量

| 常量 | 值 | 含义 |
|---|---|---|
| `ext::kServerCardExtensionId` | `"io.modelcontextprotocol/server-card"` | server card 扩展 ID |
| `ext::kAppsExtensionId` | `"io.modelcontextprotocol/ui"` | apps 扩展 ID |
| `ext::kAppsMimeType` | `"text/html;profile=mcp-app"` | ui 资源 MIME |
| `ext::kAppsUriScheme` | `"ui://"` | ui 资源 uri 前缀，`ValidateUiMeta` 与 `resources/read` 门控都按它 |
| `kTasksExtensionId` | `"io.modelcontextprotocol/tasks"` | 任务扩展 ID（能力层声明） |
| `kSkillsExtensionId` | `"io.modelcontextprotocol/skills"` | 技能扩展 ID（能力层声明） |

card 与 apps 的 ID 在 `include/mcp/ext/ServerCard.hpp` 与 `include/mcp/ext/Apps.hpp`，任务与技能的两个在 `include/mcp/Capabilities.hpp`。

::: info
triggers 没有扩展 ID 常量：对外只有通知名 `notifications/triggers/event`（`include/mcp/Methods.hpp`），服务端事件出口另要求 `enable_triggers_events`（`include/mcp/server/ServerOptions.hpp`）。
:::

### Server card

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `ServerCardEndpoint` | `struct { std::string url; std::map<std::string,std::string> headers; std::map<std::string,std::string> variables; }` | 空串/空表 | 单端点，`headers`、`variables` 原样透出 |
| `ServerCardRepository` | `struct { std::optional<std::string> url; }` | `nullopt` | 仓库地址包装；`ServerCard::repository` 却是裸串，未用该结构 |
| `ServerCard` 必填字段 | `name`、`version`（`std::string`） | 空串 | 空即校验失败 |
| `ServerCard` 可选字段 | `description`、`title`、`website_url`、`repository`、`meta`（前四个 `std::optional<std::string>`，`meta` 为 `std::optional<JsonValue>`） | 全部 `nullopt` | `description` 缺省仍写空串；`title`/`website_url`/`repository`/`meta` 有值才写出 |
| `ServerCard` 列表字段 | `icons`（`std::vector<Icon>`）、`endpoints`（`std::vector<ServerCardEndpoint>`）、`supported_protocol_versions`（`std::vector<std::string>`） | 空表 | `endpoints` 空即校验失败；`icons` 现在也随卡片写出 |
| `ValidateCard` | `void ValidateCard(const ServerCard& card)` | — | 必填、http(s) URL（拒 userinfo/畸形端口）、图标 `src`/`mimeType`/`sizes`/`theme`、端点头名与头值、变量名与值、`supportedProtocolVersions` 日期串均校验；违规抛 `InvalidParams` |
| `SerializeCard` | `JsonValue SerializeCard(const ServerCard& card)` | — | 先 `ValidateCard`；固定键 `name`/`version`/`description`/`icons`/`endpoints`/`supportedProtocolVersions`，其余可选键有值才写 |
| `ServerCardClientOptions` | `struct { std::optional<std::string> if_none_match; std::optional<ServerCard> cached_card; std::optional<std::size_t> max_response_bytes; std::chrono::milliseconds timeout{30000}; int max_redirects{5}; }` | 无条件头/无缓存、上限 1 MiB | `if_none_match` 与 `cached_card` 配套用于条件请求；304 无缓存抛 `InvalidParams` |
| `ServerCardFetchResult` | `struct { ServerCard card; std::string etag; bool from_cache{false}; }` | — | `from_cache=true` 表示命中 304 并回传调用方缓存 |
| `BuildServerCardDiscoveryUrl` | `std::string BuildServerCardDiscoveryUrl(std::string_view endpoint_url)` | — | 从 endpoint 的 scheme/host/port/path 推导 `<path>/server-card`；去 query/fragment，拒 userinfo、非 http(s)、畸形 URL |
| `IsServerCardJsonMediaType` / `ServerCardEtagMatches` | `bool`（大小写不敏感类型/参数；弱比较与列表语法、支持 `*`） | — | 供调用方自检或复用同一准入规则 |
| `CanonicalizeServerCardBody` / `ComputeServerCardEtag` | `std::string`（规范化 JSON / 带引号的 SHA-256 强 ETag） | — | 同一规范化序列化用于服务端 ETag 与客户端比对 |
| `ParseServerCard` | `ServerCard ParseServerCard(std::string_view json)` | — | 解析全部字段后调用 `ValidateCard`；非 JSON 对象、非法类型与非法字段均抛 `InvalidParams` |
| `FetchServerCard` | `ServerCardFetchResult FetchServerCard(std::string_view endpoint_url, const ServerCardClientOptions& options = {})` | — | GET 发现地址；只接受 JSON media type 与有界 body，默认仅同 origin 重定向，空 body 与错误状态抛 `McpError` |

上表客户端自由函数带 `[[nodiscard]]`；确实不需要返回值时请用 `(void)` 显式消除（如 `(void)mcp::ext::FetchServerCard(endpoint)`）。

服务端 API 声明在 `include/mcp/ext/ServerCard.hpp`，校验与序列化在 `src/ext/ServerCard.cpp`；客户端 API 声明在 `include/mcp/ext/ServerCardClient.hpp`，实现在 `src/ext/ServerCardClient.cpp`。`GetServerCard`、`ValidateCard`、`SerializeCard` 与 `GET /server-card` 均保留；序列化现在会写出已设置的可选字段，旧读取方忽略未知键即可。旧卡片若带非法可选字段，现在会被新客户端与服务端校验拒绝（旧行为/新行为/受影响方：见本节表格与上列规则）。

### Apps

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `UiMeta` | `struct { std::string resource_uri; std::vector<std::string> csp; std::vector<std::string> permissions; }` | 空串/空表/空表 | `_meta` 里的 ui 声明 |
| `ValidateUiMeta` | `void ValidateUiMeta(const UiMeta& meta)` | — | `resource_uri` 非空且必须以 `ui://` 开头，否则抛 `InvalidParams`；`csp`/`permissions` 不校验 |

声明在 `include/mcp/ext/Apps.hpp`，校验在 `src/ext/Apps.cpp`。`UiMeta`、`ValidateUiMeta`、`kAppsExtensionId`、`kAppsMimeType` 与 `kAppsUriScheme` 均未改动。

### Apps 宿主桥接

声明在 `include/mcp/ext/AppsHostBridge.hpp`，实现在 `src/ext/AppsHostBridge.cpp`。本节只覆盖宿主与沙箱 UI 之间的 JSON 消息桥，不交付 iframe、WebView、DOM、脚本注入或 UI 渲染器。

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `kAppsMessageSchemaVersion` | `std::string_view` | `"2026-07-28"` | 消息唯一受支持 schema version，畸形或缺字段一律拒绝 |
| `kAppsCapabilityTokenBytes` | `size_t` | `32` | 能力凭证随机字节数；对外为 64 位十六进制串 |
| `kDefaultAppsCapabilityTtl` | `std::chrono::milliseconds` | `60000` | 凭证默认有效期 |
| `AppsHostBridgeOptions` | `struct` | 见下 | `ui_meta`（来自 `UiMeta`，含 `resource_uri`/`csp`/`permissions`）、`allowed_origins`（宿主可信 origin 白名单）、`host_allowed_methods`（宿主显式方法白名单）、`max_message_bytes`(65536)、`max_depth`(32)、`max_nodes`(10000)、`max_string_bytes`(65536)、`max_id_bytes`(128)、`max_method_bytes`(128)、`capability_ttl`(60s)、`single_use_sensitive_methods`(`true`) |
| `AppsPostMessage` | `struct` | schema version 预置、id 空、method 空、params 空 JSON | 只承载 JSON 值、schema version、可选 `request_id`、`method` 与 `params`；无函数/DOM/可执行代码字段 |
| `AppsMessageContext` | `struct` | `sandbox_verified=false`，其余空串 | 宿主可信上下文：`source_origin`、`target_origin`、`resource_uri`、`sandbox_verified` 与 `capability`，全部由宿主填入 |
| `AppsSandboxPolicy` | `struct` | 五个禁用标志均 `true`，`csp` 空 | 暴露 opaque origin、禁 `allow-same-origin`、禁顶层导航/弹窗/表单/下载；`csp` 逐项来自 `UiMeta` |
| `AppsHostBridge::Create` | `static std::unique_ptr<AppsHostBridge> Create(AppsHostBridgeOptions)` | — | 校验 `UiMeta`、预算与 origin/方法合法性；有效权限 = `UiMeta::permissions` ∩ `host_allowed_methods`，空即拒绝一切方法 |
| `SerializeHostMessage` | `std::string SerializeHostMessage(const AppsPostMessage&, const AppsMessageContext&) const` | — | 宿主→应用序列化；仍校验可信上下文、schema version、方法权限与消息预算 |
| `ParseAppMessage` | `AppsPostMessage ParseAppMessage(std::string_view, const AppsMessageContext&) const` | — | 应用→宿主解析；先查总字节再解析，逐项校验 origin、sandbox、凭证、schema、方法与 id |
| `IsMethodAllowed` | `bool IsMethodAllowed(std::string_view, const AppsMessageContext&) const` | — | 只读判定；未知方法、越权、过期/已用凭证均返回 `false` |
| `CreateCapability` | `std::string CreateCapability(const AppsMessageContext&) const` | — | 签发绑定 `resource_uri`、allowed origins、有效权限、过期时间、消息字节上限与单次使用标志的不透明凭证 |
| `GetSandboxPolicy` | `AppsSandboxPolicy GetSandboxPolicy() const` | — | 返回固定沙箱禁用标志与来自 `UiMeta` 的 CSP |
| `NormalizeOrigin` | `bool NormalizeOrigin(std::string_view, std::string&)` | — | 小写 scheme/host，补默认端口（http 80、https 443）后输出 `scheme://host:port`；拒绝 `*`、`null`、userinfo、路径、通配符与非 http(s) |
| `IsAppsSensitiveMethod` | `bool IsAppsSensitiveMethod(std::string_view)` | — | `tools/call` 与 `resources/read` 为敏感方法，默认不在权限内即拒绝，且要求单次使用凭证 |

信任边界：

- `source_origin`、`target_origin`、`resource_uri`、`sandbox_verified` 只从 `AppsMessageContext` 读取；payload 中同名或 `permissions`/`capability` 字段属于未知顶层键，直接拒绝，不参与任何授权决策。
- origin 规范化后按 scheme/host/有效端口精确比较，默认端口省略与显式写出等价；目标为 `*`、opaque origin、跨白名单 origin、未证明 sandbox 一律拒绝。
- 有效权限逐项取 `UiMeta::permissions` 与 `host_allowed_methods` 交集；敏感方法默认拒绝，`single_use_sensitive_methods=true` 时凭证单次使用；凭证 32 字节随机、常量时间比较，绑定 resource/origin/权限/过期/字节上限；过期、重放、跨上下文或权限变更后一律拒绝。
- JSON 解析先查总字节，再限制深度/节点/单串字节；重复 key、非有限数字、超长 id/method、畸形 schema version 均抛 `McpError`，错误信息只含固定分类器，不回显 payload 内容。

::: warning 旧行为 / 新行为 / 受影响怎么改
- 旧行为：0.3.13 之前没有 Apps 宿主桥接 API，宿主自行拼接 postMessage，origin、sandbox 与权限无统一强制点。
- 新行为：新增上述 API，默认失败关闭；任何未证明 sandbox、未列入白名单的 origin、未知方法或未签发凭证的消息均被拒绝。
- 受影响怎么改：原来自行拼装 postMessage 的宿主改为由宿主侧填入 `AppsMessageContext`（不得从消息 payload 取值），调用 `CreateCapability` 签发凭证，再用 `SerializeHostMessage`/`ParseAppMessage` 走桥；现有 `UiMeta`/`ValidateUiMeta`/Apps MIME 与 URI 常量以及服务端 `ui://` 资源读门不受影响，无需迁移。
:::

### Triggers

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `TriggerRegistration` | `struct { std::vector<std::string> types; std::string webhook_url; }` | 空表/空串 | 一个 id 对应的订阅类型与回调地址 |
| `TriggerRegistry::Register` | `void Register(const std::string& id, std::vector<std::string> types, const std::string& webhook_url)` | — | id 空或 types 空抛 `InvalidParams`；同 id 覆盖 |
| `Unregister` / `Has` / `Empty` / `Size` | `bool Unregister(const std::string&)` / `bool Has(const std::string&) const` / `bool Empty() const` / `std::size_t Size() const` | — | 全部加锁；`Unregister` 返回是否真删 |
| `Match` | `std::vector<std::pair<std::string, TriggerRegistration>> Match(const std::string& type) const` | — | 按订阅类型挑出全部登记项 |
| `SerializeTriggerEvent` | `JsonValue SerializeTriggerEvent(const std::string& type, const JsonValue& data)` | — | 固定两键 `{"type":…,"data":…}` |
| `PostTriggerWebhook` | `void PostTriggerWebhook(const std::string& webhook_url, const std::string& body, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))` | `5000ms` | POST JSON，最多 3 次，退避自 100ms 起翻倍；全失败只记 Warning（URL 与错误串脱敏），不抛 |

声明在 `include/mcp/ext/Triggers.hpp`，注册表与 webhook 投递在 `src/ext/Triggers.cpp`。

## 技能校验

### 常量与函数

`include/mcp/client/SkillVerify.hpp`，实现 `src/client/SkillVerify.cpp`。

| 符号 | 签名/类型 | 默认值 | 说明 |
|---|---|---|---|
| `kMaxSkillFiles` | `constexpr size_t` | `512` | 单技能清单文件数上限（超出报错，不截断） |
| `kMaxSkillTotalBytes` | `constexpr int64_t` | `16 * 1024 * 1024` | 单技能累计字节上限；负 `size` 直接判错 |
| `CheckSkillLimits` | `bool CheckSkillLimits(const Skill& skill, std::string& error_out)` | — | 仅对 manifest 形态（`vector<SkillResource>`）生效；字符串形态（目录读取）直接通过 |
| `VerifyFileBytes` | `bool VerifyFileBytes(const SkillResource& expected, std::string_view bytes, std::string& error_out)` | — | 字节数必须相等，`digest` 必须是 `sha256:` 前缀（大小写不敏感）+ 十六进制摘要 |
| `VerifyFrontmatter` | `bool VerifyFrontmatter(const Skill& advertised, const JsonValue& fetched_frontmatter, std::string& error_out)` | — | 广告与取回的 frontmatter 必须深相等，且必须是对象、带字符串 `name`，`name` 还要等于 uri 推出的技能名段 |
| `IsApprovalStillValid` | `bool IsApprovalStillValid(const Skill& approved, const Skill& current)` | — | 无时间有效期：uri、frontmatter、meta、resources 四项全等才算审批仍有效，任一变化即失效 |
| `ApprovalKey` | `std::string ApprovalKey(const std::string& host_label, const Skill& skill)` | — | `host_label` + `0x1F` + `skill.uri` 拼接，供调用方做审批表主键 |

### 与服务端 `RegisterSkill` 的 uri 约定

| 约定 | 事实 |
|---|---|
| 入口 uri 形态 | 必须形如 `<scheme>://<authority>/<路径段>/SKILL.md`：以 `/SKILL.md` 结尾、含 `://`、authority 非空且 authority 后至少一段路径 |
| 违反即抛 | `RegisterSkill` 对不合形态的 uri 抛 `McpError(InvalidParams, "invalid skill uri: …")`；同 uri 重复注册亦抛 |
| frontmatter `name` 的期望值 | 客户端校验取 uri 去掉尾部 `/SKILL.md`、去掉 `scheme://` 与 authority 之后的最后一段路径作技能名，要求等于 `frontmatter.name` |
| 结论 | 注册名与校验名由同一 uri 形态决定：uri 多一层或少一层，都会让已审批技能在校验侧判为名字不符；注册与目录读取开关见 [服务端参考](/zh/reference/server) |

形态判定在 `src/server/McpServer.cpp` 的 `IsSkillEntryUri`，名字比对在 `src/client/SkillVerify.cpp`。

## 协议常量索引

时代列只做符号索引；判定条件与额外门以 [协议版本](/zh/learn/protocol-versions) 为唯一权威。方法名与通知名常量全部声明在 `include/mcp/Methods.hpp`。

### 请求方法名 `methods` 命名空间

| 常量 | 线格式值 | 时代 |
|---|---|---|
| `kInitialize` | `"initialize"` | 仅旧 |
| `kPing` | `"ping"` | 仅旧 |
| `kListTools` / `kCallTool` | `"tools/list"` / `"tools/call"` | 双时代 |
| `kListResources` / `kReadResource` | `"resources/list"` / `"resources/read"` | 双时代 |
| `kReadDirectory` | `"resources/directory/read"` | 仅现代 |
| `kListResourceTemplates` | `"resources/templates/list"` | 双时代 |
| `kSubscribeResource` / `kUnsubscribeResource` | `"resources/subscribe"` / `"resources/unsubscribe"` | 仅旧 |
| `kListPrompts` / `kGetPrompt` | `"prompts/list"` / `"prompts/get"` | 双时代 |
| `kComplete` | `"completion/complete"` | 双时代 |
| `kListRoots` | `"roots/list"` | 仅旧 |
| `kCreateMessage` | `"sampling/createMessage"` | 仅旧 |
| `kElicit` | `"elicitation/create"` | 仅旧 |
| `kDiscover` | `"server/discover"` | 仅现代 |
| `kSubscribe` | `"subscriptions/listen"` | 仅现代 |
| `kGetTask` / `kUpdateTask` / `kCancelTask` | `"tasks/get"` / `"tasks/update"` / `"tasks/cancel"` | 双时代（需任务开关） |
| `kGetTaskPayload` / `kLegacyGetTaskPayload` | 两者同为 `"tasks/result"` | 仅旧；`kLegacy*` 是同值别名 |
| `kListTasks` / `kLegacyListTasks` | 两者同为 `"tasks/list"` | 仅旧；同上 |
| `kDeleteTask` | `"tasks/delete"` | 仅现代；按 owner 校验后删除任务 |
| `kListSkills` / `kGetSkill` | `"skills/list"` / `"skills/get"` | 仅现代 |
| `kSetLoggingLevel` | `"logging/setLevel"` | 仅旧 |
| `kListExtensions` | `"server/extensions/list"` | 双时代 |
| `kExtensionMethodPrefix` | `"ext/"` | 非方法名：扩展方法名前缀 |

### 通知名 `notifications` 命名空间

| 常量 | 线格式值 | 时代 |
|---|---|---|
| `kInitialized` | `"notifications/initialized"` | 仅旧 |
| `kCancelled` | `"notifications/cancelled"` | 双时代 |
| `kResourceUpdated` / `kResourceListChanged` | `"notifications/resources/updated"` / `"…/list_changed"` | 双时代 |
| `kToolListChanged` / `kPromptListChanged` | `"notifications/tools/list_changed"` / `"notifications/prompts/list_changed"` | 双时代 |
| `kMessage` / `kProgress` | `"notifications/message"` / `"notifications/progress"` | 双时代 |
| `kRootsListChanged` | `"notifications/roots/list_changed"` | 仅旧；客户端另要求协商不低于 `2025-06-18` |
| `kElicitationComplete` | `"notifications/elicitation/complete"` | 仅旧 |
| `kSubscriptionsAcknowledged` | `"notifications/subscriptions/acknowledged"` | 仅现代 |
| `kTasks` | `"notifications/tasks"` | 仅现代；`params` 是完整 `DetailedTask`，支持 `taskIds` 订阅过滤 |
| `kTaskStatus` / `kTaskWorking` / `kTaskCompleted` / `kTaskFailed` / `kTaskCancelled` / `kTaskInputRequired` | `"notifications/tasks/status"`、`"…/working"`、`"…/completed"`、`"…/failed"`、`"…/cancelled"`、`"…/input_required"` | 仅旧；现代不再发送或接收这些旧通知 |
| `kTriggersEvent` | `"notifications/triggers/event"` | 双时代 |

### 协议版本常量

| 常量 | 值 | 说明 |
|---|---|---|
| `kLatestProtocolVersion` | `"2026-07-28"` | 支持表最新项 |
| `kLegacyProtocolVersion` | `"2025-11-25"` | 旧时代代表版本，`initialize` 协商的回落目标 |
| `kDefaultNegotiatedProtocolVersion` | `"2025-03-26"` | 对端未声明版本时的默认协商值 |
| `kModernProtocolVersion` | `"2026-07-28"` | 现代时代基准值 |
| `kJsonRpcVersion` | `"2.0"` | 信封版本 |
| `kProtocolVersions` | `{"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28"}` | 支持的五个版本，字典序即时间序 |
| `IsModernProtocolVersion` | `constexpr bool IsModernProtocolVersion(std::string_view version) noexcept` | 字典序 `>= kLatestProtocolVersion`（判据不是 `kModernProtocolVersion`，当前两者同值） |

全部声明在 `include/mcp/ProtocolVersion.hpp`。

相关任务写法：[/zh/howto/add-oauth-to-a-client](/zh/howto/add-oauth-to-a-client)、[/zh/howto/protect-an-http-server](/zh/howto/protect-an-http-server)。

## 本页对应源码

- `include/mcp/client/auth/OAuthClientProvider.hpp`、`include/mcp/client/auth/TokenCache.hpp`、`include/mcp/storage/FileTokenCache.hpp`、`include/mcp/client/SkillVerify.hpp`
- `include/mcp/ext/DPoP.hpp`、`include/mcp/ext/ServerCard.hpp`、`include/mcp/ext/ServerCardClient.hpp`、`include/mcp/ext/Apps.hpp`、`include/mcp/ext/AppsHostBridge.hpp`、`include/mcp/ext/Triggers.hpp`、`include/mcp/Capabilities.hpp`
- `include/mcp/Methods.hpp`、`include/mcp/ProtocolVersion.hpp`、`include/mcp/McpTypes.hpp`、`include/mcp/transport/StreamableHttpClientTransport.hpp`、`include/mcp/server/ServerOptions.hpp`
- `src/client/auth/OAuthClientProvider.cpp`、`src/client/FileTokenCache.cpp`、`src/client/SkillVerify.cpp`、`src/ext/DPoP.cpp`、`src/ext/ServerCard.cpp`、`src/ext/ServerCardClient.cpp`、`src/ext/Apps.cpp`、`src/ext/AppsHostBridge.cpp`、`src/ext/Triggers.cpp`、`src/server/McpServer.cpp`
- `cmake/FetchDependencies.cmake`、`cmake/OpenSslVersion.cmake`、`CMakeLists.txt`
