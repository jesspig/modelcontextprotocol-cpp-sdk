# 服务端概述

`McpServer` 类向连接的 MCP 客户端暴露能力（工具、资源、提示）。

## 创建服务端

```cpp
asio::io_context io_ctx;
auto transport = std::make_shared<StdioServerTransport>(io_ctx);

ServerOptions opts;
opts.server_info = Implementation{"MyServer", "1.0.0"};
opts.capabilities = ServerCapabilities{};
opts.capabilities->tools = ToolsCapability{};

auto server = McpServer::Create(std::move(transport), opts, &io_ctx);
server->Run();
```

对于 Streamable HTTP 模式：

```cpp
asio::io_context io_ctx;

StreamableHttpServerOptions http_opts;
http_opts.port = 3001;
http_opts.stateless = true;  // 或 false 使用会话模式

auto transport = std::make_shared<StreamableHttpServerTransport>(io_ctx, http_opts);

ServerOptions opts;
opts.server_info = Implementation{"MyServer", "1.0.0"};
opts.capabilities = ServerCapabilities{};
opts.capabilities->tools = ToolsCapability{};

auto server = McpServer::Create(std::move(transport), opts, &io_ctx);
server->Run();
```

::: warning
`McpServer::Create` 必须与传输层共享同一个 `asio::io_context`。将 io_context 作为第三个参数传入。
:::

## ServerOptions

| 字段 | 类型 | 描述 |
|-------|------|-------------|
| `server_info` | `optional<Implementation>` | 服务端标识（名称、版本） |
| `capabilities` | `optional<ServerCapabilities>` | 声明的能力 |
| `protocol_version` | `optional<string>` | 固定到特定协议版本 |
| `server_instructions` | `optional<string>` | 发送给客户端的指令 |
| `initialization_timeout` | `chrono::seconds` | 握手超时时间（默认 60s） |
| `validate_tool_input` | `bool` | 启用 JSON Schema 输入验证 |
| `validate_tool_output` | `bool` | 启用 JSON Schema 输出验证 |
| `task_store` | `shared_ptr<IMcpTaskStore>` | 任务持久化后端 |
| `request_state_verifier` | `function<bool(string_view)>` | MRTR 的 HMAC/AEAD 验证器 |
| `cache_hints` | `optional<map<string, CacheHint>>` | 按方法的缓存提示（ttlMs, cacheScope） |
| `input_required_config` | `optional<InputRequiredConfig>` | MRTR/elicitation 行为的配置 |
| `input_required_config.max_rounds` | `int` | 最大启发式收集轮次（默认 8） |
| `input_required_config.round_timeout` | `chrono::seconds` | 每轮超时时间（默认 600s） |
| `input_required_config.legacy_shim` | `bool` | 启用旧版兼容垫片（默认 true） |
| `on_request` | `function` | 收到每个入站 JSON-RPC 请求时回调，包含方法名和完整请求体 |
| `on_response` | `function` | 发送每个出站 JSON-RPC 响应时回调 |
| `on_error` | `function` | 发送每个出站 JSON-RPC 错误时回调 |
| `on_notification` | `function` | 收到每个入站 JSON-RPC 通知时回调 |
| `on_method_called` | `function(string_view)` | 简写——仅方法名（与 `on_request` 同时触发） |
| `on_protocol_error` | `function(string_view)` | 简写——仅错误消息（与 `on_error` 同时触发） |
| `on_client_connected` | `function(Implementation)` | 客户端完成 `initialize` 时回调 |
| `on_initialized` | `function()` | 客户端发送 `notifications/initialized` 时回调 |
| `on_transport_close` | `function()` | 传输连接关闭时回调 |
| `on_transport_error` | `function(string_view)` | 传输层错误时回调 |
| `incoming_filters` | `shared_ptr<FilterPipeline>` | 用于拦截/修改/阻断入站消息的管道 |
| `outgoing_filters` | `shared_ptr<FilterPipeline>` | 用于拦截/修改/阻断出站消息的管道 |

简写回调（`on_method_called`、`on_protocol_error`）和完整消息回调（`on_request`、`on_error`）为**链式触发** — 同时设置时两者都会触发。

## 生命周期

1. **构造**：`McpServer::Create` — 创建会话处理器、挂载处理器、启动消息循环
2. **注册**：注册工具、资源、提示、扩展
3. **运行**：`server->Run()` — 阻塞于 `io_context.run()`
4. **关闭**：`server->Close()` — 取消待处理请求、关闭传输层

## 能力推导

能力从注册的原语中自动推导。例如，注册工具会设置 `capabilities.tools.list_changed = true`。`ServerOptions.capabilities` 字段已声明但当前未被服务端消费——能力始终从已注册的工具、资源、提示和任务存储自动推导。
