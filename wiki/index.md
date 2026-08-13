# 项目知识库

MCP C++ SDK（`mcp-cpp-sdk`）的源码知识库。基于实际源码整理，与代码保持同步；在线文档见 [docs/](../docs/)，本库与其互不冲突。

## 模块（libraries）

- [/modules/core.md](/modules/core.md) — mcp-core：JSON 值模型、JSON-RPC 消息、协议数据类型、错误码与方法常量
- [/modules/transport.md](/modules/transport.md) — mcp-transport：三态状态机、连接工厂、PlatformIO 与各传输实现
- [/modules/protocol.md](/modules/protocol.md) — mcp-protocol：JSON-RPC 引擎与双时代线协议编解码
- [/modules/server.md](/modules/server.md) — mcp-server：注册 API、能力推导、任务存储集成
- [/modules/client.md](/modules/client.md) — mcp-client：连接模式协商、OAuth 与令牌缓存
- [/modules/http.md](/modules/http.md) — mcp-http：HttpServer、EventStore、Streamable HTTP 双端

## 关键类（classes）

- [/classes/json-value.md](/classes/json-value.md) — 变体 JSON 值类型与手写序列化
- [/classes/mcp-session-handler.md](/classes/mcp-session-handler.md) — JSON-RPC 引擎：分发、超时、取消、过滤器
- [/classes/wire-codec.md](/classes/wire-codec.md) — 2025/2026 双时代编解码器
- [/classes/message-channel.md](/classes/message-channel.md) — 有界异步队列
- [/classes/mcp-server.md](/classes/mcp-server.md) — 服务端门面与四层回调接线
- [/classes/mcp-client.md](/classes/mcp-client.md) — 客户端门面：创建即协商
- [/classes/http-server.md](/classes/http-server.md) — 自研 HTTP 服务端与 SSE 广播
- [/classes/file-task-store.md](/classes/file-task-store.md) — 任务存储：双锁与锁外写盘、回滚与损坏备份
- [/classes/file-token-cache.md](/classes/file-token-cache.md) — 令牌缓存：DPAPI 与 0600

## 传输（transports）

- [/transports/stdio.md](/transports/stdio.md) — 管道传输与子进程
- [/transports/sse.md](/transports/sse.md) — SSE 客户端：重连与回放
- [/transports/in-memory.md](/transports/in-memory.md) — 同步测试传输
- [/transports/websocket.md](/transports/websocket.md) — 自研 WebSocket 客户端
- [/transports/streamable-http.md](/transports/streamable-http.md) — 2026 时代 HTTP 传输（stateless / 504）

## 概念（concepts）

- [/concepts/version-negotiation.md](/concepts/version-negotiation.md) — initialize 与 server/discover 双轨协商
- [/concepts/mrtr.md](/concepts/mrtr.md) — 多轮请求-响应与 elicitation
- [/concepts/meta-and-filters.md](/concepts/meta-and-filters.md) — _meta 元数据与过滤器管线
- [/concepts/concurrency.md](/concepts/concurrency.md) — 线程模型与 Close self-join 陷阱
- [/concepts/storage.md](/concepts/storage.md) — 原子写入与失败语义
- [/concepts/oauth.md](/concepts/oauth.md) — PKCE、RFC 9207、刷新/吊销

## 工程（build & tests）

- [/build.md](/build.md) — CMake 预设、Unity/LTO、依赖拉取
- [/tests.md](/tests.md) — 14 个 gtest 目标与守护测试

## 维护

- [/log.md](/log.md) — 更新摘要（最近 7 条）
- [/changelog/](/changelog/) — 按天的详细更新日志
