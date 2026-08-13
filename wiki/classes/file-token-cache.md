---
type: Class
title: FileTokenCache
description: OAuth 令牌文件缓存：Windows DPAPI 加密、POSIX 明文 + chmod 0600。
tags: [oauth, 缓存, dpapi, 安全]
timestamp: 2026-08-13T12:36:14+08:00
resource: src/client/FileTokenCache.cpp
---

# FileTokenCache

实现 `ITokenCache` 接口（`StoreTokens / GetTokens / ClearTokens`，[TokenCache.hpp](../../include/mcp/client/auth/TokenCache.hpp)）。构造即 `Load()`，析构 `Save()`，所有操作持 `mutex_`。

## 存储语义（[FileTokenCache.cpp](../../src/client/FileTokenCache.cpp)）

- `Save`：无 tokens 时**删除文件**；否则写 JSON `{access_token, refresh_token, token_type, expires_at(毫秒), scopes[]}`，`Dump(2)` 后经 `detail::WriteAtomic`（临时文件 + fsync + rename）
- **Windows（DPAPI）**：`CryptProtectData` 加密（描述符 `L"MCP Token Cache"`）后写二进制；加密失败仅记 Error 不写文件
- **POSIX**：明文 JSON + `chmod 0600`
- **Load（Windows）**：`CryptUnprotectData` 解密失败**不回落明文**（记 Error 忽略缓存，重新认证即可）

## 相关页面

- [/modules/client.md](../modules/client.md) — 所属库
- [/concepts/oauth.md](../concepts/oauth.md) — 授权流程
- [/concepts/storage.md](../concepts/storage.md) — 原子写入机制
- [/tests.md](../tests.md) — mcp-token-cache-tests
