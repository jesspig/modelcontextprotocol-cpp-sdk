---
type: Concept
title: OAuth 授权流程
description: 客户端授权码 + PKCE（S256）、CIMD 客户端标识、RFC 8707 资源指示符、RFC 9207 iss 强制校验、刷新/吊销/提权、令牌缓存；服务端 Bearer 资源服务器（RFC 6750/9728）。
tags: [oauth, pkce, 安全, rfc8707, rfc9207, cimd, bearer]
timestamp: 2026-09-20T01:45:19+08:00
resource: src/client/auth/OAuthClientProvider.cpp
---

# OAuth 授权流程

`OAuthClientProvider`（[OAuthClientProvider.cpp](../../src/client/auth/OAuthClientProvider.cpp)）——头文件注释仍声明整体单线程使用；`GetAccessToken` 内部用 `refresh_mutex_` 串行化"读 token → 判断过期 → 刷新 → 写回"整段（防多线程用同一 `refresh_token` 并发刷新）。无 token_cache 时默认 `InMemoryTokenCache`。

## 流程（`Authenticate()`）

1. `DiscoverMetadata`：先 RFC 8414 `.well-known/oauth-authorization-server`；失败回退硬编码 `/authorize` + `/token` + issuer=server_url
2. `client_id` 已配置时先 `FetchClientMetadataDocument`（**CIMD**：`client_id` 为 URL 则 GET 该文档并解析客户端元数据，失败仅记 Warning 并回退已配置值）；仅当**无** `client_id` 且 metadata 有 registration_endpoint 时才 `RegisterClient`（POST `{redirect_uris, client_name:"mcp-cpp-client"}`）
3. 缓存 token 未过期直接成功；过期但有 refresh_token → `RefreshTokens`；否则授权码流

另提供 `AuthenticateClientCredentials()`（client_credentials grant）：要求 `client_id` + `client_secret`，POST `grant_type=client_credentials`，同样强制 `iss` 校验（[OAuthClientProvider.cpp:276](../../src/client/auth/OAuthClientProvider.cpp)）。

## 授权码流

- **PKCE S256**：`GenerateCodeVerifier` 32 随机字节 → Base64url（OpenSSL `RAND_bytes`；无 OpenSSL 时 Win32 走 `BCryptGenRandom`，仅 POSIX 回退 `random_device + mt19937`）；`ComputeCodeChallenge` 经内置 SHA-256（[sha256.hpp](../../include/mcp/detail/sha256.hpp)，FIPS 180-4 独立实现，不依赖 OpenSSL）
- **CSRF 校验**（RFC 6749 §10.12）：回调返回的 state 必须等于发送的 state
- **授权响应 iss 校验**（RFC 9207）：`AuthorizationCodeResult::iss` 有值且与 metadata issuer 不等即拒绝，位置在 state 校验之后、`ExchangeCodeForToken` 之前；未携带 `iss` 则不校验
- `ValidateTokenIssuer`（**RFC 9207 强制**）：token/refresh 响应缺 `iss` 或与 metadata issuer 不匹配即拒绝

## 资源指示符（RFC 8707）

`ResolveResourceIndicator()` 取值优先级：显式 `OAuthClientOptions::resource` → 已发现 metadata 的 `resource` → `server_url`。解析结果非空时注入授权 URL（`&resource=`）与三个令牌请求表单（client_credentials、授权码交换、refresh_token），为空则**完全不发送**该参数。

## HandleAuthChallenge（RFC 9728）

解析 `resource_metadata="<uri>"` → 拉取资源元数据合并进 metadata_，随后 `VerifyResourceMatch` **事后校验**（[OAuthClientProvider.cpp:125](../../src/client/auth/OAuthClientProvider.cpp)）：`OAuthMetadata.resource` 字段须与 `options_.server_url` **精确相等**或匹配其 **scheme+host** 前缀；字段缺失/空则放行；不匹配抛 `McpError(InternalError)`。

## 失败语义

- 响应缺 `access_token` → 失败，**不回退旧 token**（`RefreshTokens` 亦然）
- 响应缺 `refresh_token` → 保留旧值（RFC 6749 非轮转）
- `GetAccessToken`：`WillExpireSoon()`（默认 margin 60s）触发自动刷新；刷新失败抛 `McpError(InternalError, "OAuth token refresh failed")`
- `Revoke`：RFC 7009 best-effort，本地缓存无论 HTTP 结果都清空
- `StepUpAuthorization`（SEP-2350）：合并 scopes → Revoke → 重新授权

## 服务端 Bearer 资源服务器（RFC 6750/9728）

`StreamableHttpServerOptions::bearer_auth`（[StreamableHttpServerTransport.hpp](../../include/mcp/transport/StreamableHttpServerTransport.hpp)）把服务端变成受保护资源：

- `verify` 回调必填（`AuthResult{ok, scopes}`），token 校验完全委托调用方（SDK 不解析 JWT）
- POST/GET 入口挑战：无头/非 Bearer → 401 + `WWW-Authenticate: Bearer resource_metadata="<url>"`；校验失败 → 401 + `error="invalid_token"`；`required_scopes` 未被 token scopes 全覆盖 → 403 + `error="insufficient_scope", scope="..."`
- `serve_metadata_endpoint` 注册匿名 `GET /.well-known/oauth-protected-resource`（RFC 9728 文档：`resource`/`authorization_servers`/`scopes_supported`/`bearer_methods_supported`）
- 未配置时零回归（无任何鉴权路径）；wire 细节见 [/transports/streamable-http.md](../transports/streamable-http.md)；客户端侧对接入口为 `auth_challenge_handler`（收到 401/403 挑战后重试）

## 相关页面

- [/modules/client.md](../modules/client.md) — 所属库
- [/transports/streamable-http.md](../transports/streamable-http.md) — 服务端 Bearer 挑战落点
- [/classes/file-token-cache.md](../classes/file-token-cache.md) — 持久化
- [/concepts/storage.md](storage.md) — 原子写入
- [/docs/en/client/oauth.md](../../docs/en/client/oauth.md) — 在线文档
