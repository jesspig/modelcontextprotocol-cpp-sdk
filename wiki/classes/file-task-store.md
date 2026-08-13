---
type: Class
title: FileTaskStore
description: 文件持久化任务存储：CRUD、Flush 失败回滚、损坏文件备份 .corrupt。
tags: [storage, 任务, 原子写入, 回滚]
timestamp: 2026-08-13T12:36:14+08:00
resource: include/mcp/storage/FileTaskStore.hpp
---

# FileTaskStore

`class FileTaskStore : public IMcpTaskStore`（[FileTaskStore.hpp](../../include/mcp/storage/FileTaskStore.hpp)），持 `storage_path_`、`tasks_`（`unordered_map<string, TaskState>`）。析构调用 `Flush()`。

**双锁**：`write_mutex_`（`std::mutex`，写方法全程持有，含写盘与回滚）+ `data_mutex_`（`std::shared_mutex`，读 `shared_lock` / 写 `unique_lock`）；锁序固定 `write_mutex_` → `data_mutex_`。写方法统一模式：锁内检查存在性 → 修改 `tasks_`（内存提交）→ 拷贝快照 → 释放 `data_mutex_` → **锁外** `PersistTasks`（序列化 + `WriteAtomic`）→ 失败重取 `data_mutex_` 回滚（恢复 `original` / `erase`）并抛 `std::runtime_error`。

## CRUD 与失败语义（[FileTaskStore.cpp](../../src/server/FileTaskStore.cpp)）

- `CreateTask`：`try_emplace`，已存在抛 `runtime_error "task already exists: <id>"`；Flush 失败从内存 erase 并抛异常
- `GetTask`：不存在返回 `nullopt`；`GetAllTasks`：拷贝全部（均 `shared_lock`，与写盘并发）
- `UpdateTask / CancelTask / SetTaskStatus`：**返回 false 仅表示任务不存在**；Flush 失败恢复原值（`original` 回滚）并抛异常
- `SetTaskStatus`：status=Cancelled 时写 `error_message`
- `Flush()`：`shared_lock` 快照 + 锁外 `PersistTasks`；析构忽略返回值

## 持久化格式

根对象 `{"tasks": {id: {task_id, status, result?, error_message?, input_required?, progress, progress_total?, created_at}}}`；`result`/`input_required` 为 null 时不写字段。序列化提取为 `SerializeTasks`（`{"tasks": {...}}` 格式不变），经 `detail::WriteAtomic` 原子写入（见 [/concepts/storage.md](../concepts/storage.md)）。

损坏文件：`LoadJson` 抛异常时把原文件复制为 `<path>.corrupt`（overwrite），复制失败仅记 Error；不覆盖原数据。

## 相关页面

- [/modules/server.md](../modules/server.md) — 集成方
- [/concepts/storage.md](../concepts/storage.md) — 原子写入机制
- [/classes/mcp-server.md](mcp-server.md) — tasks/* 方法分发
- [/tests.md](../tests.md) — mcp-task-store-tests
