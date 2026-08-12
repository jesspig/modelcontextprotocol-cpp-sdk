---
type: Class
title: FileTaskStore
description: 文件持久化任务存储：CRUD、Flush 失败回滚、损坏文件备份 .corrupt。
tags: [storage, 任务, 原子写入, 回滚]
timestamp: 2026-08-13T03:25:00+08:00
resource: include/mcp/storage/FileTaskStore.hpp
---

# FileTaskStore

`class FileTaskStore : public IMcpTaskStore`（[FileTaskStore.hpp](../../include/mcp/storage/FileTaskStore.hpp)），持 `storage_path_`、`tasks_`（`unordered_map<string, TaskState>`）、`mutex_`。析构调用 `Flush()`。

## CRUD 与失败语义（[FileTaskStore.cpp](../../src/server/FileTaskStore.cpp)）

- `CreateTask`：已存在抛 `runtime_error "task already exists: <id>"`；Flush 失败从内存 erase 并抛异常
- `GetTask`：不存在返回 `nullopt`；`GetAllTasks`：拷贝全部
- `UpdateTask / CancelTask / SetTaskStatus`：**返回 false 仅表示任务不存在**；Flush 失败恢复原值（`original` 回滚）并抛异常
- `SetTaskStatus`：status=Cancelled 时写 `error_message`

## 持久化格式

根对象 `{"tasks": {id: {task_id, status, result?, error_message?, input_required?, progress, progress_total?, created_at}}}`；`result`/`input_required` 为 null 时不写字段。经 `detail::WriteAtomic` 原子写入（见 [/concepts/storage.md](../concepts/storage.md)）。

损坏文件：`LoadJson` 抛异常时把原文件复制为 `<path>.corrupt`（overwrite），复制失败仅记 Error；不覆盖原数据。

## 相关页面

- [/modules/server.md](../modules/server.md) — 集成方
- [/concepts/storage.md](../concepts/storage.md) — 原子写入机制
- [/classes/mcp-server.md](mcp-server.md) — tasks/* 方法分发
- [/tests.md](../tests.md) — mcp-task-store-tests
