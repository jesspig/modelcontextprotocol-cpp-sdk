# OAuth 支持

客户端支持需要认证的 MCP OAuth 授权流程。

## 流程

1. **授权码 + PKCE**，使用 S256 代码质询
2. **客户端标识**：`client_id` 为 URL 时按 CIMD（Client ID Metadata Document）拉取并解析该文档；只有未配置 `client_id` 时才走**动态客户端注册**（DCR，HTTP POST 到注册端点）
3. **资源指示符**：授权请求与全部令牌请求按 RFC 8707 携带 `resource` 参数
4. **令牌刷新**，通过预过期检查提前刷新（非 401 驱动）
5. **令牌撤销**，通过手动调用 `Revoke()`（best-effort 调用 RFC 7009 撤销端点，失败不影响本地令牌清除）

## OAuthClientOptions

| 字段 | 类型 | 描述 |
|-------|------|-------------|
| `server_url` | `string` | 授权服务器基础 URL |
| `redirect_uri` | `string` | OAuth 重定向 URI |
| `client_id` | `optional<string>` | 客户端标识（未提供时自动注册） |
| `client_secret` | `optional<string>` | 客户端密钥（可选） |
| `scopes` | `vector<string>` | 请求的 OAuth 作用域 |
| `token_cache` | `shared_ptr<ITokenCache>` | 令牌持久化（默认：`InMemoryTokenCache`） |
| `resource` | `optional<string>` | RFC 8707 资源指示符，随授权请求与令牌请求发送；缺省时回退到已发现元数据的 `resource`，再回退到 `server_url`；解析结果为空则完全不发送该参数 |
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
- **RFC 9207 `iss`**：令牌与刷新响应中的 `iss` 必须存在且与已发现元数据的 issuer 一致，否则拒绝；授权响应侧在 `state` 校验通过之后、换取令牌之前做同样比对——`AuthorizationCodeResult::iss` 有值且与 issuer 不一致即拒绝，未携带则不校验。
- **CIMD（Client ID Metadata Document）**：`client_id` 为 URL 时按该 URL 拉取并解析客户端元数据，失败属 best-effort 回退（记录警告并使用已配置的值），不会中断授权流程。

## 与服务端 Bearer 鉴权集成

SDK 服务端内置 RFC 6750/9728 Bearer 鉴权（`StreamableHttpServerOptions::bearer_auth`，配置详见[传输层](/zh/guide/transports)）：验证失败返回 401/403 挑战，`WWW-Authenticate` 头引用受保护资源元数据 URL，并可选在 `/.well-known/oauth-protected-resource` 公开元数据文档。

客户端有两个对接点：

- `HttpClientTransportOptions::auth_challenge_handler`（见[传输层](/zh/guide/transports)）：收到 401/403 的 `WWW-Authenticate` 时回调；返回非空 `Authorization` 头则恰好重试一次。回调内可委托 `OAuthClientProvider::HandleAuthChallenge(www_authenticate)` 解析挑战并获取新令牌。
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

OpenSSL 是**可选依赖**：CMake 通过 `find_package(OpenSSL QUIET)` 探测本机安装，找到后才定义 `MCP_HAVE_OPENSSL` 并链接 `OpenSSL::Crypto`/`OpenSSL::SSL`。未找到时 TLS 被禁用（`TlsSocket::Connect` 抛 `TlsHandshakeFailed`），PKCE 代码验证器生成回退到 `BCryptGenRandom`（Windows）/`std::random_device`（POSIX），SHA-256 使用内置实现——`mcp-client` 与 OAuth 功能仍可编译使用。安装 OpenSSL（`vcpkg install openssl` / `apt install libssl-dev` / `brew install openssl`）可同时为 TLS（由自研网络栈处理）和密码学级随机数提供支持。
