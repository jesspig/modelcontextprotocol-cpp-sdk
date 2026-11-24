---
type: Concept
title: 存储与原子写入
description: 临时文件 + fsync + rename 的原子持久化，任务/令牌存储的失败语义，FileEventStore 事件持久化与 SessionStore 会话记录抽象。
tags: [storage, 原子写入, fsync, 持久化, event-store, session-store]
timestamp: 2026-11-24T23:55:18+08:00
resource: include/mcp/detail/AtomicJsonFile.hpp
---

# 存储与原子写入

## AtomicJsonFile（[AtomicJsonFile.hpp](../../include/mcp/detail/AtomicJsonFile.hpp)）

- 两个重载共用同一写入内核：临时文件名为 `<path>.tmp.<pid>.<counter>.<salt>`（pid + 进程内静态自增计数器 + 随机 salt，多进程与同进程并发均不踩踏，[AtomicJsonFile.cpp](../../src/detail/AtomicJsonFile.cpp)），以 `CREATE_NEW`/`O_CREAT|O_EXCL|O_NOFOLLOW` 独占创建、no-follow，创建即收紧权限（Windows SDDL `D:P(A;;FA;;;OW)` 仅所有者 ACL / POSIX `0600`）；拒绝目标或临时路径为目录、symlink 与 Windows reparse point
- 写入循环处理短写与 `EINTR`，落盘用 `FlushFileBuffers`/`fsync`；POSIX 用目录句柄 + 相对 `renameat` 替换，随后 `fsync` 父目录，**父目录刷盘失败返回 false 而不报成功**；Windows 用 `ReplaceFileW`（目标存在）或 `MoveFileExW`（目标不存在）按句柄验证后替换
- `WriteAtomic(path, contents)` / `WriteAtomic(path, JsonValue)`（后者 `Dump(2)`）：失败清理临时文件、记 Error 日志并返回 false；`WriteAtomicSecure(path, contents)`：同样内核，任一步失败清理临时文件并**抛 `McpError(InternalError)`**
- 测试注入 `SetAtomicWriteFailureInjection(stage)` 可在 Create/Write/Flush/Rename/ParentSync 五个阶段单次失败：断言目标始终为旧版或完整新版且无残留临时文件（ParentSync 失败时 rename 已完成，目标为完整新版但仍报告失败）
- `LoadJson(path)`：文件不存在返回 null；打开失败或解析出 null 抛 `runtime_error`
- 调用方须保证父目录存在

## 消费方

| 组件 | 行为 | 页 |
|------|------|----|
| FileTaskStore | 双锁 + 锁外 JSONL 追加（见下）；中间行损坏先字节一致备份 `<path>.corrupt` 再原子重建，末行截断按合法前缀原子重写，备份/重建/重写失败保留原文件并明确抛错（见下） | [/classes/file-task-store.md](../classes/file-task-store.md) |
| FileTokenCache | Windows DPAPI 加密、POSIX 明文；`WriteAtomicSecure` 创建即安全、加载前权限校验；解密失败不回落明文；DPoP 完整 P-256 key 与令牌同文件持久化，加载校验 x/y/d/曲线/公私钥匹配，损坏拒载且保留原字段不覆盖；issuer 锚点额外拒绝链接/非普通文件/越权权限/超长/控制字符 | [/classes/file-token-cache.md](../classes/file-token-cache.md) |
| FileEventStore | SSE 事件 JSONL 持久化（跨进程文件锁，见下） | [/classes/file-event-store.md](../classes/file-event-store.md) |

## FileEventStore 事件持久化

`FileEventStore : EventStore`（[FileEventStore.hpp](../../include/mcp/storage/FileEventStore.hpp)），SSE 断线回放事件的参考实现（详见 [/classes/file-event-store.md](../classes/file-event-store.md)）：

- 每会话一个 JSONL 文件（`<dir>/sess-<净化id>.jsonl`，行 `{"id":N,"data":"..."}`）+ 一个 `.lock` 锁文件；旁路 `.meta.json` 只存 `next_id`/`count` 作加速缓存
- **跨进程文件锁**：每操作独立句柄阻塞排他锁（Win32 `LockFileEx` / POSIX `flock`），同进程多线程与其他进程在同一会话上串行
- 读取边界：缺失或不可读 JSONL 文件视为空存储；坏行或缺少字段的尾行跳过，读取不会因为单条损坏记录抛出
- 写入边界：会话锁内**每次追加都扫描 JSONL** 校验 `next_id`/`count`；日志缺失、截断、裁剪、cursor 缺失/损坏/超前/落后或追加成功但 cursor 未落盘时一律从日志重建；`max_id` 触及 `uint64` 上限时拒绝追加；目录创建、文件锁、追加或 `Clear` 失败抛异常；cursor 落盘失败只记日志（下次从日志重建，不产生重复 id）；超过 `kMaxEventsPerSession`（10000）事件经 `WriteAtomic` 全量重写裁剪，写失败抛错

