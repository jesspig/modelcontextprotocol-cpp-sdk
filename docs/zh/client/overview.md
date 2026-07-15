# 客户端概述

`McpClient` 类连接 MCP 服务器，协商协议版本，并调用服务器能力。

## 创建客户端

```cpp
auto transport = std::make_shared<StdioClientTransport>("path/to/server");
ClientOptions opts;
opts.client_info = Implementation{"MyClient", "1.0.0"};

auto client = McpClient::Create(transport, opts);
```

## ClientOptions

| 字段 | 类型 | 描述 |
|-------|------|-------------|
| `client_info` | `Implementation` | 客户端标识 |
| `capabilities` | `optional<ClientCapabilities>` | 声明的能力 |
| `connect_mode` | `ConnectMode` | `Auto`（发现 → 初始化）、`Legacy`、`Pin` |
| `initialization_timeout` | `chrono::seconds` | 握手超时 |

## 发起请求

```cpp
// 列出工具
auto tools = client->ListTools();

// 调用工具
auto result = client->CallTool("echo",
    nlohmann::json{{"text", "Hello"}});

// 读取资源
auto resource = client->ReadResource("file:///config.json");

// 获取提示词
auto prompt = client->GetPrompt("code_review",
    nlohmann::json{{"diff", "..."}});

// 补全提示词/资源引用
auto completion = client->Complete(params);

// Ping（心跳）
client->Ping();
```

## 服务端到客户端处理器

注册用于处理服务器发起请求的处理器：

```cpp
client->SetElicitationHandler(
    [](const ElicitRequestParams& params) -> ElicitResult {
        // 提示用户输入，返回结果
        ElicitResult result;
        result.values = nlohmann::json{{"name", "Alice"}};
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
```

## 订阅

```cpp
// 订阅服务器通知（2026 时代）
SubscriptionsListenRequestParams subs;
subs.notifications.tools_list_changed = true;
subs.notifications.resources_list_changed = true;
client->SubscribeAsync(subs);
```

## 版本协商

客户端自动协商协议版本：

1. **Auto**（默认）：探测 `server/discover`，回退至 `initialize` 握手
2. **Pin**：强制指定版本
3. **Legacy**：仅使用 `initialize` 握手
