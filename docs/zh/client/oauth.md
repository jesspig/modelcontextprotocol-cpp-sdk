# OAuth 支持

客户端支持需要认证的 MCP OAuth 授权流程。

## 流程

1. **元数据发现**：RFC 9728 受保护资源元数据（`/.well-known/oauth-protected-resource`）优先，经 `authorization_servers[0]` 定位授权服务器后再取 RFC 8414 元数据（缺 `resource` 时沿用受保护资源文档的值）；逐级失败记警告并回退——先 RFC 8414 直连，最终回退 `server_url` 硬编码默认（`issuer=server_url`、`authorize`/`token` 后缀），全程不硬失败
2. **授权码 + PKCE**，使用 S256 代码质询
3. **客户端标识**：`client_id` 为 `http(s)://` 形态时按 CIMD（Client ID Metadata Document）拉取并解析该文档；只有未配置 `client_id` 时才走**动态客户端注册**（DCR，HTTP POST 到注册端点）
4. **资源指示符**：授权请求与全部令牌请求按 RFC 8707 携带 `resource` 参数
5. **令牌刷新**，通过预过期检查提前刷新（非 401 驱动）
6. **令牌撤销**，通过手动调用 `Revoke()`（best-effort 调用 RFC 7009 撤销端点，失败不影响本地令牌清除）

## OAuthClientOptions

| 字段 | 类型 | 描述 |
|-------|------|-------------|
| `server_url` | `string` | 授权服务器基础 URL |
| `redirect_uri` | `string` | OAuth 重定向 URI |
| `client_id` | `optional<string>` | 客户端标识（未提供时自动注册） |
| `client_secret` | `optional<string>` | 客户端密钥（可选） |
| `scopes` | `vector<string>` | 请求的 OAuth 作用域 |
| `scope_filter` | `function<vector<string>(const vector<string>&)>` | 作用域发送前钩子（默认空即原样发送）：返回值替换授权 URL 与 `client_credentials` 令牌请求中的 `scope`；刷新请求不带 `scope`，落盘作用域缺失时沿用旧值，不受该钩子影响 |
| `token_cache` | `shared_ptr<ITokenCache>` | 令牌持久化（默认：`InMemoryTokenCache`） |
| `resource` | `optional<string>` | RFC 8707 资源指示符，随授权请求与令牌请求发送；缺省时回退到已发现元数据的 `resource`，再回退到 `server_url`；解析结果为空则完全不发送该参数；落盘为令牌的 audience，指示符变化时已缓存令牌视为无效 |
| `application_type` | `string` | 动态注册时发送的 `application_type`（默认 `"native"`） |
| `private_key_pem` | `optional<string>` | `private_key_jwt` 客户端认证用的 PKCS#8 EC 私钥 PEM（P-256）；与 `client_secret` 互斥，配置后不再发送 `client_secret` |
| `signing_algorithm` | `string` | 客户端断言签名算法（默认 `"ES256"`，目前仅支持该值，其他值抛 `McpError`） |
| `client_assertion_clock_seconds` | `function<int64_t()>` | 客户端断言 `iat`/`exp` 取时间的钩子（默认取系统时钟，便于测试注入固定时间） |
| `authorization_redirect_handler` | `function<void(string_view url)>` | 打开授权 URL 的回调 |
| `authorization_code_callback` | `function<optional<AuthorizationCodeResult>()>` | 返回授权码、授权服务器回显的 `state` 与可选的 `iss`（`AuthorizationCodeResult{code, state, iss}`），失败时返回 `nullopt` |

## 设置

```cpp
OAuthClientOptions oauth_opts;
oauth_opts.server_url = "https://auth.server.com";
oauth_opts.redirect_uri = "http://localhost:3000/callback";
oauth_opts.client_id = "my-client";
oauth_opts.scopes = {"profile", "email"};
oauth_opts.authorization_redirect_handler =
    [](std::string_view url) {
        // 在浏览器中打开 URL 让用户授权
    };
oauth_opts.authorization_code_callback =
    []() -> std::optional<AuthorizationCodeResult> {
        // 返回授权码、授权服务器回显的 `state`（CSRF 防护）
        // 以及授权响应携带的 `iss`（RFC 9207，未携带时留空）
        return AuthorizationCodeResult{"auth-code", "state", std::nullopt};
    };

auto auth = std::make_shared<OAuthClientProvider>(oauth_opts);
auth->Authenticate();
auto token = auth->GetAccessToken();
```

`OAuthClientProvider` 是独立类，不传递给 `McpClient::Create`。

## API

