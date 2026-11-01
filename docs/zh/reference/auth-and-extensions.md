# 授权与扩展参考

本页是授权层（OAuth、PKCE、DPoP、token 缓存）与扩展层（server card、apps、triggers、技能校验）公共符号的**唯一权威字段表**，逐条对照头文件；门面签名见 [服务端参考](/zh/reference/server) 与 [客户端参考](/zh/reference/client)，传输选项见 [传输层参考](/zh/reference/transports)，扩展开关语义见 [扩展开关概念](/zh/learn/extensions)，双时代归属的唯一权威表是 [协议版本](/zh/learn/protocol-versions)，`Skill`/`SkillResource` 等字段见 [类型总表](/zh/reference/types)。本页不含操作步骤；`detail/` 下的类型与函数不属于用户 API，只在解释行为时点名文件，不列入接口表。证据列 `hpp` 指该小节首行点名的头文件，`cpp` 指其同名实现文件。

## OAuth 客户端提供者

### `OAuthClientOptions` 字段

`include/mcp/client/auth/OAuthClientProvider.hpp:28-47`，实现 `src/client/auth/OAuthClientProvider.cpp`；`AuthorizationCodeResult`（`hpp:22-26`）含 `code`、`state`（`std::string`）与 `iss`（`std::optional<std::string>`，`nullopt`）。

| 字段 | 类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `server_url` | `std::string` | 空串 | MCP 服务端 URL，元数据发现与资源指示的基准 | `hpp:29`、`cpp:459-463` |
| `redirect_uri` | `std::string` | 空串 | 授权回跳地址，注册、授权 URL、码兑换三处都用 | `hpp:30`、`cpp:780`、`cpp:802`、`cpp:841` |
| `client_id` | `std::optional<std::string>` | `nullopt` | 为空且服务端广告注册端点时自动动态注册；值本身是 http(s) URL 时按客户端元数据文档去取 | `hpp:31`、`cpp:401-410`、`cpp:771-780` |
| `client_secret` | `std::optional<std::string>` | `nullopt` | 仅 `client_credentials` 分支在无私钥断言时必需 | `hpp:32`、`cpp:446-450` |
| `scopes` | `std::vector<std::string>` | 空 | 请求范围；`StepUpAuthorization` 就地追加 | `hpp:33`、`cpp:1067-1077` |
| `scope_filter` | `std::function<std::vector<std::string>(const std::vector<std::string>&)>` | 未设置 | 每次解析范围都以 `scopes` 为入参回调；未设置即原样使用 | `hpp:34`、`cpp:642-645` |
| `token_cache` | `std::shared_ptr<ITokenCache>` | `nullopt` | 构造时为空则自建 `InMemoryTokenCache` | `hpp:35`、`cpp:389-394` |
| `resource` | `std::optional<std::string>` | `nullopt` | RFC 8707 资源指示符；未设置回退 `server_url` 的规范化值 | `hpp:36`、`cpp:634-641` |
| `application_type` | `std::string` | `"native"` | 动态注册请求体字段；空串按 `native` 送出 | `hpp:37`、`cpp:775-780`、`cpp:1259-1260` |
| `enable_dpop` | `bool` | `false` | 打开后 token 端点请求带 `DPoP` 头，`Authorization` 改用 `DPoP` 方案 | `hpp:38`、`cpp:366-368`、`cpp:1022` |
| `private_key_pem` | `std::optional<std::string>` | `nullopt` | `private_key_jwt` 断言签名密钥；仅当服务端广告该认证方式才生效 | `hpp:39`、`cpp:711-723` |
| `signing_algorithm` | `std::string` | `"ES256"` | 断言签名算法名；非 `ES256` 时生成断言抛 `InvalidParams` | `hpp:40`、`cpp:726-728` |
| `client_assertion_clock_seconds` | `std::function<int64_t()>` | 未设置 | 断言 `iat`/`exp` 的时钟源；未设置用 `dpop::NowSeconds` | `hpp:41`、`cpp:749-751` |
| `allowed_authorization_server_origins` | `std::vector<std::string>` | 空 | 授权服务器 origin 白名单，比对 issuer 与各端点；空表示不收窄 | `hpp:42`、`cpp:555-584` |
| `authorization_redirect_handler` | `std::function<void(std::string_view url)>` | 未设置 | 拿到完整授权 URL 后回调，供外部打开浏览器；不影响流程判定 | `hpp:44`、`cpp:818-819` |
| `authorization_code_callback` | `std::function<std::optional<AuthorizationCodeResult>()>` | 未设置 | 必须提供，否则授权流程直接 `false`；返回空、`state` 不符、`iss` 不符均失败 | `hpp:45`、`cpp:820-829` |
| `token_update_handler` | `std::function<void(const TokenContainer&)>` | 未设置 | 每次令牌落缓存后回调 | `hpp:46`、`cpp:625-632` |

### `OAuthClientProvider` 公开方法

