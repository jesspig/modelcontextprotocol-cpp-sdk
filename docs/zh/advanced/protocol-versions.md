# 协议版本

SDK 通过双 `WireCodec` 架构支持两个 MCP 协议时代。

## 版本对比

| 版本 | 状态 | 关键特性 |
|-------------|---------|--------------|
| 2024-11-05 | 旧版 | 原始规范 |
| 2025-03-26 | 旧版 | 稳定握手 |
| 2025-06-18 | 旧版 | 中间版本 |
| 2025-11-25 | 旧版 | `initialize` 握手，独立的服务器到客户端请求 |
| 2026-07-28 | 当前 | `server/discover`、每请求 `_meta`、MRTR、`subscriptions/listen` |

## WireCodec

`WireCodec` 工厂通过字符串比较自动选择正确的编解码器：

```cpp
auto codec = MakeWireCodec("2026-07-28");
// 如果版本 >= "2026-07-28"，返回 Rev2026Codec
// 否则回退至 Rev2025Codec 用于旧版本
```

`SetNegotiatedProtocolVersion(version)` 同时存储版本号并通过 `MakeWireCodec(version)` 重建 `WireCodec`，在 `Rev2025Codec`（无 `_meta` 信封）和 `Rev2026Codec`（每请求 `_meta`）之间切换。

注意 `HandleDiscover` 返回 `{"2025-11-25", "2026-07-28"}` 但**不**调用 `SetNegotiatedProtocolVersion` —— 现代客户端通过 `_meta.protocolVersion` 按请求驱动版本选择。

### 时代之间的关键差异

| 方面 | 2025-11-25 | 2026-07-28 |
|--------|-----------|------------|
| 连接 | `initialize` 握手 | `server/discover` 每请求 `_meta` |
| 能力 | 一次性协商 | 通过 `_meta` 每请求指定 |
| 采样 | 独立请求 | 已移除（使用 Elicitation） |
| 日志 | `logging/setLevel` RPC | 每请求 `_meta.logLevel` |
| 订阅 | `subscribe`/`unsubscribe` | `subscriptions/listen` 流 |
| 错误码 | 直接值 | 重新映射（`-32001` → `-32020` 等） |
| 结果 | 普通 JSON（恒等编解码） | 带 `resultType` 字段的类型化结果（自动标记 `"complete"`） |
| `_meta` 验证 | 不需要 | 除 `server/discover` 外所有请求必须携带 |

### 2026 时代的 Wire 验证

`Rev2026Codec::ValidateRequest` 拒绝缺少 `_meta` 信封的请求，`server/discover` 除外（它是引导调用）。这强制实现了无状态协议设计。

### 结果编码

- **2025 时代**：`EncodeResult`/`DecodeResult` 为恒等操作——原始 JSON 直接通过。
- **2026 时代**：`EncodeResult` 自动在结果中标记 `resultType: "complete"`（如果尚未存在）。`resultType` 字段使下游能够区分正常结果和 `input_required`（MRTR）结果。

### IncomingRequestMeta

`IncomingRequestMeta` 结构体从 2026 时代的 `_meta` 信封中提取以下字段：

| 字段 | `_meta` 键 |
|-------|-------------|
| `protocol_version` | `io.modelcontextprotocol/protocolVersion` |
| `client_info` | `io.modelcontextprotocol/clientInfo` |
| `client_capabilities` | `io.modelcontextprotocol/clientCapabilities` |
| `log_level` | `io.modelcontextprotocol/logLevel` |
| `progress_token` | `progressToken` |
| `subscription_id` | `io.modelcontextprotocol/subscriptionId` |

`StampOutgoingMeta` 辅助函数将 `protocolVersion`、`clientInfo`、`clientCapabilities` 和 `logLevel` 标记到传出请求的 `_meta` 中。

### 按时代划分的方法

编解码器定义了每个时代的方法集合：

