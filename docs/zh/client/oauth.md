# OAuth 支持

客户端支持需要认证的 MCP OAuth 授权流程。

## 流程

1. **授权码 + PKCE**，使用 S256 代码质询
2. **动态客户端注册**（DCR），用于首次使用的客户端
3. **令牌刷新**，在收到 401 响应时自动重试
4. **令牌撤销**，在客户端关闭时执行

## 设置

```cpp
auto oauth = std::make_shared<OAuthClientProvider>(
    "https://auth.server.com/.well-known/oauth-authorization-server",
    "client-id");

auto client = McpClient::Create(transport, options, &io_ctx, oauth);
```

## 令牌缓存

SDK 提供了 `ITokenCache` 的两种实现：

| 实现 | 持久化 |
|----------------|-------------|
| `InMemoryTokenCache` | 仅运行时 |
| `FileTokenCache` | 磁盘上的 JSON 文件 |

```cpp
auto token_cache = std::make_shared<FileTokenCache>("./tokens.json");
OAuthClientOptions oauth_opts;
oauth_opts.token_cache = token_cache;
```

## 要求

OAuth PKCE 需要 OpenSSL。安装 OpenSSL（`vcpkg install openssl` / `apt install libssl-dev` / `brew install openssl`）后构建系统会自动检测到它。