同一头文件 `hpp:49-86`；私有嵌套类型与私有成员不属于用户 API，此处不列。

| 方法 | 签名 | 返回语义 | 说明 | 证据 |
|---|---|---|---|---|
| 构造/析构 | `explicit OAuthClientProvider(OAuthClientOptions options)` / `~OAuthClientProvider()` | — | 选项按值接管；令牌缓存缺省自建 | `hpp:51-52`、`cpp:389-396` |
| `IsSecureEndpoint` | `static bool IsSecureEndpoint(std::string_view url)` | `bool` | https 判定，回环地址例外 | `hpp:54`、`cpp:586-588` |
| `IsSameOrigin` | `static bool IsSameOrigin(std::string_view lhs, std::string_view rhs)` | `bool` | 源（scheme+host+port）比对，任一侧取不出源即 `false` | `hpp:56`、`cpp:590-594` |
| `Authenticate` | `bool Authenticate()` | 是否拿到可用令牌 | 发现元数据 → 按需注册/取文档 → 未过期缓存直接 true → 有过期令牌则刷新，`invalid_grant`/`invalid_client` 回落完整授权流；资源不符的缓存先清掉 | `hpp:58`、`cpp:398-424` |
| `AuthenticateClientCredentials` | `bool AuthenticateClientCredentials()` | 令牌是否入库 | 需 `client_id`；无断言时还需 `client_secret`；不发授权码流 | `hpp:60`、`cpp:426-457` |
| `GetAccessToken` | `std::string GetAccessToken()` | 令牌串，无令牌为空串 | 临期且有 refresh token 时走合并刷新（并发共享一次刷新）；刷新失败抛 `InternalError` | `hpp:62`、`cpp:987-996`、`cpp:960-985` |
| `RefreshTokens` | `bool RefreshTokens()` | 是否刷新成功 | 直接刷新，不合并；`invalid_grant` 与临时失败都返回 `false` | `hpp:64`、`cpp:886-888` |
| `Revoke` | `void Revoke()` | void | 有 revocation 端点才 POST（非 https 抛 `InvalidParams`），无论成败都清缓存 | `hpp:66`、`cpp:998-1009`、`cpp:533-542` |
| `IsAuthenticated` / `HasToken` | `bool IsAuthenticated() const` / `bool HasToken() const` | `bool` | 前者要求未过期，后者只要求 access token 非空；两者都校验资源匹配 | `hpp:68-69`、`cpp:1010-1016` |
| `GetAuthorizationHeader` | `std::string GetAuthorizationHeader() const` | `DPoP <token>` 或 `Bearer <token>`，无令牌为空串 | 只读，不触发刷新；方案名由 `enable_dpop` 决定 | `hpp:71`、`cpp:1018-1024` |
| `BuildDPoPProof` | `std::string BuildDPoPProof(std::string_view htm, std::string_view htu, std::optional<std::string_view> access_token = std::nullopt)` | 证明 JWT 串；`enable_dpop` 为 false 时空串 | 首次调用懒生成 P-256 密钥，并带上已记录的 nonce | `hpp:73-75`、`cpp:1026-1036` |
| `ApplyDPoPProofHeader` | `void ApplyDPoPProofHeader(const HttpRequestHeaderContext& context, std::map<std::string, std::string>& headers)` | void | 按上下文的方法与 URL 造证明写入 `headers["DPoP"]`；未开 dpop、缺方法或 URL 不可解析则原样不动 | `hpp:77-78`、`cpp:1043-1058` |
| `ObserveDPoPNonce` / `SetDPoPNonce` | `void ObserveDPoPNonce(std::string_view dpop_nonce)` / `void SetDPoPNonce(std::string nonce)` | void | 前者空串或超过 1024 字节忽略；后者无条件覆盖，不看 dpop 开关 | `hpp:80-82`、`cpp:1061-1065`、`cpp:1038-1041` |
| `StepUpAuthorization` | `bool StepUpAuthorization(const std::vector<std::string>& additional_scopes)` | 是否重新授权成功 | 追加范围 → `Revoke()` → 重跑授权流，会清掉当前令牌 | `hpp:84`、`cpp:1067-1081` |
| `HandleAuthChallenge` | `bool HandleAuthChallenge(std::string_view www_authenticate)` | 是否据此重建了元数据 | 只认 `resource_metadata="…"`；头部超 4096 字节、URL 非 https、文档超 65536 字节、资源不符、授权服务器不在白名单，一律 `false` | `hpp:86`、`cpp:1083-1160` |

### PKCE 辅助与 OpenSSL 依赖边界

`hpp:180-188` 的 `pkce` 命名空间：

