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
| `cache_hints` | `bool` | 启用 CacheHint 支持（默认 false） |
| `input_required_config` | `InputRequiredConfig` | MRTR/elicitation 行为的配置 |

## 生命周期

1. **构造**：`McpServer::Create` — 创建会话处理器、挂载处理器、启动消息循环
2. **注册**：注册工具、资源、提示、扩展
3. **运行**：`server->Run()` — 阻塞于 `io_context.run()`
4. **关闭**：`server->Close()` — 取消待处理请求、关闭传输层

## 能力推导

能力从注册的原语中自动推导。例如，注册工具会设置 `capabilities.tools.list_changed = true`。手动 `ServerOptions.capabilities` 覆盖可用于补充自动推导的值。
