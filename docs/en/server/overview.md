# Server Overview

The `McpServer` class exposes capabilities (tools, resources, prompts) to connected MCP clients.

## Creating a Server

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
`McpServer::Create` must share the same `asio::io_context` as the transport. Pass io_context as the third argument.
:::

## ServerOptions

| Field | Type | Description |
|-------|------|-------------|
| `server_info` | `optional<Implementation>` | Server identity (name, version) |
| `capabilities` | `optional<ServerCapabilities>` | Declared capabilities |
| `protocol_version` | `optional<string>` | Pin to a specific version |
| `server_instructions` | `optional<string>` | Instructions sent to client |
| `initialization_timeout` | `chrono::seconds` | Handshake timeout (default 60s) |
| `validate_tool_input` | `bool` | Enable JSON Schema input validation |
| `validate_tool_output` | `bool` | Enable JSON Schema output validation |
| `task_store` | `shared_ptr<IMcpTaskStore>` | Task persistence backend |
| `request_state_verifier` | `function<bool(string_view)>` | HMAC/AEAD verifier for MRTR |

## Lifecycle

1. **Construction**: `McpServer::Create` — creates the session handler, wires handlers, starts message loop
2. **Registration**: Register tools, resources, prompts, extensions
3. **Run**: `server->Run()` — blocks on `io_context.run()`
4. **Shutdown**: `server->Close()` — cancels pending requests, closes transport

## Capability Derivation

Capabilities are automatically derived from registered primitives. For example, registering a tool sets `capabilities.tools.list_changed = true`. Manual `ServerOptions.capabilities` overrides can supplement auto-derived values.