| 符号 | 签名 | 说明 | 证据 |
|---|---|---|---|
| `GenerateCodeVerifier` | `std::string GenerateCodeVerifier()` | 32 字节随机 → base64url；授权流程另用它生成 `state` | `hpp:182`、`cpp:289-309`、`cpp:793-795` |
| `ComputeCodeChallenge` | `std::string ComputeCodeChallenge(std::string_view code_verifier)` | `S256`：SHA-256 后 base64url | `hpp:184`、`cpp:311-314` |
| `Base64UrlEncode` | `std::string Base64UrlEncode(std::string_view input)` | 无填充 base64url | `hpp:186`、`cpp:285-287` |

| 能力 | 启用 OpenSSL | 未启用 OpenSSL | 证据 |
|---|---|---|---|
| PKCE 随机源 | `RAND_bytes`，失败抛 `std::runtime_error` | Windows 用 `BCryptGenRandom`，其余平台逐字节取 `std::random_device` | `src/client/auth/OAuthClientProvider.cpp:291-305` |
| PKCE 摘要 | 内置 SHA-256，不经 OpenSSL | 同左，恒为内置实现 | `src/client/auth/OAuthClientProvider.cpp:311-314` |
| DPoP 密钥、证明、验证 | OpenSSL EC/ECDSA | 抛 `McpError`（见 DPoP 小节） | `src/ext/DPoP.cpp:192-537`、`538-573` |
| `private_key_jwt` 断言签名 | OpenSSL EC | 同路径抛 `McpError` | `src/client/auth/OAuthClientProvider.cpp:725-761`、`src/ext/DPoP.cpp:562-571` |
| DPoP nonce 与断言 `jti` 随机源 | `RAND_bytes` | `std::random_device`（无 Windows 分支） | `src/ext/DPoP.cpp:147-160` |
| TLS | 启用 | 禁用 TLS | `cmake/FetchDependencies.cmake:15-40` |

构建侧：`MCP_WITH_OPENSSL` 取 `AUTO`（默认，找到才用）/`ON`（找不到即 configure 失败）/`OFF`（强制关闭）（`cmake/FetchDependencies.cmake:7-16`、`27-38`）；`MCP_HAVE_OPENSSL` 宏与链接统一由 INTERFACE 目标 `mcp-openssl` 施加到 `mcp-transport`/`mcp-client`/`mcp-ext`（`CMakeLists.txt:227-238`）；仅支持 OpenSSL 3.x/4.x，主版本不符即 configure 失败，不静默关闭（`cmake/OpenSslVersion.cmake:7-11`）。

### 与 HTTP 客户端传输的接线点

provider 不被其他公共头引用，三个钩子由调用方在 `HttpClientTransportOptions` 里接线；本仓库示例只接了两个（`examples/conformance/client/main.cpp:1289-1297`），`auth_challenge_handler` 无示例接线点。

| 传输选项字段 | 对应 provider 方法 | 证据 |
|---|---|---|
| `auth_challenge_handler` | `HandleAuthChallenge` | `include/mcp/transport/StreamableHttpClientTransport.hpp:50` |
| `apply_per_request_headers` | `ApplyDPoPProofHeader` | `StreamableHttpClientTransport.hpp:51-53` |
| `observe_response_challenge` | `ObserveDPoPNonce` | `StreamableHttpClientTransport.hpp:54-55` |

## DPoP

### 公开符号

`include/mcp/ext/DPoP.hpp`，命名空间 `mcp::ext::dpop`；实现 `src/ext/DPoP.cpp`。

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `EcP256Jwk` | `struct { std::string x; std::string y; std::string d; }` | 三个空串 | P-256 JWK，`d` 为私钥标量；`CreateProof` 要求三者齐备 | `hpp:12-16`、`cpp:385-395` |
| `GenerateKey` | `EcP256Jwk GenerateKey()` | — | 生成 P-256 密钥 | `hpp:18`、`cpp:349-378` |
| `ComputeThumbprint` | `std::string ComputeThumbprint(std::string_view x, std::string_view y)` | — | 对 `{"crv":"P-256","kty":"EC","x":…,"y":…}` 取 SHA-256 再 base64url；x 或 y 为空抛 `InvalidParams`；不依赖 OpenSSL | `hpp:20`、`cpp:96-101` |
| `PublicJwkJson` | `std::string PublicJwkJson(const EcP256Jwk& key)` | — | 只输出 `crv`/`kty`/`x`/`y`，不含 `d`；不依赖 OpenSSL | `hpp:22`、`cpp:103-110` |
| `CreateProof` | `std::string CreateProof(const EcP256Jwk& key, std::string_view htm, std::string_view htu, std::optional<std::string_view> access_token = std::nullopt, std::optional<std::string_view> nonce = std::nullopt, std::optional<int64_t> iat_override_seconds = std::nullopt)` | 三个 `nullopt` | 产出 DPoP JWT：头部 `typ=dpop+jwt`、`alg=ES256`、内嵌 `jwk`，载荷 `htm`/`htu`/`jti`/`iat`，给了 access token 就写 `ath` | `hpp:24-29`、`cpp:380-428` |
| `VerifyProof` | `bool VerifyProof(std::string_view proof, const DPoPVerifyOptions& options, JtiReplayCache& replay)` | — | 校 `typ`/`alg`、`htm`/`htu`（可用 `alternate_htu`）、`ath`、nonce、时间窗、ECDSA 与 jti 回放；不匹配返回 `false`，无 OpenSSL 时改为抛 | `hpp:56-58`、`cpp:430-537` |
| `IsNonceFresh` / `NowSeconds` | `bool IsNonceFresh(int64_t iat_seconds, int64_t now_seconds, int64_t window_seconds = 300)` / `int64_t NowSeconds()` | `300` | `iat` 晚于 now 或窗口为负即 `false`；时间源为 `system_clock` 秒；均不依赖 OpenSSL | `hpp:31-33`、`cpp:112-122` |
| `GenerateNonce` | `std::string GenerateNonce(std::size_t random_bytes = 16)` | `16`（0 视作 16，超 64 钳为 64） | base64url 随机串 | `hpp:60`、`cpp:147-160` |

