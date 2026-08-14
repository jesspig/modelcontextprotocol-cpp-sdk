# 协议版本

SDK 通过双 `WireCodec` 架构支持两个 MCP 协议时代。

## 版本对比

| 版本 | 状态 | 关键特性 |
|-------------|---------|--------------|
| 2024-10-07 | 旧版 | 原始规范 |
| 2024-11-05 | 旧版 | 原始规范修订 |
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

`Rev2026Codec::ValidateRequest` 执行两项检查：
1. **时代成员资格**：如果方法不在 2026 时代的方法集合中，返回 `NotInEra`。
2. **`_meta` 存在性**：如果请求（`server/discover` 除外）缺少 `_meta` 信封，返回 `Invalid`。

这强制实现了无状态协议设计，其中 `server/discover` 是唯一不需要前置上下文的引导调用。

### 响应验证（2026 时代）

`Rev2026Codec::ValidateResponse` 验证出站响应：
1. **`resultType` 字段**：所有响应必须包含 `resultType`。缺失则返回 `Invalid`。
2. **列表方法**：对于 `tools/list`、`resources/list`、`resources/templates/list`、`prompts/list`，`resultType` 必须为 `"complete"`。列表结果不能是部分/input_required。

### 通知验证（2026 时代）

`Rev2026Codec::ValidateNotification` 确保通知不包含 `id`、`result` 或 `error` 字段——只允许 `method` 和 `params`。

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
| `traceparent`     | `traceparent` |
| `tracestate`      | `tracestate` |
| `baggage`         | `baggage` |

出站请求的 `_meta` 由 `McpSessionHandler::SendRequest` 通过 `SerializeRequestMeta` 写入，序列化 `protocolVersion`、`clientInfo` 和 `clientCapabilities`（`WireCodec::StampOutgoingRequest` 同样只 stamp 这三个字段，且无调用点）。追踪字段（`traceparent`、`tracestate`、`baggage`）仅在请求元数据中显式设置时才会被序列化。

### 按时代划分的方法

编解码器定义了每个时代的方法集合：

| 集合 | 方法 |
|-----|---------|
| 公共（两个时代） | `tools/list`、`tools/call`、`resources/list`、`resources/read`、`resources/templates/list`、`prompts/list`、`prompts/get`、`completion/complete` |
| 仅 2025 | `initialize`、`ping`、`resources/subscribe`、`resources/unsubscribe`、`logging/setLevel`、`roots/list`、`sampling/createMessage`、`elicitation/create`、`tasks/get`、`tasks/update`、`tasks/cancel`、`tasks/result`、`tasks/list` |
| 仅 2026 | `server/discover`、`subscriptions/listen` |

注意 `ping`、`elicitation/create` 与全部 `tasks/*` 方法仅属于 2025 时代：2026 时代不提供这些方法（`server/extensions/list` 不属于任何时代集合）。

### 按时代划分的通知

| 集合 | 通知 |
|-----|---------------|
| 公共（两个时代） | `notifications/cancelled`、`notifications/progress`、`notifications/message`、`notifications/resources/updated`、`notifications/resources/list_changed`、`notifications/tools/list_changed`、`notifications/prompts/list_changed` |
| 仅 2025 | `notifications/initialized`、`notifications/roots/list_changed`、`notifications/elicitation/complete`、`notifications/tasks/status` |
| 仅 2026 | `notifications/subscriptions/acknowledged` |

其余 `notifications/tasks/working`、`notifications/tasks/completed`、`notifications/tasks/failed`、`notifications/tasks/cancelled`、`notifications/tasks/input_required` 通知常量仍保留（`Methods.hpp`），但不在任何时代的集合中。

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
pipeline->AddFilter(std::make_shared<MessageFilterFuncAdapter>(
    [](const JsonRpcMessage& msg, MessageFilterNext next) {
        // 检查/修改，然后调用 next(filtered) 或短路
        next(msg);
    }));
```

入站过滤器包装处理程序分发；出站过滤器包装传输层发送。两者都是可选的，通过 `ServerOptions::incoming_filters` / `outgoing_filters` 配置。

## 错误码重新映射（2026 时代）

`Rev2026Codec::EncodeErrorCode` 将 2025 时代的错误码重新映射为 2026 时代的值：

| 错误 | 2025 值 | 2026 值 |
|-------|-----------|-----------|
| `RequestTimeout` → `HeaderMismatch` | -32001 | -32020 |
| `ConnectionRefused` → `MissingRequiredClientCapability` | -32003 | -32021 |
| `TlsHandshakeFailed` → `UnsupportedProtocolVersion` | -32004 | -32022 |

其他所有错误码原样传递。
