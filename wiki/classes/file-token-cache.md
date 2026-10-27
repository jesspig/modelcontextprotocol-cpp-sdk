---
type: Class
title: FileTokenCache
description: OAuth 令牌文件缓存：Windows DPAPI 加密、POSIX 明文、创建即安全的原子写入、加载前权限校验、issuer 锚点持久化。
tags: [oauth, 缓存, dpapi, 安全]
timestamp: 2026-10-27T04:48:00+08:00
resource: src/client/FileTokenCache.cpp
---

# FileTokenCache

实现 `ITokenCache` 接口（`StoreTokens / GetTokens / ClearTokens`，[TokenCache.hpp](../../include/mcp/client/auth/TokenCache.hpp)）。类声明位于上述接口头，具体实现为 [FileTokenCache.cpp](../../src/client/FileTokenCache.cpp)。构造即 `Load()`，析构 `Save()`；三个公开方法持 `mutex_`。

## 存储语义（[FileTokenCache.cpp](../../src/client/FileTokenCache.cpp)）

- `Save`：无 tokens 时**删除文件**；否则写 JSON `{access_token, refresh_token, token_type, expires_at(毫秒), scopes[]}`，Windows 经 DPAPI `CryptProtectData` 加密后、POSIX 明文，统一经 `detail::WriteAtomicSecure`（**独占临时文件**：Windows SDDL 仅所有者 ACL / POSIX `0600` + `O_NOFOLLOW`，刷盘后原子改名；失败抛 `McpError(InternalError)`）
- **Load（Windows）**：`CryptUnprotectData` 解密失败**不回落明文**（记 Error 忽略缓存，重新认证即可）
- **Load（POSIX）**：先校验文件属主与权限（`ValidateTokenCachePermissions`：属主非当前用户、或组/其他可读即拒绝加载），**事后 chmod 已废除**——权限在创建时即正确
- **issuer 锚点**：`StoreBoundIssuer/GetBoundIssuer` 以 `<cache>.issuer` 旁挂文件持久化（同样走 `WriteAtomicSecure`），`ClearTokens` 不清除锚点——OAuth issuer 锚定跨实例持久化，见 [/concepts/oauth.md](../concepts/oauth.md)
- 析构 `Save` 异常捕获记 Error，不终止进程；`StoreTokens/Save` 路径的持久化失败会向调用方抛 `McpError`

## 相关页面

- [/modules/client.md](../modules/client.md) — 所属库
- [/concepts/oauth.md](../concepts/oauth.md) — 授权流程
- [/concepts/storage.md](../concepts/storage.md) — 原子写入机制
- [/tests.md](../tests.md) — mcp-token-cache-tests