### `DPoPVerifyOptions` 字段与 `JtiReplayCache`

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `htm` / `htu` / `access_token` | `std::string` ×3 | 空串 | 期望的方法与 URL，以及用来比对 `ath` 的 access token | `hpp:36-38` |
| `alternate_htu` | `std::optional<std::string>` | `nullopt` | 允许证明对上第二个 URL | `hpp:39` |
| `expected_nonce` | `std::optional<std::string>` | `nullopt` | 非空则证明必须携带同一 nonce | `hpp:40` |
| `window_seconds` | `int64_t` | `300` | `iat` 新鲜度窗口 | `hpp:41` |
| `now_seconds` | `int64_t` | `0` | `0` 表示改用 `NowSeconds()` | `hpp:42` |
| 构造 | `explicit JtiReplayCache(int64_t window_seconds = 300)` | `300`（负数钳为 0） | 每次查表顺带清掉超窗条目 | `hpp:47`、`cpp:124-125` |
| `CheckAndStore` / `Clear` | `bool CheckAndStore(std::string_view jti, int64_t iat_seconds, int64_t now_seconds)` / `void Clear()` | — | 空 `jti` 或已见过返回 `false`，否则记录并 `true`；`Clear` 清空回放表；两者加锁 | `hpp:48-49`、`cpp:127-145` |

### 无 OpenSSL 构建的行为

::: danger
`enable_dpop` 与 `private_key_pem` 在无 OpenSSL 的构建里**不是降级可用**：`GenerateKey`、`CreateProof`、`VerifyProof` 直接抛 `McpError(InternalError, "DPoP requires OpenSSL EC support, which is not available in this build")`，客户端断言签名走同一族 EC 例程亦然。不存在「纯内置实现跑通 DPoP」这条路。
:::

| 分组 | 行为 | 证据 |
|---|---|---|
| `GenerateKey`、`CreateProof`、`VerifyProof` | 抛 `McpError`，无降级路径 | `src/ext/DPoP.cpp:540-559` |
| `ComputeThumbprint`、`PublicJwkJson`、`IsNonceFresh`、`NowSeconds`、`JtiReplayCache`、`GenerateNonce` | 定义在 EC 段之外，仍可调用；摘要用内置 SHA-256，随机源回退 | `src/ext/DPoP.cpp:96-160` |

## Token 缓存

### `TokenContainer` 与 `ITokenCache`

`include/mcp/client/auth/TokenCache.hpp`（授权缓存契约随 client/auth 提供），实现 `src/client/auth/OAuthClientProvider.cpp`。

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `access_token` / `refresh_token` / `scopes` | `std::string` / `std::string` / `std::vector<std::string>` | 空串/空串/空 | refresh token 为空即无法刷新 | `hpp:12-14` |
| `expires_at` + 判定 | `int64_t`；`bool IsExpired() const` / `bool WillExpireSoon(int64_t margin_ms = 60000) const` | `0`；`60000` | 绝对过期时刻（毫秒），默认提前 60 秒判临期 | `hpp:15`、`hpp:19-20`、`cpp:318-329` |
| `token_type` / `resource` | `std::string` / `std::optional<std::string>` | `"Bearer"` / `nullopt` | provider 用 `resource` 做资源匹配，不符即当作无令牌 | `hpp:16-17`、`cpp:509-513` |
| `StoreTokens` / `ClearTokens` | `virtual void StoreTokens(const TokenContainer&) = 0` / `virtual void ClearTokens() = 0` | — | 落令牌 / 清空 | `hpp:26`、`hpp:28` |
| `GetTokens` | `virtual std::optional<TokenContainer> GetTokens() = 0` | — | 非 const 成员 | `hpp:27` |
| `StoreBoundIssuer` | `virtual void StoreBoundIssuer(const std::string& issuer)` | 空实现（可覆写） | 按 issuer 绑定的持久锚点；provider 首次发现 issuer 即写入，之后 issuer 变更一律拒绝 | `hpp:30`、`cpp:515-531` |
| `GetBoundIssuer` | `virtual std::optional<std::string> GetBoundIssuer()` | `nullopt`（可覆写） | 读回绑定 issuer；默认实现表示不持久化 | `hpp:31` |
| `OAuthMetadata` | `struct`：issuer、authorization/token/revocation/registration/jwks/resource 端点与四类 `*_supported` 列表；`static std::optional<OAuthMetadata> Discover(std::string_view server_url)` | 端点串空、列表空 | 后四个端点与 `resource` 为 `optional`；`Discover` 取 `/.well-known/oauth-authorization-server`，失败返回空 | `hpp:47-61`、`cpp:1226` |
| `OAuthTokenResponse` | `struct`：`access_token`/`refresh_token`/`expires_in`/`token_type`/`scope` | `expires_in = 3600`、`token_type = "Bearer"` | 令牌端点响应载荷 | `hpp:64-70` |
| `ClientRegistrationInfo::Register` | `static std::optional<ClientRegistrationInfo> Register(std::string_view registration_endpoint, std::string_view redirect_uri, std::string_view client_name, std::string_view application_type = "native")` | `"native"` | DCR 出口 | `hpp:79-83` |

