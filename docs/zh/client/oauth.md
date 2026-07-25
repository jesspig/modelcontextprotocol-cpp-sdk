# OAuth 支持

客户端支持需要认证的 MCP OAuth 授权流程。

## 流程

1. **授权码 + PKCE**，使用 S256 代码质询
2. **动态客户端注册**（DCR），用于首次使用的客户端（HTTP POST 到注册端点）
3. **令牌刷新**，通过预过期检查提前刷新（非 401 驱动）
4. **令牌撤销**，通过手动调用 `Revoke()`

## OAuthClientOptions

| 字段 | 类型 | 描述 |
|-------|------|-------------|
| `server_url` | `string` | 授权服务器基础 URL |
| `redirect_uri` | `string` | OAuth 重定向 URI |
| `client_id` | `optional<string>` | 客户端标识（未提供时自动注册） |
| `client_secret` | `optional<string>` | 客户端密钥（可选） |
| `client_metadata_document_uri` | `optional<string>` | 外部元数据文档 URI |
| `scopes` | `vector<string>` | 请求的 OAuth 作用域 |
| `token_cache` | `shared_ptr<ITokenCache>` | 令牌持久化（默认：`InMemoryTokenCache`） |
| `auth_server_url` | `optional<string>` | 显式授权服务器 URL（覆盖元数据发现） |
| `timeout_seconds` | `int` | HTTP 请求超时（默认 30） |
| `authorization_redirect_handler` | `function<void(string_view url)>` | 打开授权 URL 的回调 |
| `authorization_code_callback` | `function<optional<string>()>` | 返回授权码的回调 |

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
    []() -> std::optional<std::string> {
        // 从重定向返回授权码
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
| `GetAccessToken()` | 返回有效的访问令牌，将在过期前自动刷新 |
| `RefreshTokens()` | 使用存储的刷新令牌强制刷新 |
| `IsAuthenticated()` | 检查令牌是否存在且未过期 |
| `HasToken()` | 检查是否存在任何令牌（可能已过期） |
| `GetAuthorizationHeader()` | 返回 `"Bearer {token}"` 字符串 |
| `StepUpAuthorization(scopes)` | 使用额外的作用域重新授权 |
| `Revoke()` | 清除存储的令牌（不调用撤销端点） |

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
| `FileTokenCache`（`<mcp/storage/FileTokenCache.hpp>`） | JSON 文件（Windows 上 DPAPI 加密） | POSIX 上 `chmod 0600`，Windows 上 `CryptProtectData`（DPAPI）；额外提供 `LoadTokenResponse()` 和 `LoadClientRegistration()` 方法 |

```cpp
#include <mcp/storage/FileTokenCache.hpp>

auto token_cache = std::make_shared<FileTokenCache>("./tokens.json");
OAuthClientOptions oauth_opts;
oauth_opts.token_cache = token_cache;
```

## 要求

OAuth PKCE 在构建时若使用 OpenSSL（`MCP_HAVE_OPENSSL`），则使用 `RAND_bytes`；否则回退到 `std::random_device`。安装 OpenSSL（`vcpkg install openssl` / `apt install libssl-dev` / `brew install openssl`）可为代码验证器生成提供密码学级随机数。令牌交换的 TLS 由 libhv 的 HTTP 客户端处理。
