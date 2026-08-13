---
type: Test Suite
title: 测试体系
description: 14 个 gtest 目标（unit/integration/conformance），278 个测试用例。
tags: [test, gtest, ctest, conformance]
timestamp: 2026-08-13T12:36:14+08:00
resource: tests/CMakeLists.txt
---

# 测试体系

全部经 `gtest_discover_tests()` 注册到 ctest。用例/断言数为静态统计（行首 `TEST(_F|_P)?` 宏与 `EXPECT_|ASSERT_` 宏计数），运行期数量可能略少。

## 目标清单

| 目标 | 文件 | 用例 | 断言 |
|------|------|------|------|
| mcp-core-tests | JsonRpcTests、McpTypesTests | 41 | 100 |
| mcp-wire-codec-tests | WireCodecTests、SessionHandlerTests | 27 | 84 |
| mcp-server-tests | McpServerTests | 11 | 22 |
| mcp-client-tests | McpClientTests | 15 | 43 |
| mcp-oauth-tests | OAuthTests | 16 | 33 |
| mcp-transport-tests | TransportTests | 6 | 18 |
| mcp-http-tests | HttpServerTests | 11 | 44 |
| mcp-message-filter-tests | MessageFilterTests | 5 | 12 |
| mcp-token-cache-tests | FileTokenCacheTests | 5 | 19 |
| mcp-task-store-tests | FileTaskStoreTests | 9 | 29 |
| mcp-streamable-http-tests | StreamableHttpTransportTests | 11 | 18 |
| mcp-websocket-tests | WebSocketTransportTests | 2 | 5 |
| mcp-integration-tests | ClientServerRoundTrip | 8 | 21 |
| mcp-conformance-tests | ProtocolConformance | 111 | 316 |
| **合计** | | **278** | **764** |

## 测试注意点

- `InMemoryTransport` 是**同步**的：消息在 `Send()`/`AsyncReceive()` 时交付（`MessageChannel` 有界队列，默认 64），无外部事件循环
- 集成测试的 `RunWithTimeout`：body 挂起超 10s 会 `std::_Exit(1)` 使进程直接失败（快于永久阻塞）
- 关键协议行为有盲区守护测试：`RejectsRequestsBeforeInitialized`、`InitializeEchoesClientVersion`（回显旧版版本号）、`ProgressNotificationExtendsDeadline`、`IncomingFilterInterceptsRequests`
- `WireCodec::ValidateResponse`/`StampOutgoingRequest` 生产代码无调用者但**有测试守护**——不是死代码，勿删；`WireCodec::ExtractIncomingMeta` 已不再有派生实现（基类默认返回 nullopt，WireCodec 版测试 `Rev2026ExtractMeta` 已删）——meta 提取现由 `McpSessionHandler::ExtractIncomingMeta` 承担（有真实调用者），守护测试为 `IncomingMetaCarriesProtocolVersion`
- 2026 时代相关新增测试：`PingRejectedIn2026`/`PingAvailableIn2025`（SessionHandler）、`Rev2025ValidateInitializeRequest( +MissingProtocolVersion)`（WireCodec）、`Rev2026EncodeResultFlattensCacheHint`/`Rev2025EncodeResultKeepsCacheHintNested`、`AutoNegotiationCorrectsVersionOnSharedVersion`/`AutoNegotiationFallsBackWhenOnlyLegacySupported`（McpClient）；旧名 `Rev2026HasTaskAndSubscriptionNotifications` 已更名 `Rev2026HasMessageAndSubscriptionNotificationsNoTasks`

## 相关页面

- [/build.md](/build.md) — 构建与运行测试
- [/modules/protocol.md](/modules/protocol.md) — 测试守护的核心行为
- [/transports/in-memory.md](/transports/in-memory.md) — 同步测试传输