### 内置实现

| 符号 | 签名/类型 | 说明 | 证据 |
|---|---|---|---|
| `InMemoryTokenCache` | `class : public ITokenCache` | 进程内实现，五个接口全覆写、含 issuer 绑定，内部加锁 | `TokenCache.hpp:34-45` |
| `FileTokenCache` 构造/析构 | `explicit FileTokenCache(std::filesystem::path cache_path)` / `~FileTokenCache() override` | 构造即读盘（文件不存在视作空缓存）；析构落盘失败只记 Error 日志，不外抛 | `include/mcp/storage/FileTokenCache.hpp:13-14`、`src/client/FileTokenCache.cpp:78-90` |
| `StoreTokens` / `GetTokens` / `ClearTokens` | `override` | 每次写入同步落盘；令牌为空时写盘即删除文件 | `FileTokenCache.hpp:16-18`、`cpp:92-107`、`cpp:236-241` |
| `StoreBoundIssuer` / `GetBoundIssuer` | `override` | 锚点写在 `<cache_path>.issuer`；传空串删除该文件，写失败记 Error | `FileTokenCache.hpp:19-20`、`cpp:109-136`、`cpp:47-51` |
| 权限语义（POSIX） | 加载前 `stat`：属主须为当前 uid 且 group/other 权限位必须全空，否则拒载并记 Error | 拒载等价空缓存，不抛异常 | `cpp:53-72`、`cpp:173` |
| 权限语义（Windows） | 整份缓存经 DPAPI（`CryptProtectData`/`CryptUnprotectData`）按当前用户加解密；解密失败忽略缓存 | 磁盘上不是明文 JSON；落盘走内部原子安全写（仅属主可访问） | `cpp:19-45`、`cpp:145-152`、`cpp:221-232` |

## 扩展类型

### 扩展与标识常量

| 常量 | 值 | 含义 | 证据 |
|---|---|---|---|
| `ext::kServerCardExtensionId` | `"io.modelcontextprotocol/server-card"` | server card 扩展 ID | `include/mcp/ext/ServerCard.hpp:14` |
| `ext::kAppsExtensionId` | `"io.modelcontextprotocol/ui"` | apps 扩展 ID | `include/mcp/ext/Apps.hpp:11` |
| `ext::kAppsMimeType` | `"text/html;profile=mcp-app"` | ui 资源 MIME | `Apps.hpp:12` |
| `ext::kAppsUriScheme` | `"ui://"` | ui 资源 uri 前缀，`ValidateUiMeta` 与 `resources/read` 门控都按它 | `Apps.hpp:13` |
| `kTasksExtensionId` | `"io.modelcontextprotocol/tasks"` | 任务扩展 ID（能力层声明） | `include/mcp/Capabilities.hpp:43` |
| `kSkillsExtensionId` | `"io.modelcontextprotocol/skills"` | 技能扩展 ID（能力层声明） | `Capabilities.hpp:44` |

::: info
triggers 没有扩展 ID 常量：对外只有通知名 `notifications/triggers/event`（`include/mcp/Methods.hpp:76`），服务端事件出口另要求 `enable_triggers_events`（`include/mcp/server/ServerOptions.hpp:88`）。
:::

