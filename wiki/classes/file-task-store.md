---
type: Class
title: FileTaskStore
description: 文件持久化任务存储：CRUD、AdvanceTaskWithInput、Flush 失败回滚、损坏文件备份 .corrupt、新字段（statusMessage/lastUpdatedAt/ttlMs/pollIntervalMs/inputRequests/inputResponses）持久化。
tags: [storage, 任务, 原子写入, 回滚, tasks-extension]
timestamp: 2026-10-11T16:35:00+08:00
resource: include/mcp/storage/FileTaskStore.hpp
---

# FileTaskStore

`class FileTaskStore : public IMcpTaskStore`（[FileTaskStore.hpp](../../include/mcp/storage/FileTaskStore.hpp)），持 `storage_path_`、`tasks_`（`unordered_map<string, TaskState>`）。析构调用 `Flush()`。

**双锁**：`write_mutex_`（`std::mutex`，写方法全程持有，含写盘与回滚）+ `data_mutex_`（`std::shared_mutex`，读 `shared_lock` / 写 `unique_lock`）；锁序固定 `write_mutex_` → `data_mutex_`。写方法统一模式：锁内检查存在性 → 修改 `tasks_`（内存提交）→ 拷贝快照 → 释放 `data_mutex_` → **锁外** `PersistTasks`（序列化 + `WriteAtomic`）→ 失败重取 `data_mutex_` 回滚（恢复 `original` / `erase`）并抛 `std::runtime_error`。

## CRUD 与失败语义（[FileTaskStore.cpp](../../src/server/FileTaskStore.cpp)）

- `CreateTask`：`try_emplace`，已存在抛 `runtime_error "task already exists: <id>"`；`created_at` 与 `last_updated_at` 同取毫秒时钟（`MakeMillisNow`）；Flush 失败从内存 erase 并抛异常
- `GetTask`：不存在返回 `nullopt`；`GetAllTasks`：拷贝全部（均 `shared_lock`，与写盘并发）
- `UpdateTask / CancelTask / SetTaskStatus`：**返回 false 仅表示任务不存在**；成功路径均刷新 `last_updated_at`；Flush 失败恢复原值（`original` 回滚）并抛异常
- `CancelTask`：置 status=Cancelled 并写 `error_message`=reason（[FileTaskStore.cpp:154](../../src/server/FileTaskStore.cpp)）；`SetTaskStatus` 仅改状态，不写 `error_message`
- `AdvanceTaskWithInput(task_id, input_responses)`：仅当任务存在**且**状态为 `InputRequired` 时推进（否则返回 false，不抛异常）；写入 `input_responses`、状态置 Working、刷新 `last_updated_at`；持久化走同一快照+回滚路径
- `Flush()`：`shared_lock` 快照 + 锁外 `PersistTasks`；析构忽略返回值

## 持久化格式

根对象 `{"tasks": {id: {task_id, status, result?, error_message?, input_required?, progress, progress_total?, created_at, statusMessage?, lastUpdatedAt?, ttlMs?, pollIntervalMs?, inputRequests?, inputResponses?}}}`；`result`/`input_required`/`inputResponses` 为 null 时不写字段，`statusMessage/lastUpdatedAt/ttlMs/pollIntervalMs` 仅有值时写，`inputRequests` 仅对象形态时读写（`SerializeInputRequests`/`DeserializeInputRequests`）。反序列化容缺：缺字段的旧文件仍可读（新字段留空），未知键忽略。序列化提取为 `SerializeTasks`（`{"tasks": {...}}` 格式不变），经 `detail::WriteAtomic` 原子写入（见 [/concepts/storage.md](../concepts/storage.md)）。

损坏文件：`LoadJson` 抛异常时把原文件复制为 `<path>.corrupt`（overwrite），复制失败仅记 Error；不覆盖原数据。

## 相关页面

- [/modules/server.md](../modules/server.md) — 集成方
- [/concepts/storage.md](../concepts/storage.md) — 原子写入机制
- [/classes/mcp-server.md](mcp-server.md) — tasks/* 方法分发
- [/concepts/tasks-extension.md](../concepts/tasks-extension.md) — 任务扩展语义
- [/tests.md](../tests.md) — mcp-task-store-tests
