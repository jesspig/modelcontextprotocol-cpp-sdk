---
type: Concept
title: 客户端响应缓存
description: McpClient 对带 cache hint 的列表和资源读取结果使用的双作用域、带 TTL 和按 URI 失效的本地缓存。
tags: [client, cache, sep2549, ttl, 订阅]
timestamp: 2026-09-20T03:14:18+08:00
resource: src/detail/ResponseCache.hpp
---

# 客户端响应缓存

`detail::ResponseCache` 是 `McpClient` 使用的进程内缓存，服务于带 `ttlMs` / `cacheScope` 提示的列表和资源读取结果。缓存不跨进程，也不负责持久化。

## 键与作用域

- `CacheKey` 按方法和上下文构成：列表请求包含 cursor，`resources/read` 包含 URI；同一方法的不同页或 URI 使用不同键。
- 条目分为 `public` 与 `private` 两个分区。`GetAny` 先查 public，再查 private；同一连接内可以命中任一分区。
- TTL 上限固定为 24 小时；过期或超过调用方传入的 `max_age` 时惰性删除并返回未命中。
- `ClearPrivate` 在客户端关闭时清除 private 条目，public 条目保留；`Clear` 清空两个分区。

## 失效与协议提示

- `notifications/tools/list_changed`、`resources/list_changed`、`prompts/list_changed` 清空整个响应缓存。
- `notifications/resources/updated` 只调用 `Invalidate(uri)`，同时删除两个分区中对应 URI 键，不影响其他资源和列表条目。
- `McpClient` 兼容 2026 时代结果顶层 `ttlMs` / `cacheScope` 与 2025 时代嵌套 `cacheHint`；读取提示和写入提示的优先顺序按实现分别处理，详见客户端页面。
- `cache_mode` 为 `use`、`bypass`、`refresh` 时分别对应正常读取、跳过缓存、绕过旧值后重新写入；`max_age_ms` 约束命中年龄。

## 相关页面

- [/classes/mcp-client.md](../classes/mcp-client.md) — 缓存键、提示兼容和调用入口
- [/modules/client.md](../modules/client.md) — 客户端通知失效
- [/concepts/meta-and-filters.md](../concepts/meta-and-filters.md) — cache hint 的线格式
- [/tests.md](../tests.md) — 客户端缓存守护测试