## SessionStore 会话记录抽象

`SessionStore`（[SessionStore.hpp](../../include/mcp/http/SessionStore.hpp)）：`Save/Load/Remove` 会话记录 `SessionRecord{protocol_version, created_at_ms, last_seen_ms}`，键为会话 id。`last_seen_ms` 为 0 时按写入时刻回填（`TrySave` 内 `stored.last_seen_ms = now`）；`InMemorySessionStore`（mutex map）为默认实现，闲置超 `kSessionIdleTimeoutMs`（30 分钟，[Limits.hpp](../../include/mcp/transport/detail/Limits.hpp)）即过期——`TrySave` 先 `SweepExpiredLocked` 再判满（满回 false，`Save` 转抛 `runtime_error("session store is full")`，上限 `kMaxHttpSessions = 10000`），`Load` 命中即刷新 `last_seen_ms = now`、过期则删除并回 `nullopt`；`Size()` 返回当前 map 规模，`SweepExpired()` 返回本轮清除条数（基类默认均回 0）。stateful 模式注入 `StreamableHttpServerOptions::session_store` 后会话可被共享 store 的其他实例接管（[/transports/streamable-http.md](../transports/streamable-http.md)），未知会话 id 回 404 + `-32009`。

## FileTaskStore 双锁结构

`write_mutex_`（`std::mutex`，写方法全程持有，含写盘与回滚）+ `data_mutex_`（`std::shared_mutex`，读 `shared_lock` / 写 `unique_lock`）；**锁序固定 `write_mutex_` → `data_mutex_`**（[FileTaskStore.hpp](../../include/mcp/storage/FileTaskStore.hpp)）。

写方法（CreateTask/UpdateTask/CancelTask/SetTaskStatus/FailTask/SetTaskOwner/SetTaskTtl/AdvanceTaskWithInput/ParkTaskForInput/BindTaskTool）流程：

1. 锁内检查（任务不存在或已终态返回 `false`；重复创建抛 `McpError(InvalidParams)`）
2. 内存提交 → 释放 `data_mutex_` → **锁外** `AppendRecord`（单条 JSONL 追加 + flush，追加句柄先关作用域再计数；超过阈值自动 `CompactLocked` 压缩为 `WriteAtomic` 全量快照；压缩失败只记日志并重置计数，不影响本次追加成功）
3. 持久化失败 → 重取 `data_mutex_` 回滚内存修改 → 抛 `McpError(InternalError)`

读（`GetTask`/`GetAllTasks`）在写盘 I/O 期间**不阻塞**——同一时刻至多一个写者持锁写盘，读者并发通过。

## 失败语义约定

- `UpdateTask/CancelTask/SetTaskStatus/FailTask/ParkTaskForInput` 返回 `false` 表示任务**不存在或已终态**（不是持久化失败——持久化失败抛 `McpError(InternalError)`）；`CreateTask` 不返回 bool（返回 `TaskState`，重复创建抛 `McpError(InvalidParams)`）
- `FileTaskStore` 的压缩失败是 **best-effort**：`AppendRecord` 只记 Error 日志并把 `pending_records_` 归零，仍返回 `true`；只有追加本身（打开/写入/flush）失败才回滚内存并抛 `McpError(InternalError)`；加载期的损坏备份、恢复重建与截断重写失败则**明确抛错**，不返回伪成功，也不以空库覆盖证据
- token 刷新失败不回退旧 token
- `FileTokenCache` 的机密写入经 `WriteAtomicSecure`（失败抛 `McpError(InternalError)`，向 `StoreTokens`/`Save` 调用方传播）；析构内的 `Save` 捕获异常仅记 Error，避免进程终止；加载前校验文件属主与权限（组/其他可读即拒绝加载）

## 相关页面

- [/modules/server.md](../modules/server.md) — 服务端存储集成
- [/classes/file-event-store.md](../classes/file-event-store.md) — 事件存储实现
- [/classes/file-token-cache.md](../classes/file-token-cache.md) — 令牌缓存失败语义
- [/concepts/oauth.md](oauth.md) — 令牌生命周期
- [/tests.md](../tests.md) — 损坏处理测试
