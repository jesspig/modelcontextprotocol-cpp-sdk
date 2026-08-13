# 客户端概述

`McpClient` 类连接 MCP 服务器，协商协议版本，并调用服务器能力。

## 创建客户端

```cpp
StdioClientTransportOptions transport_opts;
transport_opts.command = "path/to/server";
auto factory = std::make_shared<StdioClientTransport>(transport_opts);
auto transport = factory->Connect();
ClientOptions opts;
opts.client_info = Implementation{"MyClient", "1.0.0"};

auto client = McpClient::Create(transport, opts);
```

## ClientOptions

| 字段 | 类型 | 描述 |
|-------|------|-------------|
| `client_info` | `Implementation` | 客户端标识（默认 `{"mcp-cpp-client", "0.1.0"}`） |
| `capabilities` | `optional<ClientCapabilities>` | 声明的能力 |
| `connect_mode` | `ConnectMode` | `Auto`（发现 → 初始化）、`Legacy`、`Pin` |
| `initialization_timeout` | `chrono::seconds` | 握手超时（默认 60s） |
| `pin_protocol_version` | `optional<string>` | 固定到特定协议版本（用于 `Pin` 模式） |
| `discover_probe_timeout` | `chrono::seconds` | 服务发现探测超时（默认 5s） |
| `input_required_config` | `optional<InputRequiredConfig>` | MRTR elicitation 配置：`auto_fulfill=true`、`max_rounds=8`、`round_timeout=600s` |
| `input_required_config.max_total_timeout` | `chrono::seconds` | 整个 MRTR 流程的硬预算（默认 `0` = 不限，`round_timeout` 按轮生效） |
| `extensions` | `optional<JsonValue>` | 协议扩展声明 |

## 发起请求

```cpp
// 列出工具（可选游标用于分页）
auto tools = client->ListTools();

// 调用工具（支持可选参数、RequestOptions 和 MRTR）
auto result = client->CallTool("echo",
    JsonValue(JsonValue::Object{{"text", "Hello"}}));

// 读取资源（支持 CacheableRequestOptions）
auto resource = client->ReadResource("file:///config.json");

// 获取提示词（支持可选参数和 RequestOptions）
auto prompt = client->GetPrompt("code_review",
    JsonValue(JsonValue::Object{{"diff", "..."}}));

// 补全提示词/资源引用
auto completion = client->Complete(params);

// Ping（心跳，2026-07-28 协议版本已弃用）
client->Ping();

// 发现服务器能力（重新协商）
auto discover = client->Discover();

// 列出资源和模板
auto resources = client->ListResources();
auto templates = client->ListResourceTemplates();

// 列出提示词
auto prompts = client->ListPrompts();

// 订阅/取消订阅资源变更
client->SubscribeResource("file:///config.json");
client->UnsubscribeResource("file:///config.json");

// 任务操作
auto task = client->GetTask("task-123");
client->UpdateTask("task-123", result_json);
client->CancelTask("task-123", "不再需要");

// 轮询任务直到完成（可配置间隔和超时）
auto completed = client->PollTaskToCompletion("task-123");
```

## 服务端到客户端处理器

注册用于处理服务器发起请求的处理器：

```cpp
client->SetElicitationHandler(
    [](const ElicitRequestParams& params) -> ElicitResult {
        // 提示用户输入，返回结果
        ElicitResult result;
        result.values = JsonValue(JsonValue::Object{{"name", "Alice"}});
        return result;
    });

client->SetSamplingHandler(
    [](const CreateMessageRequestParams& params) -> CreateMessageResult {
        // 已弃用：请使用 Elicitation 替代
    });

client->SetRootsHandler(
    [](const ListRootsRequestParams& params) -> ListRootsResult {
        // 已弃用：提供根目录
    });

client->SetNotificationHandler("custom/notification",
    [](const JsonRpcNotification& notif) {
        // 处理服务器发送的通知
    });

client->SetLoggingHandler(
    [](const LoggingMessageNotificationParams& params) {
        // 处理来自服务器的日志消息
    });
```

## 订阅

```cpp
// 订阅服务器通知（2026 时代）
SubscriptionsListenRequestParams subs;
subs.notifications.tools_list_changed = true;
subs.notifications.resources_list_changed = true;
subs.notifications.resource_subscriptions = {"file:///config.json"};
client->SubscribeAsync(subs);
```

## 版本协商

客户端自动协商协议版本：

1. **Auto**（默认）：探测 `server/discover`，回退至 `initialize` 握手
2. **Pin**：强制指定版本（通过 `pin_protocol_version`，未设置时默认使用 `kLatestProtocolVersion`）
3. **Legacy**：仅使用 `initialize` 握手