### Server card

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `ServerCardEndpoint` | `struct { std::string url; std::map<std::string,std::string> headers; std::map<std::string,std::string> variables; }` | 空串/空表 | 单端点，`headers`、`variables` 原样透出 | `ServerCard.hpp:16-20` |
| `ServerCardRepository` | `struct { std::optional<std::string> url; }` | `nullopt` | 仓库地址包装；`ServerCard::repository` 却是裸串，未用该结构 | `ServerCard.hpp:22-24`、`ServerCard.hpp:32` |
| `ServerCard` 必填字段 | `name`、`version`（`std::string`） | 空串 | 空即校验失败 | `ServerCard.hpp:27-28`、`src/ext/ServerCard.cpp:17-21` |
| `ServerCard` 可选字段 | `description`、`title`、`website_url`、`repository`、`meta`（前四个 `std::optional<std::string>`，`meta` 为 `std::optional<JsonValue>`） | 全部 `nullopt` | 序列化时 `description` 缺省写成空串，其余可选字段不写出 | `ServerCard.hpp:29-36`、`src/ext/ServerCard.cpp:37` |
| `ServerCard` 列表字段 | `icons`（`std::vector<Icon>`）、`endpoints`（`std::vector<ServerCardEndpoint>`）、`supported_protocol_versions`（`std::vector<std::string>`） | 空表 | `endpoints` 空即校验失败 | `ServerCard.hpp:31-35`、`src/ext/ServerCard.cpp:22-28` |
| `ValidateCard` | `void ValidateCard(const ServerCard& card)` | — | name/version/endpoints 非空、每个 endpoint url 必须 http/https；违规抛 `InvalidParams` | `ServerCard.hpp:39`、`src/ext/ServerCard.cpp:15-30` |
| `SerializeCard` | `JsonValue SerializeCard(const ServerCard& card)` | — | 先 `ValidateCard`；键为 `name`/`version`/`description`/`endpoints`/`supportedProtocolVersions` | `ServerCard.hpp:41`、`src/ext/ServerCard.cpp:32-65` |

### Apps

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `UiMeta` | `struct { std::string resource_uri; std::vector<std::string> csp; std::vector<std::string> permissions; }` | 空串/空表/空表 | `_meta` 里的 ui 声明 | `Apps.hpp:15-19` |
| `ValidateUiMeta` | `void ValidateUiMeta(const UiMeta& meta)` | — | `resource_uri` 非空且必须以 `ui://` 开头，否则抛 `InvalidParams`；`csp`/`permissions` 不校验 | `Apps.hpp:21`、`src/ext/Apps.cpp:7-15` |

### Triggers

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `TriggerRegistration` | `struct { std::vector<std::string> types; std::string webhook_url; }` | 空表/空串 | 一个 id 对应的订阅类型与回调地址 | `include/mcp/ext/Triggers.hpp:15-18` |
| `TriggerRegistry::Register` | `void Register(const std::string& id, std::vector<std::string> types, const std::string& webhook_url)` | — | id 空或 types 空抛 `InvalidParams`；同 id 覆盖 | `Triggers.hpp:22-24`、`src/ext/Triggers.cpp:47-61` |
| `Unregister` / `Has` / `Empty` / `Size` | `bool Unregister(const std::string&)` / `bool Has(const std::string&) const` / `bool Empty() const` / `std::size_t Size() const` | — | 全部加锁；`Unregister` 返回是否真删 | `Triggers.hpp:25-28`、`src/ext/Triggers.cpp:63-81` |
| `Match` | `std::vector<std::pair<std::string, TriggerRegistration>> Match(const std::string& type) const` | — | 按订阅类型挑出全部登记项 | `Triggers.hpp:29-30`、`src/ext/Triggers.cpp:83-91` |
| `SerializeTriggerEvent` | `JsonValue SerializeTriggerEvent(const std::string& type, const JsonValue& data)` | — | 固定两键 `{"type":…,"data":…}` | `Triggers.hpp:36`、`src/ext/Triggers.cpp:98-103` |
| `PostTriggerWebhook` | `void PostTriggerWebhook(const std::string& webhook_url, const std::string& body, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))` | `5000ms` | POST JSON，最多 3 次，退避自 100ms 起翻倍；全失败只记 Warning（URL 与错误串脱敏），不抛 | `Triggers.hpp:38-41`、`src/ext/Triggers.cpp:105-133` |

## 技能校验

### 常量与函数

`include/mcp/client/SkillVerify.hpp`，实现 `src/client/SkillVerify.cpp`。