| 集合 | 方法 |
|-----|---------|
| 公共（两个时代） | `tools/list`、`tools/call`、`resources/list`、`resources/read`、`resources/templates/list`、`prompts/list`、`prompts/get`、`completion/complete`、`elicitation/create` |
| 仅 2025 | `ping`、`initialize`、`resources/subscribe`、`resources/unsubscribe`、`logging/setLevel`、`roots/list`、`sampling/createMessage` |
| 仅 2026 | `server/discover`、`server/extensions/list`、`subscriptions/listen`、`tasks/get`、`tasks/update`、`tasks/cancel` |

### 按时代划分的通知

| 集合 | 通知 |
|-----|---------------|
| 公共（两个时代） | `notifications/cancelled`、`notifications/progress`、`notifications/resources/updated`、`notifications/resources/list_changed`、`notifications/tools/list_changed`、`notifications/prompts/list_changed`、`notifications/subscriptions/acknowledged` |
| 仅 2025 | `notifications/initialized`、`notifications/message`、`notifications/roots/list_changed`、`notifications/elicitation/complete` |
| 仅 2026 | `notifications/tasks/status`、`notifications/tasks/working`、`notifications/tasks/completed`、`notifications/tasks/failed`、`notifications/tasks/cancelled`、`notifications/tasks/input_required` |

### 订阅系统

2026 时代的订阅系统使用 `SubscriptionFilter` 声明兴趣：

```cpp
struct SubscriptionFilter {
    std::optional<bool> tools_list_changed;
    std::optional<bool> prompts_list_changed;
    std::optional<bool> resources_list_changed;
    std::vector<std::string> resource_subscriptions;
};
```

客户端通过 `subscriptions/listen` 发送过滤器；服务端通过 `AddSubscription`/`AddSubscriptionEntry` 跟踪订阅条目，并通过 `NotifySubscribers` 分发通知，根据每个订阅的过滤器匹配通知类型。通知的 `_meta` 中包含 `io.modelcontextprotocol/subscriptionId`。

### 语义辅助函数

`McpSession` 和 `McpSessionHandler` 都提供：

```cpp
bool IsJuly2026OrLater() const;
// 如果 negotiated_version_ >= "2026-07-28" 返回 true
```

用于在应用代码中控制协议时代相关的行为。

## 协议基础设施

### MessageChannel

`MessageChannel` 提供了基于 `std::queue`、`std::mutex` 和 `std::condition_variable` 的有界异步消息队列，支持背压：

- `AsyncReceive(callback)` — 阻塞直到消息到达或通道关闭
- `Send(message)` — 缓冲区满时阻塞（背压）
- `TrySend(message)` — 非阻塞发送
- `Close()` — 唤醒所有等待者

由 `McpSessionHandler` 用于异步消息循环。

### MessageFilter 管道

`FilterPipeline` 链式组合多个 `MessageFilter` 实例，用于拦截（认证、审计、限流、请求修改）：

```cpp
auto pipeline = std::make_shared<FilterPipeline>();
pipeline->AddFilter(std::make_shared<IncomingMessageFilter>(
    [](const JsonRpcMessage& msg, MessageFilterNext next) {
        // 检查/修改，然后调用 next(filtered) 或短路
        next(msg);
    }));
```

入站过滤器包装处理程序分发；出站过滤器包装传输层发送。两者都是可选的，通过 `ServerOptions::incoming_filters` / `outgoing_filters` 配置。

### X-Mcp-Header 注解

`XMcpHeaders.hpp` 为 JSON Schema 中的 `x-mcp-header` 注解提供辅助函数：

- `ScanXMcpHeaders(schema)` — 从 `x-mcp-header` 声明中提取 `paramName → headerName` 映射
- `ExtractXMcpHeaderValues(params, decls)` — 从参数中提取基本类型（字符串、整数、布尔值）的头部值

## 错误码重新映射（2026 时代）

| 代码 | 名称 | 2025 值 | 2026 值 |
|------|------|-----------|-----------|
| HeaderMismatch | 头部不匹配 | -32001 | -32020 |
| MissingRequiredClientCapability | 缺少必要能力 | -32003 | -32021 |
| UnsupportedProtocolVersion | 版本不匹配 | -32004 | -32022 |
