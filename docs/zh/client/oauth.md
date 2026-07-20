# OAuth 支持

客户端支持需要认证的 MCP OAuth 授权流程。

## 流程

1. **授权码 + PKCE**，使用 S256 代码质询
2. **动态客户端注册**（DCR），用于首次使用的客户端（合成注册，无网络请求）
3. **令牌刷新**，通过预过期检查提前刷新（非 401 驱动）
4. **令牌撤销**，通过手动调用 `Revoke()`

## 设置

```cpp
OAuthClientOptions oauth_opts;
oauth_opts.server_url = "https://auth.server.com";
oauth_opts.redirect_uri = "http://localhost:3000/callback";
oauth_opts.client_id = "my-client";
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
| `Revoke()` | 清除存储的令牌 |

## PKCE 辅助函数

```cpp
auto verifier = pkce::GenerateCodeVerifier();
auto challenge = pkce::ComputeCodeChallenge(verifier);
auto encoded = pkce::Base64UrlEncode(input);
```

## 令牌缓存

SDK 提供了 `ITokenCache` 的两种实现：

| 实现 | 持久化 | 加密 |
|----------------|-------------|------------|
| `InMemoryTokenCache` | 仅运行时 | 无 |
| `FileTokenCache` | JSON 文件（POSIX）/ DPAPI 加密（Windows） | POSIX 上 `chmod 0600`，Windows 上 `CryptProtectData` |

```cpp
auto token_cache = std::make_shared<FileTokenCache>("./tokens.json");
OAuthClientOptions oauth_opts;
oauth_opts.token_cache = token_cache;
```

## 要求

当没有 OpenSSL 时，OAuth PKCE 使用 `std::random_device` 作为回退。安装 OpenSSL 后（`vcpkg install openssl` / `apt install libssl-dev` / `brew install openssl`）将使用 `RAND_bytes` 生成代码验证器，同时启用 TLS 安全的令牌交换。