| 方法 | 描述 |
|--------|-------------|
| `Authenticate()` | 完整 OAuth 流程：发现 → 注册 → 授权 → 令牌交换（返回 `bool` 指示成败） |
| `GetAccessToken()` | 返回有效的访问令牌，将在过期前自动刷新；刷新失败抛出 `McpError`（InternalError） |
| `RefreshTokens()` | 使用存储的刷新令牌强制刷新（返回 `bool`） |
| `IsAuthenticated()` | 检查令牌是否存在且未过期 |
| `HasToken()` | 检查是否存在任何令牌（可能已过期） |
| `GetAuthorizationHeader()` | 返回 `"Bearer {token}"` 字符串 |
| `StepUpAuthorization(scopes)` | 使用额外的作用域重新授权（返回 `bool`） |
| `AuthenticateClientCredentials()` | 客户端凭据授权（RFC 6749 §4.4），用于无需用户交互的服务到服务场景（返回 `bool`） |
| `HandleAuthChallenge(www_authenticate)` | 按 RFC 9728 解析 401/403 挑战头中的 `resource_metadata` URL，发现授权服务器并重新走授权流程（返回 `bool`） |
| `Revoke()` | best-effort 调用 RFC 7009 撤销端点（配置了 `revocation_endpoint` 时），无论成败都清除本地令牌 |

## 资源指示符与发行者校验

- **RFC 8707 `resource`**：取值优先级为「显式 `options.resource` → 已发现元数据的 `resource` → `server_url`」，解析结果非空时注入授权 URL 以及三个令牌请求（`client_credentials`、授权码交换、`refresh_token`）。
- **audience 绑定**：令牌响应落盘时一并记录当前资源指示符（`TokenContainer.resource`，`FileTokenCache` 同步持久化）；指示符变化后，`Authenticate` 会先清空失配缓存，`GetAccessToken` / `IsAuthenticated` / `HasToken` / `GetAuthorizationHeader` 均按无有效令牌处理。
- **RFC 9207 `iss`**：授权响应侧在 `state` 校验通过之后、换取令牌之前做比对——`AuthorizationCodeResult::iss` 有值且与已发现元数据的 issuer 不一致即拒绝，未携带则不校验。**令牌与刷新响应侧的 `iss` 为「存在即校验」**：带字符串 `iss` 且与 issuer 不一致即拒，带非字符串 `iss` 同样拒，**不带 `iss` 则放行**。依据是 RFC 9207 §2 与 §2.4（以及 MCP 规范的 Authorization Response Validation 四态表）只对**授权响应**的 `iss` 施加义务，§2.4 明确客户端可依本地策略接受不带 `iss` 的响应；token 响应携带 `iss` 时仍须严格校验以防 mix-up 攻击。
- **发行者绑定**：`issuer` 为必填字段，缺失的授权服务器实现应补齐；当前实现缺失时记警告并继续回退链（先 RFC 8414 直连，再 `server_url` 硬编码默认），未来版本将收紧为直接失败；首次发现绑定 issuer，后续发现 issuer 变化时清空动态注册信息与本地令牌再重绑。
- **expectedResource 比对**：客户端 `VerifyResourceMatch` 要求发现元数据的 `resource` 与 `server_url` 精确相等或匹配其 scheme+host 前缀（字段缺失/空则放行）；服务端 audience 门（`validate_token_resource` 为 true 且 `expected_resource` 非空时生效）要求令牌 `resource` 非空且与 `expected_resource` 相等，否则 401 `invalid_token`；两端归一化一致（去 `#fragment`、去尾斜杠，见 `NormalizeResourceUrl`），但方向不对称——服务端缺 audience 即拒绝，客户端缺 `resource` 字段即放行。
- **客户端认证方法**：发送侧支持 `none`、`client_secret_post`（`client_secret` 随表单发送）与 `private_key_jwt`——配置 `private_key_pem` 且已发现元数据的 `token_endpoint_auth_methods_supported` 含 `private_key_jwt` 时，三个令牌请求（`client_credentials`、授权码交换、`refresh_token`）改发 `client_assertion_type=urn:ietf:params:oauth:client-assertion-type:jwt-bearer` 与 ES256 签名的 `client_assertion`，不再发送 `client_secret`（未配置 `client_secret` 时也不再因此失败）；断言 JWT 头为 `{alg:ES256,typ:JWT}`，载荷为 `{iss,sub,aud,jti,iat,exp}`（`iss`/`sub` 取 `client_id`、**`aud` 优先取元数据的 `issuer`，issuer 缺失时才回退 `token_endpoint`**——RFC 7523 §3 规定 `aud` 的确切字符串由授权服务器与断言颁发方带外约定、token endpoint URL 仅为 MAY、`exp=iat+60`）。`jwt-bearer`（授权方断言）仍不支持；`token_endpoint_auth_method` 仍仅做信息采纳（存入注册信息）。
- **CIMD（Client ID Metadata Document）**：仅 `http(s)://` 形态的 `client_id` 按该 URL 拉取并解析客户端元数据（采纳其中的 `token_endpoint_auth_method`，`grant_types`/`redirect_uris` 超 128 项、所载 `client_id` 与请求 URL 不等、已配置 `redirect_uri` 不在文档 `redirect_uris` 之列时拒绝采纳），失败属 best-effort 回退（记录警告并使用已配置的值），不会中断授权流程；动态注册发送 `application_type`（默认 `"native"`）。
- **挑战加固**：`HandleAuthChallenge` 对超长/非法输入直接返回 false（不抛异常），非法 URL 与失配资源均拒绝合并。

