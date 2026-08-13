# OAuth 支持

客户端支持需要认证的 MCP OAuth 授权流程。

## 流程

1. **授权码 + PKCE**，使用 S256 代码质询
2. **动态客户端注册**（DCR），用于首次使用的客户端（HTTP POST 到注册端点）
3. **令牌刷新**，通过预过期检查提前刷新（非 401 驱动）
4. **令牌撤销**，通过手动调用 `Revoke()`（best-effort 调用 RFC 7009 撤销端点，失败不影响本地令牌清除）

## OAuthClientOptions

| 字段 | 类型 | 描述 |
|-------|------|-------------|
| `server_url` | `string` | 授权服务器基础 URL |
| `redirect_uri` | `string` | OAuth 重定向 URI |
| `client_id` | `optional<string>` | 客户端标识（未提供时自动注册） |
| `client_secret` | `optional<string>` | 客户端密钥（可选） |
| `scopes` | `vector<string>` | 请求的 OAuth 作用域 |
| `token_cache` | `shared_ptr<ITokenCache>` | 令牌持久化（默认：`InMemoryTokenCache`） |
| `authorization_redirect_handler` | `function<void(string_view url)>` | 打开授权 URL 的回调 |
| `authorization_code_callback` | `function<optional<AuthorizationCodeResult>()>` | 返回授权码及服务器回显的 `state`（`AuthorizationCodeResult{code, state}`），失败时返回 `nullopt` |

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
        // 返回授权码及授权服务器回显的 `state`（CSRF 防护）
        return AuthorizationCodeResult{"auth-code", "state"};
    };

auto auth = std::make_shared<OAuthClientProvider>(oauth_opts);
auth->Authenticate();
auto token = auth->GetAccessToken();
```

`OAuthClientProvider` 是独立类，不传递给 `McpClient::Create`。

## API

| 方法 | 描述 |
|--------|-------------|
| `Authenticate()` | 完整 OAuth 流程：发现 → 注册 → 授权 → 令牌交换 |
| `GetAccessToken()` | 返回有效的访问令牌，将在过期前自动刷新；刷新失败抛出 `McpError`（InternalError） |
| `RefreshTokens()` | 使用存储的刷新令牌强制刷新 |
| `IsAuthenticated()` | 检查令牌是否存在且未过期 |
| `HasToken()` | 检查是否存在任何令牌（可能已过期） |
| `GetAuthorizationHeader()` | 返回 `"Bearer {token}"` 字符串 |
| `StepUpAuthorization(scopes)` | 使用额外的作用域重新授权 |
| `AuthenticateClientCredentials()` | 客户端凭据授权（RFC 6749 §4.4），用于无需用户交互的服务到服务场景 |
| `HandleAuthChallenge(www_authenticate)` | 处理服务端返回的 401/403 认证挑战头（RFC 9728），成功时重试原请求 |
| `Revoke()` | best-effort 调用 RFC 7009 撤销端点（配置了 `revocation_endpoint` 时），无论成败都清除本地令牌 |

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

OpenSSL 开发头文件是编译 `mcp-client` 的**必需依赖**（`OAuthClientProvider.cpp` 无条件包含 `<openssl/rand.h>`；CMake 通过 `find_package(OpenSSL QUIET)` 自动查找，未找到时定义 `MCP_HAVE_OPENSSL` 失败，但编译仍会因缺少头文件而失败）。PKCE 代码验证器生成在构建时启用 OpenSSL（`MCP_HAVE_OPENSSL`）时使用 `RAND_bytes`，否则回退到 `std::random_device`。安装 OpenSSL（`vcpkg install openssl` / `apt install libssl-dev` / `brew install openssl`）可同时为 TLS（由自研网络栈处理）和密码学级随机数提供支持。