| 符号 | 签名/类型 | 默认值 | 说明 | 证据 |
|---|---|---|---|---|
| `kMaxSkillFiles` | `constexpr size_t` | `512` | 单技能清单文件数上限（超出报错，不截断） | `hpp:12`、`cpp:60-63` |
| `kMaxSkillTotalBytes` | `constexpr int64_t` | `16 * 1024 * 1024` | 单技能累计字节上限；负 `size` 直接判错 | `hpp:13`、`cpp:65-77` |
| `CheckSkillLimits` | `bool CheckSkillLimits(const Skill& skill, std::string& error_out)` | — | 仅对 manifest 形态（`vector<SkillResource>`）生效；字符串形态（目录读取）直接通过 | `hpp:15`、`cpp:57-78` |
| `VerifyFileBytes` | `bool VerifyFileBytes(const SkillResource& expected, std::string_view bytes, std::string& error_out)` | — | 字节数必须相等，`digest` 必须是 `sha256:` 前缀（大小写不敏感）+ 十六进制摘要 | `hpp:16`、`cpp:79-96` |
| `VerifyFrontmatter` | `bool VerifyFrontmatter(const Skill& advertised, const JsonValue& fetched_frontmatter, std::string& error_out)` | — | 广告与取回的前马文必须深相等，且必须是对象、带字符串 `name`，`name` 还要等于 uri 推出的技能名段 | `hpp:17`、`cpp:98-117` |
| `IsApprovalStillValid` | `bool IsApprovalStillValid(const Skill& approved, const Skill& current)` | — | 无时间有效期：uri、frontmatter、meta、resources 四项全等才算审批仍有效，任一变化即失效 | `hpp:18`、`cpp:119-124` |
| `ApprovalKey` | `std::string ApprovalKey(const std::string& host_label, const Skill& skill)` | — | `host_label` + `0x1F` + `skill.uri` 拼接，供调用方做审批表主键 | `hpp:19`、`cpp:126-130` |

### 与服务端 `RegisterSkill` 的 uri 约定

| 约定 | 事实 | 证据 |
|---|---|---|
| 入口 uri 形态 | 必须形如 `<scheme>://<authority>/<路径段>/SKILL.md`：以 `/SKILL.md` 结尾、含 `://`、authority 非空且 authority 后至少一段路径 | `src/server/McpServer.cpp:125-140`、`908-913` |
| 违反即抛 | `RegisterSkill` 对不合形态的 uri 抛 `McpError(InvalidParams, "invalid skill uri: …")`；同 uri 重复注册亦抛 | `McpServer.cpp:908-925` |
| 前马文 `name` 的期望值 | 客户端校验取 uri 去掉尾部 `/SKILL.md`、去掉 `scheme://` 与 authority 之后的最后一段路径作技能名，要求等于 `frontmatter.name` | `src/client/SkillVerify.cpp:28-38`、`98-116` |
| 结论 | 注册名与校验名由同一 uri 形态决定：uri 多一层或少一层，都会让已审批技能在校验侧判为名字不符；注册与目录读取开关见 [服务端参考](/zh/reference/server) | `McpServer.cpp:129-140`、`SkillVerify.cpp:28-38` |

## 协议常量索引

时代列只做符号索引；判定条件与额外门以 [协议版本](/zh/learn/protocol-versions) 为唯一权威。

### 请求方法名 `methods` 命名空间

| 常量 | 线格式值 | 时代 | 证据 |
|---|---|---|---|
| `kInitialize` | `"initialize"` | 仅旧 | `include/mcp/Methods.hpp:8` |
| `kPing` | `"ping"` | 仅旧 | `Methods.hpp:9` |
| `kListTools` / `kCallTool` | `"tools/list"` / `"tools/call"` | 双时代 | `Methods.hpp:11-12` |
| `kListResources` / `kReadResource` | `"resources/list"` / `"resources/read"` | 双时代 | `Methods.hpp:14-15` |
| `kReadDirectory` | `"resources/directory/read"` | 仅现代 | `Methods.hpp:16` |
| `kListResourceTemplates` | `"resources/templates/list"` | 双时代 | `Methods.hpp:17` |
| `kSubscribeResource` / `kUnsubscribeResource` | `"resources/subscribe"` / `"resources/unsubscribe"` | 仅旧 | `Methods.hpp:18-19` |
| `kListPrompts` / `kGetPrompt` | `"prompts/list"` / `"prompts/get"` | 双时代 | `Methods.hpp:21-22` |
| `kComplete` | `"completion/complete"` | 双时代 | `Methods.hpp:24` |
| `kListRoots` | `"roots/list"` | 仅旧 | `Methods.hpp:26` |
| `kCreateMessage` | `"sampling/createMessage"` | 仅旧 | `Methods.hpp:28` |
| `kElicit` | `"elicitation/create"` | 仅旧 | `Methods.hpp:30` |
| `kDiscover` | `"server/discover"` | 仅现代 | `Methods.hpp:32` |
| `kSubscribe` | `"subscriptions/listen"` | 仅现代 | `Methods.hpp:34` |
| `kGetTask` / `kUpdateTask` / `kCancelTask` | `"tasks/get"` / `"tasks/update"` / `"tasks/cancel"` | 双时代（需任务开关） | `Methods.hpp:36-38` |
| `kGetTaskPayload` / `kLegacyGetTaskPayload` | 两者同为 `"tasks/result"` | 仅旧；`kLegacy*` 是同值别名 | `Methods.hpp:40`、`43` |
| `kListTasks` / `kLegacyListTasks` | 两者同为 `"tasks/list"` | 仅旧；同上 | `Methods.hpp:41`、`44` |
| `kListSkills` / `kGetSkill` | `"skills/list"` / `"skills/get"` | 仅现代 | `Methods.hpp:46-47` |
| `kSetLoggingLevel` | `"logging/setLevel"` | 仅旧 | `Methods.hpp:49` |
| `kListExtensions` | `"server/extensions/list"` | 双时代 | `Methods.hpp:51` |
| `kExtensionMethodPrefix` | `"ext/"` | 非方法名：扩展方法名前缀 | `Methods.hpp:52` |

