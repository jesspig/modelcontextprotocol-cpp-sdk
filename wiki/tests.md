---
type: Test Suite
title: 测试体系
description: 16 个测试目标（13 unit + integration/conformance/framework），521 个测试用例（ctest 注册口径，自研测试框架）。
tags: [test, framework, ctest, conformance]
timestamp: 2026-09-11T08:40:00+08:00
resource: tests/CMakeLists.txt
---

# 测试体系

全部经自研 `mcp_discover_tests()`（tests/framework 框架）逐用例注册进 ctest。用例数为 **ctest 注册口径**（`ctest -N` 实测 521，与静态 `^TEST` 宏计数一致）；断言数为静态统计（`EXPECT_|ASSERT_` 宏计数）。

## 目标清单

| 目标 | 文件 | 用例 | 断言 |
|------|------|------|------|
| mcp-core-tests | JsonRpcTests、McpTypesTests、JsonParserTests、LogTests | 120 | 173 |
| mcp-wire-codec-tests | WireCodecTests、SessionHandlerTests | 42 | 164 |
| mcp-server-tests | McpServerTests | 34 | 178 |
| mcp-client-tests | McpClientTests | 50 | 226 |
| mcp-oauth-tests | OAuthTests | 16 | 33 |
| mcp-transport-tests | TransportTests、MessageChannelTests | 11 | 36 |
| mcp-net-tests | NetStackTests、WebSocketClientTests | 25 | 52 |
| mcp-http-tests | HttpServerTests | 27 | 113 |
| mcp-message-filter-tests | MessageFilterTests | 5 | 12 |
| mcp-token-cache-tests | FileTokenCacheTests | 5 | 19 |
| mcp-task-store-tests | FileTaskStoreTests | 9 | 29 |
| mcp-streamable-http-tests | StreamableHttpTransportTests | 26 | 83 |
| mcp-websocket-tests | WebSocketTransportTests | 2 | 5 |
| mcp-integration-tests | ClientServerRoundTrip、Phase1InteropTests、Phase2ClosureTests | 19 | 112 |
| mcp-conformance-tests | ProtocolConformance | 116 | 336 |
| mcp-framework-self-tests | SelfTests | 14 | 30 |
| **合计** | | **521** | **1601** |

## 测试注意点

- `InMemoryTransport` 是**同步**的：消息在 `Send()`/`AsyncReceive()` 时交付（`MessageChannel` 有界队列，默认 64），无外部事件循环
- 集成测试的 `RunWithTimeout`：body 挂起超 10s 会 `std::_Exit(1)` 使进程直接失败（快于永久阻塞）
- 关键协议行为有盲区守护测试：`RejectsRequestsBeforeInitialized`、`InitializeEchoesClientVersion`（回显旧版版本号）、`ProgressNotificationExtendsDeadline`、`IncomingFilterInterceptsRequests`
- `WireCodec::ValidateResponse`/`StampOutgoingRequest` 生产代码无调用者但**有测试守护**——不是死代码，勿删；`WireCodec::ExtractIncomingMeta` 已不再有派生实现（基类默认返回 nullopt，WireCodec 版测试 `Rev2026ExtractMeta` 已删）——meta 提取现由 `McpSessionHandler::ExtractIncomingMeta` 承担（有真实调用者），守护测试为 `IncomingMetaCarriesProtocolVersion`
- 2026 时代相关新增测试：`PingRejectedIn2026`/`PingAvailableIn2025`（SessionHandler）、`Rev2025ValidateInitializeRequest( +MissingProtocolVersion)`（WireCodec）、`Rev2026EncodeResultFlattensCacheHint`/`Rev2025EncodeResultKeepsCacheHintNested`、`AutoNegotiationCorrectsVersionOnSharedVersion`/`AutoNegotiationFallsBackWhenOnlyLegacySupported`（McpClient）；旧名 `Rev2026HasTaskAndSubscriptionNotifications` 已更名 `Rev2026HasMessageAndSubscriptionNotificationsNoTasks`
- 第一期互操作测试（[Phase1InteropTests.cpp](../../tests/integration/Phase1InteropTests.cpp)，4 用例）：InMemory 与 HTTP 的 progress 双向（服务端 `SendProgress` → 客户端 `on_progress`）、客户端 `SendRootsListChanged`、GET SSE 接收流收 `tools/list_changed`；另与 TS conformance 服务器（2025-11-25 legacy）人工联调 7/7 通过（协商、tools/list、progress 0/50/100、roots 通知、logging 通知）
- 第二期收尾测试（[Phase2ClosureTests.cpp](../../tests/integration/Phase2ClosureTests.cpp)，7 用例）：任务全生命周期与运行中取消（InMemory/HTTP 双形态）、URL elicitation 往返（InMemory/HTTP）、`ListToolsAll` 翻页聚合、`max_total_timeout` 截断请求
- 期二关键守护测试：`RequireInitialized` era 门控（modern era 未初始化放行 / legacy 拒绝）、任务化 tools/call 立即返回 `CreateTaskResult` + 状态通知流转、`tasks/cancel` 终态不迁移、`RequestContext::IsCancellationRequested` 协作取消、404 会话自愈重放恰一次（SessionExpired）、discover `serverInfo` 容缺与 `supportedVersions` 交集、`ElicitResult` wire 键 `content`、发送侧独立 POST 与接收侧边读边分发（StreamableHttp 传输层）

## 相关页面

- [/build.md](/build.md) — 构建与运行测试
- [/modules/protocol.md](/modules/protocol.md) — 测试守护的核心行为
- [/transports/in-memory.md](/transports/in-memory.md) — 同步测试传输