## 与服务端 Bearer 鉴权集成

SDK 服务端内置 RFC 6750/9728 Bearer 鉴权（`StreamableHttpServerOptions::bearer_auth`，配置详见[传输层](/zh/guide/transports)）：验证失败返回 401/403 挑战，`WWW-Authenticate` 头引用受保护资源元数据 URL，并可选在 `/.well-known/oauth-protected-resource` 公开元数据文档。`expected_resource` 非空时额外校验令牌 audience（失配 401 `invalid_token`，与 scope 缺口的 403 分流）。

客户端有两个对接点：

- `HttpClientTransportOptions::auth_challenge_handler`（见[传输层](/zh/guide/transports)）：401 必回调，`WWW-Authenticate` 含 `insufficient_scope` 的 403 才回调；返回非空 `Authorization` 头则恰好重试一次。回调内可委托 `OAuthClientProvider::HandleAuthChallenge(www_authenticate)` 解析挑战并获取新令牌。
- `HandleAuthChallenge(www_authenticate)`（返回 `bool`）：按 RFC 9728 解析挑战中的元数据 URL 以发现授权服务器，必要时重新走授权流程。

```cpp
auto auth = std::make_shared<OAuthClientProvider>(oauth_opts);
auth->Authenticate();

HttpClientTransportOptions http_opts;
http_opts.endpoint = "https://api.example.com/mcp";
http_opts.auth_challenge_handler =
    [auth](std::string_view www_authenticate) -> std::string {
        auth->HandleAuthChallenge(www_authenticate);
        return auth->GetAuthorizationHeader();
    };
```

## PKCE 辅助函数

PKCE 辅助函数位于 `<mcp/client/auth/OAuthClientProvider.hpp>` 的 `pkce` 命名空间中。

```cpp
auto verifier = pkce::GenerateCodeVerifier();
auto challenge = pkce::ComputeCodeChallenge(verifier);
auto encoded = pkce::Base64UrlEncode(input);
```

## 令牌缓存

SDK 提供了 `ITokenCache`（定义于 `<mcp/client/auth/TokenCache.hpp>`）的两种实现：

| 实现 | 持久化 | 保护 |
|----------------|-------------|------------|
| `InMemoryTokenCache` | 仅运行时 | 无 |
| `FileTokenCache`（`<mcp/storage/FileTokenCache.hpp>`） | JSON 文件（Windows 上 DPAPI 加密） | POSIX 上 `chmod 0600`，Windows 上 `CryptProtectData`（DPAPI） |

```cpp
#include <mcp/storage/FileTokenCache.hpp>

auto token_cache = std::make_shared<FileTokenCache>("./tokens.json");
OAuthClientOptions oauth_opts;
oauth_opts.token_cache = token_cache;
```

## 要求

OpenSSL 是**可选依赖**：CMake 通过 `find_package(OpenSSL QUIET)` 探测本机安装，找到后才定义 `MCP_HAVE_OPENSSL` 并链接 `OpenSSL::Crypto`/`OpenSSL::SSL`。未找到时 TLS 被禁用（`TlsSocket::Connect` 抛 `TlsHandshakeFailed`），PKCE 代码验证器生成回退到 `BCryptGenRandom`（Windows）/`std::random_device`（POSIX），SHA-256 使用内置实现——`mcp-client` 与 OAuth 功能仍可编译使用。安装 OpenSSL（`vcpkg install openssl` / `apt install libssl-dev` / `brew install openssl`）可同时为 TLS（由自研网络栈处理）、密码学级随机数与 `private_key_jwt`/DPoP 的 ECDSA 签名（ES256）提供支持；缺少 OpenSSL EC 支持时，配置了 `private_key_pem` 的 `private_key_jwt` 断言与 DPoP 证明都会抛 `McpError`（InternalError）。
