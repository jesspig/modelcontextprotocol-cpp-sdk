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

For Streamable HTTP mode:

```cpp
asio::io_context io_ctx;

StreamableHttpServerOptions http_opts;
http_opts.port = 3001;
http_opts.stateless = true;  // or false for session mode

auto transport = std::make_shared<StreamableHttpServerTransport>(io_ctx, http_opts);

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
| `cache_hints` | `optional<map<string, CacheHint>>` | Per-method cache hints (ttlMs, cacheScope) |
| `input_required_config` | `optional<InputRequiredConfig>` | Configuration for MRTR/elicitation behavior |
| `input_required_config.max_rounds` | `int` | Maximum elicitation rounds (default 8) |
| `input_required_config.round_timeout` | `chrono::seconds` | Per-round timeout (default 600s) |
| `input_required_config.legacy_shim` | `bool` | Enable legacy compatibility shim (default true) |
| `on_request` | `function` | Called on each incoming JSON-RPC request with method name + full request body |
| `on_response` | `function` | Called on each outgoing JSON-RPC response |
| `on_error` | `function` | Called on each outgoing JSON-RPC error response |
| `on_notification` | `function` | Called on each incoming JSON-RPC notification |
| `on_method_called` | `function(string_view)` | Shorthand — method name only (fires alongside `on_request`) |
| `on_protocol_error` | `function(string_view)` | Shorthand — error message only (fires alongside `on_error`) |
| `on_client_connected` | `function(Implementation)` | Called when a client completes `initialize` |
| `on_initialized` | `function()` | Called when client sends `notifications/initialized` |
| `on_transport_close` | `function()` | Called when the transport connection closes |
| `on_transport_error` | `function(string_view)` | Called on transport-level errors |
| `incoming_filters` | `shared_ptr<FilterPipeline>` | Pipeline to intercept/modify/block inbound messages |
| `outgoing_filters` | `shared_ptr<FilterPipeline>` | Pipeline to intercept/modify/block outbound messages |

Shorthand callbacks (`on_method_called`, `on_protocol_error`) and full-message callbacks (`on_request`, `on_error`) are **chained** — both fire when set simultaneously.

## Lifecycle

1. **Construction**: `McpServer::Create` — creates the session handler, wires handlers, starts message loop
2. **Registration**: Register tools, resources, prompts, extensions
3. **Run**: `server->Run()` — blocks on `io_context.run()`
4. **Shutdown**: `server->Close()` — cancels pending requests, closes transport

## Capability Derivation

Capabilities are automatically derived from registered primitives. For example, registering a tool sets `capabilities.tools.list_changed = true`. The `ServerOptions.capabilities` field is declared but currently not consumed by the server — capabilities are always auto-derived from registered tools, resources, prompts, and task store.
