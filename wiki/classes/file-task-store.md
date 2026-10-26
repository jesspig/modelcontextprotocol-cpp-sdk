---
type: Class
title: FileTaskStore
description: 文件持久化任务存储：CRUD、AdvanceTaskWithInput、Flush 失败回滚、损坏文件备份 .corrupt、新字段（statusMessage/lastUpdatedAt/ttlMs/pollIntervalMs/inputRequests/inputResponses）持久化。
tags: [storage, 任务, 原子写入, 回滚, tasks-extension]
timestamp: 2026-10-27T04:48:00+08:00
resource: include/mcp/storage/FileTaskStore.hpp
---

# FileTaskStore

`class FileTaskStore : public IMcpTaskStore`（[FileTaskStore.hpp](../../include/mcp/storage/FileTaskStore.hpp)），持 `storage_path_`、`tasks_`（`unordered_map<string, TaskState>`）。析构调用 `Flush()`。

**双锁**：`write_mutex_`（`std::mutex`，写方法全程持有，含写盘与回滚）+ `data_mutex_`（`std::shared_mutex`，读 `shared_lock` / 写 `unique_lock`）；锁序固定 `write_mutex_` → `data_mutex_`。写方法统一模式：锁内检查存在性与终态 → 修改 `tasks_`（内存提交）→ **锁外** `AppendRecord`（单条 JSONL 追加 + flush）→ 失败重取 `data_mutex_` 回滚（恢复 `original` / `erase`）并抛 `McpError(InternalError)`。

## CRUD 与失败语义（[FileTaskStore.cpp](../../src/server/FileTaskStore.cpp)）

- `CreateTask(task_id)` 与 `CreateTask(task_id, owner, ttl_ms)`：已存在抛 `McpError(InvalidParams, "task already exists: <id>")`；`created_at` 与 `last_updated_at` 同取毫秒时钟（`MakeMillisNow`）；追加失败从内存 erase 并抛 `McpError(InternalError)`
- `GetTask`：不存在返回 `nullopt`；`GetAllTasks`：拷贝全部（均 `shared_lock`，与写盘并发）
- `UpdateTask / CancelTask / SetTaskStatus / FailTask / ParkTaskForInput`：**返回 false 表示任务不存在或已终态**（Completed/Failed/Cancelled 拒绝再变更，`SetTaskStatus` 同状态幂等除外）；成功路径均刷新 `last_updated_at`；追加失败恢复原值（`original` 回滚）并抛 `McpError(InternalError)`
- `FailTask(task_id, error_payload)`：一步落 Failed 态并提取 `error.message` 进 `error_message`（服务端失败路径统一入口，替代「写错误结果再改状态」两步）
- `SetTaskOwner / SetTaskTtl`：按 id 更新对应字段；`RemoveTask`：追加 `removed` 记录后内存删除（不存在返回 false）；`GetExpiredTaskIds(now_ms)`：枚举 `ttl_ms > 0` 且 `now - created_at >= ttl_ms` 的任务
- `AdvanceTaskWithInput(task_id, input_responses)`：仅当任务存在**且**状态为 `InputRequired` 时推进（否则返回 false，不抛异常）；写入 `input_responses`、状态置 Working、刷新 `last_updated_at`
- `Flush()`：`write_mutex_` 下压缩（`CompactLocked`，`WriteAtomic` 全量重写现存活任务）；析构调用

## 持久化格式

**JSONL 追加 + 阈值压缩**：每条变更追加一行 `{"task_id":"...", "state":{...任务全字段...}}` 或 `{"task_id":"...", "removed":true}`（临时文件名 `<path>.tmp.<pid>.<counter>.<salt>` 语义同 `WriteAtomic` 家族）；追加后待处理记录数超过 `kMinCompactThreshold(64) + 存活数×2` 时自动压缩为全量快照（`CompactLocked`），析构 `Flush` 亦压缩。旧格式 `{"tasks": {...}}` 单对象文件仍可读（自动识别），且容忍**末尾截断记录**（半行忽略，中途坏行备份 `.corrupt` 后清空重建）。反序列化容缺：缺字段的旧记录仍可读，未知键忽略。状态字段含 `owner`（会话锚定）、`ttl_ms` 等新字段。

损坏文件：加载遇坏行把原文件复制为 `<path>.corrupt`（overwrite）后清空重建，复制失败仅记 Error。

## 相关页面

- [/modules/server.md](../modules/server.md) — 集成方
- [/concepts/storage.md](../concepts/storage.md) — 原子写入机制
- [/classes/mcp-server.md](mcp-server.md) — tasks/* 方法分发
- [/concepts/tasks-extension.md](../concepts/tasks-extension.md) — 任务扩展语义
- [/tests.md](../tests.md) — mcp-task-store-tests
