---
type: Concept
title: 存储与原子写入
description: 临时文件 + fsync + rename 的原子持久化，以及任务/令牌存储的失败语义。
tags: [storage, 原子写入, fsync, 持久化]
timestamp: 2026-08-13T12:36:14+08:00
resource: include/mcp/detail/AtomicJsonFile.hpp
---

# 存储与原子写入

## AtomicJsonFile（[AtomicJsonFile.hpp](../../include/mcp/detail/AtomicJsonFile.hpp)）

- `WriteAtomic(path, contents)`：临时文件名为 `<path>.tmp.<pid>`（**多进程不踩踏**）；写入后 `fflush` + 刷盘（Windows `FlushFileBuffers` / POSIX `fsync`）再 rename
- 失败（打开/写/刷盘/rename）删除残留 tmp 并记 Error 日志、返回 false
- `WriteAtomic(path, JsonValue)`：`Dump(2)` 缩进 2 后走字节重载
- `LoadJson(path)`：文件不存在返回 null；打开失败或解析出 null 抛 `runtime_error`
- 调用方须保证父目录存在

## 消费方

| 组件 | 行为 | 页 |
|------|------|----|
| FileTaskStore | 双锁 + 锁外写盘（见下）；损坏文件备份 `<path>.corrupt` 继续 | [/classes/file-task-store.md](../classes/file-task-store.md) |
| FileTokenCache | Windows DPAPI 加密、POSIX chmod 0600；解密失败不回落明文 | [/classes/file-token-cache.md](../classes/file-token-cache.md) |

## FileTaskStore 双锁结构

`write_mutex_`（`std::mutex`，写方法全程持有，含写盘与回滚）+ `data_mutex_`（`std::shared_mutex`，读 `shared_lock` / 写 `unique_lock`）；**锁序固定 `write_mutex_` → `data_mutex_`**（[FileTaskStore.hpp](../../include/mcp/storage/FileTaskStore.hpp:36)）。

写方法（CreateTask/UpdateTask/CancelTask/SetTaskStatus）流程：

1. 锁内检查（任务不存在返回 `false`；重复创建抛 `runtime_error`）
2. 内存提交 → 释放 `data_mutex_` → **锁外** `PersistTasks`（`SerializeTasks` + `WriteAtomic`）
3. 持久化失败 → 重取 `data_mutex_` 回滚内存修改 → 抛 `runtime_error`

读（`GetTask`/`GetAllTasks`）在写盘 I/O 期间**不阻塞**——同一时刻至多一个写者持锁写盘，读者并发通过。

## 失败语义约定

- `CreateTask/UpdateTask/CancelTask/SetTaskStatus` 返回 `false` **仅表示任务不存在**（不是持久化失败——持久化失败抛异常）
- token 刷新失败不回退旧 token

## 相关页面

- [/modules/server.md](../modules/server.md) — 服务端存储集成
- [/concepts/oauth.md](oauth.md) — 令牌生命周期
- [/tests.md](../tests.md) — 损坏处理测试
