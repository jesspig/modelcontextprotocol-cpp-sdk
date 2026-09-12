---
type: Class
title: FileEventStore
description: EventStore 的 JSONL 文件持久化实现：跨进程文件锁、崩溃残留容错、1024 事件裁剪。
tags: [storage, event-store, sse, 文件锁, 持久化]
timestamp: 2026-09-12T06:05:00+08:00
resource: include/mcp/storage/FileEventStore.hpp
---

# FileEventStore

`class FileEventStore : public EventStore`（[FileEventStore.hpp](../../include/mcp/storage/FileEventStore.hpp)，实现 [FileEventStore.cpp](../../src/http/FileEventStore.cpp)），SSE 断线回放事件的文件持久化参考实现。构造参数为存储目录（不存在则创建，失败抛 `runtime_error`）；注入 `StreamableHttpServerOptions::event_store` 后重启/多实例均可回放。

`EventStore` 基类（[EventStore.hpp](../../include/mcp/http/EventStore.hpp)）的三个方法已虚化（虚析构），内存实现保留为基类默认——`FileEventStore` 是第一个可插拔实现。

## 持久化布局

- 每会话两个文件：`<storage_dir>/sess-<id>.jsonl`（事件）与 `<storage_dir>/sess-<id>.lock`（锁）
- 会话 id **双射净化**为安全文件名成分：`[A-Za-z0-9_-]` 透传，其余字节转义为 `~hh`（防路径穿越与非法字符）
- 每行 `{"id":N,"data":"..."}`，id 为会话内单调递增（`Append` 时重读文件取最大 id + 1，天然支持多实例续号）

## 跨进程文件锁

`SessionFileLock`：每次操作打开独立句柄，对 `.lock` 文件取**阻塞排他锁**——Windows `LockFileEx`（OVERLAPPED 事件等待），POSIX `flock(LOCK_EX)`（EINTR 重试）。同进程多线程与其他进程在同一会话上串行；锁为 RAII，析构释放。

`Append`/`GetEventsSince`/`Clear` 全程持锁；打开/加锁失败抛 `runtime_error`（错误信息含路径）。

## 失败与容错语义

- **崩溃残留（torn tail）**：读取时跳过无法 JSON 解析或缺键的行；追加时若文件不以换行结尾先补 `\n`（损坏尾行被自然隔断，不会污染新事件）
- 文件不存在/不可读 → 空存储（`GetEventsSince` 返回空，不抛）
- **trim**：事件数超过 `kMaxEventsPerSession`（1024）时保留尾部，经 `detail::WriteAtomic` 全量重写（原子替换，见 [/concepts/storage.md](../concepts/storage.md)）；写失败抛异常
- `Clear` 删除会话文件（删除失败抛异常）；锁文件不清除（后续操作复用）

## 构建注意

`src/http/FileEventStore.cpp` 在 unity TU 中**必须排在末尾**：其 `windows.h` 依赖前面文件已建立的 winsock 包含顺序（CMakeLists 有注释固化该约束）。

## 测试

`mcp-event-store-tests`（[FileEventStoreTests.cpp](../../tests/unit/FileEventStoreTests.cpp)）10 用例：双实例共享、进程重启恢复、并发追加 id 单调、trim、净化文件名、损坏尾行容错等。

## 相关页面

- [/modules/http.md](../modules/http.md) — 所属库
- [/concepts/storage.md](../concepts/storage.md) — 原子写入与 SessionStore
- [/transports/streamable-http.md](../transports/streamable-http.md) — GET 流 Last-Event-ID 回放
- [/tests.md](../tests.md) — mcp-event-store-tests
