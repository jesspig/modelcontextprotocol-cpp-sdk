---
type: Concept
title: OAuth 授权流程
description: 授权码 + PKCE（S256）、RFC 9207 iss 强制校验、刷新/吊销/提权、令牌缓存。
tags: [oauth, pkce, 安全, rfc9207]
timestamp: 2026-08-13T12:36:14+08:00
resource: src/client/auth/OAuthClientProvider.cpp
---

# OAuth 授权流程

`OAuthClientProvider`（[OAuthClientProvider.cpp](../../src/client/auth/OAuthClientProvider.cpp)）——头文件注释仍声明整体单线程使用；`GetAccessToken` 内部用 `refresh_mutex_` 串行化"读 token → 判断过期 → 刷新 → 写回"整段（防多线程用同一 `refresh_token` 并发刷新）。无 token_cache 时默认 `InMemoryTokenCache`。

## 流程（`Authenticate()`）

1. `DiscoverMetadata`：先 RFC 8414 `.well-known/oauth-authorization-server`；失败回退硬编码 `/authorize` + `/token` + issuer=server_url
2. 无 client_id 且 metadata 有 registration_endpoint 时 `RegisterClient`（POST `{redirect_uris, client_name:"mcp-cpp-client"}`）
3. 缓存 token 未过期直接成功；过期但有 refresh_token → `RefreshTokens`；否则授权码流

## 授权码流

- **PKCE S256**：`GenerateCodeVerifier` 32 随机字节 → Base64url（OpenSSL `RAND_bytes`，无则 `random_device + mt19937` 回退）；`ComputeCodeChallenge` 经内置 SHA-256（[sha256.hpp](../../include/mcp/detail/sha256.hpp)，FIPS 180-4 独立实现，不依赖 OpenSSL）
- **CSRF 校验**（RFC 6749 §10.12）：回调返回的 state 必须等于发送的 state
- `ValidateTokenIssuer`（**RFC 9207 强制**）：token/refresh 响应缺 `iss` 或与 metadata issuer 不匹配即拒绝

## HandleAuthChallenge（RFC 9728）

解析 `resource_metadata="<uri>"` → 拉取资源元数据合并进 metadata_，随后 `VerifyResourceMatch` **事后校验**（[OAuthClientProvider.cpp](../../src/client/auth/OAuthClientProvider.cpp:86)）：`OAuthMetadata.resource` 字段须与 `options_.server_url` **精确相等**或匹配其 **scheme+host** 前缀；字段缺失/空则放行；不匹配抛 `McpError(InternalError)`。

## 失败语义

- 响应缺 `access_token` → 失败，**不回退旧 token**（`RefreshTokens` 亦然）
- 响应缺 `refresh_token` → 保留旧值（RFC 6749 非轮转）
- `GetAccessToken`：`WillExpireSoon()`（默认 margin 60s）触发自动刷新；刷新失败抛 `McpError(InternalError, "OAuth token refresh failed")`
- `Revoke`：RFC 7009 best-effort，本地缓存无论 HTTP 结果都清空
- `StepUpAuthorization`（SEP-2350）：合并 scopes → Revoke → 重新授权

## 相关页面

- [/modules/client.md](../modules/client.md) — 所属库
- [/classes/file-token-cache.md](../classes/file-token-cache.md) — 持久化
- [/concepts/storage.md](storage.md) — 原子写入
- [/docs/en/client/oauth.md](../../docs/en/client/oauth.md) — 在线文档
