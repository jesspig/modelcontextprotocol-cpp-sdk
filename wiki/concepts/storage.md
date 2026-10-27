---
type: Concept
title: 存储与原子写入
description: 临时文件 + fsync + rename 的原子持久化，任务/令牌存储的失败语义，FileEventStore 事件持久化与 SessionStore 会话记录抽象。
tags: [storage, 原子写入, fsync, 持久化, event-store, session-store]
timestamp: 2026-10-27T04:48:00+08:00
resource: include/mcp/detail/AtomicJsonFile.hpp
---

# 存储与原子写入

## AtomicJsonFile（[AtomicJsonFile.hpp](../../include/mcp/detail/AtomicJsonFile.hpp)）

- `WriteAtomic(path, contents)`：临时文件名为 `<path>.tmp.<pid>.<counter>`（pid + 进程内静态自增计数器，多进程与同进程并发均不踩踏，[AtomicJsonFile.cpp:21](../../src/detail/AtomicJsonFile.cpp)）；写入后 `fflush` + 刷盘（Windows `FlushFileBuffers` / POSIX `fsync`）再 rename
- 失败（打开/写/刷盘/rename）删除残留 tmp 并记 Error 日志、返回 false
- **`WriteAtomicSecure(path, contents)`**：机密文件专用——临时文件名追加随机 salt（`<path>.tmp.<pid>.<counter>.<salt>`），以 `CREATE_NEW`/`O_CREAT|O_EXCL|O_NOFOLLOW` 独占创建且权限创建即收紧（Windows SDDL `D:P(A;;FA;;;OW)` 仅所有者 ACL / POSIX `0600`），写入循环处理短写与 `EINTR`，刷盘后原子改名，POSIX 侧再 fsync 父目录；任一步失败删除残留 tmp 并**抛 `McpError(InternalError)`**（区别于 `WriteAtomic` 的返回 false）
- `WriteAtomic(path, JsonValue)`：`Dump(2)` 缩进 2 后走字节重载
- `LoadJson(path)`：文件不存在返回 null；打开失败或解析出 null 抛 `runtime_error`
- 调用方须保证父目录存在

## 消费方

| 组件 | 行为 | 页 |
|------|------|----|
| FileTaskStore | 双锁 + 锁外 JSONL 追加（见下）；损坏文件备份 `<path>.corrupt` 继续 | [/classes/file-task-store.md](../classes/file-task-store.md) |
| FileTokenCache | Windows DPAPI 加密、POSIX 明文；`WriteAtomicSecure` 创建即安全、加载前权限校验；解密失败不回落明文 | [/classes/file-token-cache.md](../classes/file-token-cache.md) |
| FileEventStore | SSE 事件 JSONL 持久化（跨进程文件锁，见下） | [/classes/file-event-store.md](../classes/file-event-store.md) |

## FileEventStore 事件持久化

`FileEventStore : EventStore`（[FileEventStore.hpp](../../include/mcp/storage/FileEventStore.hpp)），SSE 断线回放事件的参考实现（详见 [/classes/file-event-store.md](../classes/file-event-store.md)）：

- 每会话一个 JSONL 文件（`<dir>/sess-<净化id>.jsonl`，行 `{"id":N,"data":"..."}`）+ 一个 `.lock` 锁文件
- **跨进程文件锁**：每操作独立句柄阻塞排他锁（Win32 `LockFileEx` / POSIX `flock`），同进程多线程与其他进程在同一会话上串行
- 读取边界：缺失或不可读 JSONL 文件视为空存储；坏行或缺少字段的尾行跳过，读取不会因为单条损坏记录抛出
- 写入边界：目录创建、文件锁、追加、裁剪重写或 `Clear` 失败会抛异常；超过 1024 事件经 `WriteAtomic` 全量重写裁剪

## SessionStore 会话记录抽象

`SessionStore`（[SessionStore.hpp](../../include/mcp/http/SessionStore.hpp)）：`Save/Load/Remove` 会话记录 `SessionRecord{protocol_version, created_at_ms, last_seen_ms}`，键为会话 id。`last_seen_ms` 为 0 时按写入时刻回填（`TrySave` 内 `stored.last_seen_ms = now`）；`InMemorySessionStore`（mutex map）为默认实现，闲置超 `kSessionIdleTimeoutMs`（30 分钟，[Limits.hpp](../../include/mcp/transport/detail/Limits.hpp)）即过期——`TrySave` 先 `SweepExpiredLocked` 再判满（满回 false，`Save` 转抛 `runtime_error("session store is full")`，上限 `kMaxHttpSessions = 10000`），`Load` 命中即刷新 `last_seen_ms = now`、过期则删除并回 `nullopt`；`Size()` 返回当前 map 规模，`SweepExpired()` 返回本轮清除条数（基类默认均回 0）。stateful 模式注入 `StreamableHttpServerOptions::session_store` 后会话可被共享 store 的其他实例接管（[/transports/streamable-http.md](../transports/streamable-http.md)），未知会话 id 回 404 + `-32009`。

## FileTaskStore 双锁结构

`write_mutex_`（`std::mutex`，写方法全程持有，含写盘与回滚）+ `data_mutex_`（`std::shared_mutex`，读 `shared_lock` / 写 `unique_lock`）；**锁序固定 `write_mutex_` → `data_mutex_`**（[FileTaskStore.hpp:36](../../include/mcp/storage/FileTaskStore.hpp)）。

写方法（CreateTask/UpdateTask/CancelTask/SetTaskStatus/FailTask/SetTaskOwner/SetTaskTtl/AdvanceTaskWithInput/ParkTaskForInput/BindTaskTool）流程：

1. 锁内检查（任务不存在或已终态返回 `false`；重复创建抛 `McpError(InvalidParams)`）
2. 内存提交 → 释放 `data_mutex_` → **锁外** `AppendRecord`（单条 JSONL 追加 + flush；超过阈值自动 `CompactLocked` 压缩为 `WriteAtomic` 全量快照）
3. 持久化失败 → 重取 `data_mutex_` 回滚内存修改 → 抛 `McpError(InternalError)`

读（`GetTask`/`GetAllTasks`）在写盘 I/O 期间**不阻塞**——同一时刻至多一个写者持锁写盘，读者并发通过。

## 失败语义约定

- `UpdateTask/CancelTask/SetTaskStatus/FailTask/ParkTaskForInput` 返回 `false` 表示任务**不存在或已终态**（不是持久化失败——持久化失败抛 `McpError(InternalError)`）；`CreateTask` 不返回 bool（返回 `TaskState`，重复创建抛 `McpError(InvalidParams)`）
- token 刷新失败不回退旧 token
- `FileTokenCache` 的机密写入经 `WriteAtomicSecure`（失败抛 `McpError(InternalError)`，向 `StoreTokens`/`Save` 调用方传播）；析构内的 `Save` 捕获异常仅记 Error，避免进程终止；加载前校验文件属主与权限（组/其他可读即拒绝加载）

## 相关页面

- [/modules/server.md](../modules/server.md) — 服务端存储集成
- [/classes/file-event-store.md](../classes/file-event-store.md) — 事件存储实现
- [/classes/file-token-cache.md](../classes/file-token-cache.md) — 令牌缓存失败语义
- [/concepts/oauth.md](oauth.md) — 令牌生命周期
- [/tests.md](../tests.md) — 损坏处理测试