### 通知名 `notifications` 命名空间

| 常量 | 线格式值 | 时代 | 证据 |
|---|---|---|---|
| `kInitialized` | `"notifications/initialized"` | 仅旧 | `Methods.hpp:58` |
| `kCancelled` | `"notifications/cancelled"` | 双时代 | `Methods.hpp:59` |
| `kResourceUpdated` / `kResourceListChanged` | `"notifications/resources/updated"` / `"…/list_changed"` | 双时代 | `Methods.hpp:60-61` |
| `kToolListChanged` / `kPromptListChanged` | `"notifications/tools/list_changed"` / `"notifications/prompts/list_changed"` | 双时代 | `Methods.hpp:62-63` |
| `kMessage` / `kProgress` | `"notifications/message"` / `"notifications/progress"` | 双时代 | `Methods.hpp:64`、`66` |
| `kRootsListChanged` | `"notifications/roots/list_changed"` | 仅旧；客户端另要求协商不低于 `2025-06-18` | `Methods.hpp:67` |
| `kElicitationComplete` | `"notifications/elicitation/complete"` | 仅旧 | `Methods.hpp:68` |
| `kSubscriptionsAcknowledged` | `"notifications/subscriptions/acknowledged"` | 仅现代 | `Methods.hpp:69` |
| `kTaskStatus` / `kTaskWorking` / `kTaskCompleted` / `kTaskFailed` / `kTaskCancelled` / `kTaskInputRequired` | `"notifications/tasks/status"`、`"…/working"`、`"…/completed"`、`"…/failed"`、`"…/cancelled"`、`"…/input_required"` | 双时代 | `Methods.hpp:70-75` |
| `kTriggersEvent` | `"notifications/triggers/event"` | 双时代 | `Methods.hpp:76` |

### 协议版本常量

| 常量 | 值 | 说明 | 证据 |
|---|---|---|---|
| `kLatestProtocolVersion` | `"2026-07-28"` | 支持表最新项 | `include/mcp/ProtocolVersion.hpp:7` |
| `kLegacyProtocolVersion` | `"2025-11-25"` | 旧时代代表版本，`initialize` 协商的回落目标 | `ProtocolVersion.hpp:8` |
| `kDefaultNegotiatedProtocolVersion` | `"2025-03-26"` | 对端未声明版本时的默认协商值 | `ProtocolVersion.hpp:9` |
| `kModernProtocolVersion` | `"2026-07-28"` | 现代时代基准值 | `ProtocolVersion.hpp:10` |
| `kJsonRpcVersion` | `"2.0"` | 信封版本 | `ProtocolVersion.hpp:11` |
| `kProtocolVersions` | `{"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28"}` | 支持的五个版本，字典序即时间序 | `ProtocolVersion.hpp:13-19` |
| `IsModernProtocolVersion` | `constexpr bool IsModernProtocolVersion(std::string_view version) noexcept` | 字典序 `>= kLatestProtocolVersion`（判据不是 `kModernProtocolVersion`，当前两者同值） | `ProtocolVersion.hpp:21-23` |

相关任务写法：[/zh/howto/add-oauth-to-a-client](/zh/howto/add-oauth-to-a-client)、[/zh/howto/protect-an-http-server](/zh/howto/protect-an-http-server)。

## 本页对应源码

- `include/mcp/client/auth/OAuthClientProvider.hpp`、`include/mcp/client/auth/TokenCache.hpp`、`include/mcp/storage/FileTokenCache.hpp`、`include/mcp/client/SkillVerify.hpp`
- `include/mcp/ext/DPoP.hpp`、`include/mcp/ext/ServerCard.hpp`、`include/mcp/ext/Apps.hpp`、`include/mcp/ext/Triggers.hpp`、`include/mcp/Capabilities.hpp`
- `include/mcp/Methods.hpp`、`include/mcp/ProtocolVersion.hpp`、`include/mcp/McpTypes.hpp`、`include/mcp/transport/StreamableHttpClientTransport.hpp`、`include/mcp/server/ServerOptions.hpp`
- `src/client/auth/OAuthClientProvider.cpp`、`src/client/FileTokenCache.cpp`、`src/client/SkillVerify.cpp`、`src/ext/DPoP.cpp`、`src/ext/ServerCard.cpp`、`src/ext/Apps.cpp`、`src/ext/Triggers.cpp`、`src/server/McpServer.cpp`
- `cmake/FetchDependencies.cmake`、`cmake/OpenSslVersion.cmake`、`CMakeLists.txt`
