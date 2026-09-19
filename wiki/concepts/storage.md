---
type: Concept
title: 存储与原子写入
description: 临时文件 + fsync + rename 的原子持久化，任务/令牌存储的失败语义，FileEventStore 事件持久化与 SessionStore 会话记录抽象。
tags: [storage, 原子写入, fsync, 持久化, event-store, session-store]
timestamp: 2026-09-20T03:14:18+08:00
resource: include/mcp/detail/AtomicJsonFile.hpp
---

# 存储与原子写入

## AtomicJsonFile（[AtomicJsonFile.hpp](../../include/mcp/detail/AtomicJsonFile.hpp)）

- `WriteAtomic(path, contents)`：临时文件名为 `<path>.tmp.<pid>.<counter>`（pid + 进程内静态自增计数器，多进程与同进程并发均不踩踏，[AtomicJsonFile.cpp:21](../../src/detail/AtomicJsonFile.cpp)）；写入后 `fflush` + 刷盘（Windows `FlushFileBuffers` / POSIX `fsync`）再 rename
- 失败（打开/写/刷盘/rename）删除残留 tmp 并记 Error 日志、返回 false
- `WriteAtomic(path, JsonValue)`：`Dump(2)` 缩进 2 后走字节重载
- `LoadJson(path)`：文件不存在返回 null；打开失败或解析出 null 抛 `runtime_error`
- 调用方须保证父目录存在

## 消费方

| 组件 | 行为 | 页 |
|------|------|----|
| FileTaskStore | 双锁 + 锁外写盘（见下）；损坏文件备份 `<path>.corrupt` 继续 | [/classes/file-task-store.md](../classes/file-task-store.md) |
| FileTokenCache | Windows DPAPI 加密、POSIX chmod 0600；解密失败不回落明文 | [/classes/file-token-cache.md](../classes/file-token-cache.md) |
| FileEventStore | SSE 事件 JSONL 持久化（跨进程文件锁，见下） | [/classes/file-event-store.md](../classes/file-event-store.md) |

## FileEventStore 事件持久化

`FileEventStore : EventStore`（[FileEventStore.hpp](../../include/mcp/storage/FileEventStore.hpp)），SSE 断线回放事件的参考实现（详见 [/classes/file-event-store.md](../classes/file-event-store.md)）：

- 每会话一个 JSONL 文件（`<dir>/sess-<净化id>.jsonl`，行 `{"id":N,"data":"..."}`）+ 一个 `.lock` 锁文件
- **跨进程文件锁**：每操作独立句柄阻塞排他锁（Win32 `LockFileEx` / POSIX `flock`），同进程多线程与其他进程在同一会话上串行
- 读取边界：缺失或不可读 JSONL 文件视为空存储；坏行或缺少字段的尾行跳过，读取不会因为单条损坏记录抛出
- 写入边界：目录创建、文件锁、追加、裁剪重写或 `Clear` 失败会抛异常；超过 1024 事件经 `WriteAtomic` 全量重写裁剪

## SessionStore 会话记录抽象

`SessionStore`（[SessionStore.hpp](../../include/mcp/http/SessionStore.hpp)）：`Save/Load/Remove` 会话记录 `SessionRecord{protocol_version, created_at_ms}`，键为会话 id。`InMemorySessionStore`（mutex map）为默认实现；stateful 模式注入 `StreamableHttpServerOptions::session_store` 后会话可被共享 store 的其他实例接管（[/transports/streamable-http.md](../transports/streamable-http.md)），未知会话 id 回 404 + `-32009`。

## FileTaskStore 双锁结构

`write_mutex_`（`std::mutex`，写方法全程持有，含写盘与回滚）+ `data_mutex_`（`std::shared_mutex`，读 `shared_lock` / 写 `unique_lock`）；**锁序固定 `write_mutex_` → `data_mutex_`**（[FileTaskStore.hpp](../../include/mcp/storage/FileTaskStore.hpp:36)）。

写方法（CreateTask/UpdateTask/CancelTask/SetTaskStatus）流程：

1. 锁内检查（任务不存在返回 `false`；重复创建抛 `runtime_error`）
2. 内存提交 → 释放 `data_mutex_` → **锁外** `PersistTasks`（`SerializeTasks` + `WriteAtomic`）
3. 持久化失败 → 重取 `data_mutex_` 回滚内存修改 → 抛 `runtime_error`

读（`GetTask`/`GetAllTasks`）在写盘 I/O 期间**不阻塞**——同一时刻至多一个写者持锁写盘，读者并发通过。

## 失败语义约定

- `UpdateTask/CancelTask/SetTaskStatus` 返回 `false` **仅表示任务不存在**（不是持久化失败——持久化失败抛异常）；`CreateTask` 不返回 bool（返回 `TaskState`，重复创建直接抛 `runtime_error`）
- token 刷新失败不回退旧 token
- `FileTokenCache` 的写入、删除、chmod、DPAPI 加解密失败采用 Error 日志和忽略/继续认证的 best-effort 语义，不向 `StoreTokens`/`ClearTokens` 调用方传播持久化异常

## 相关页面

- [/modules/server.md](../modules/server.md) — 服务端存储集成
- [/classes/file-event-store.md](../classes/file-event-store.md) — 事件存储实现
- [/classes/file-token-cache.md](../classes/file-token-cache.md) — 令牌缓存失败语义
- [/concepts/oauth.md](oauth.md) — 令牌生命周期
- [/tests.md](../tests.md) — 损坏处理测试
