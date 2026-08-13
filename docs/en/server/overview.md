# Server Overview

The `McpServer` class exposes capabilities (tools, resources, prompts) to connected MCP clients.

## Creating a Server

```cpp
auto transport = std::make_shared<StdioServerTransport>();

ServerOptions opts;
opts.server_info = Implementation{"MyServer", "1.0.0"};

auto server = McpServer::Create(transport, opts);
server->Run();
```

For Streamable HTTP mode:

```cpp
StreamableHttpServerOptions http_opts;
http_opts.port = 3001;
http_opts.stateless = true;  // or false for session mode

auto transport = std::make_shared<StreamableHttpServerTransport>(http_opts);

ServerOptions opts;
opts.server_info = Implementation{"MyServer", "1.0.0"};

auto server = McpServer::Create(transport, opts);
server->Run();
```

## ServerOptions

| Field | Type | Description |
|-------|------|-------------|
| `server_info` | `optional<Implementation>` | Server identity (name, version) |
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
| `on_client_connected` | `function(const Implementation&)` | Called when a client completes `initialize` |
| `on_initialized` | `function()` | Called when client sends `notifications/initialized` |
| `on_transport_close` | `function()` | Called when the transport connection closes |
| `on_transport_error` | `function(string_view)` | Called on transport-level errors |
| `incoming_filters` | `shared_ptr<FilterPipeline>` | Pipeline to intercept/modify/block inbound messages |
| `outgoing_filters` | `shared_ptr<FilterPipeline>` | Pipeline to intercept/modify/block outbound messages |

Shorthand callbacks (`on_method_called`, `on_protocol_error`) and full-message callbacks (`on_request`, `on_error`) are **chained** — both fire when set simultaneously.

## RequestContext\<TParams\>

Every tool handler receives a `RequestContext<CallToolRequestParams>` providing request context:

| Member | Return Type | Description |
|--------|-------------|-------------|
| `Params()` | `const TParams&` | The deserialized request parameters |
| `Server()` | `McpServer&` | Reference to the owning server instance |
| `GetRequest()` | `const JsonRpcRequest&` | The raw JSON-RPC request |
| `LogLevel()` | `optional<LoggingLevel>` | Per-request log level from `_meta` (2026-era) |
| `Log(level, data)` | `void` | Sends a logging notification, filtered by `LogLevel()` |

The `Log()` method respects per-request log levels: messages below the request's `LogLevel()` are silently dropped.

## McpServerTool

For reusable tool definitions, subclass `McpServerTool` or use `McpServerTool::Create`:

```cpp
auto tool = McpServerTool::Create("get_weather",
    [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
        // ...
    },
    ToolOptions{}.Description("Get weather"));
server->RegisterTool(tool);
```

The `McpServerTool` abstract class exposes:
- `ProtocolTool()` — returns the protocol-level `Tool` struct
- `InvokeAsync(ctx, promise)` — override for custom async execution

The lambda-based `RegisterTool` overload works identically — it creates an `McpServerTool` internally.

## Properties

| Method | Return Type | Description |
|--------|-------------|-------------|
| `GetClientCapabilities()` | `std::shared_ptr<const ClientCapabilities>` | Only populated during a 2025-era client `initialize` handshake; always nullptr in the 2026 era (no initialize) |
| `GetClientInfo()` | `std::shared_ptr<const Implementation>` | Client identity (nullptr before connect) |
| `GetNegotiatedProtocolVersion()` | `string_view` | The negotiated protocol version string |
| `GetCapabilities()` | `const ServerCapabilities&` | Server capabilities auto-derived from registered primitives |
| `IsMrtrSupported()` | `bool` | Whether MRTR (Multi-Round Tool Retrieval) is supported |

## Notifications

| Method | Description |
|--------|-------------|
| `SendToolListChanged()` | Notifies clients of tool list changes |
| `SendResourceListChanged()` | Notifies clients of resource list changes |
| `SendPromptListChanged()` | Notifies clients of prompt list changes |
| `SendLoggingMessage(level, data)` | Sends a logging message (respects client's `logging/setLevel`) |
| `SendLoggingMessage(level, data, min_level)` | Overload with explicit minimum level override |

## Completion Handler

Register an optional handler for `completion/complete` requests (e.g., argument auto-complete):

```cpp
server->SetCompletionHandler(
    [](const CompleteRequestParams& params) -> CompleteResult {
        CompleteResult result;
        result.completion = JsonValue::Parse(R"({"values":["value1","value2"]})");
        return result;
    });
```

## Lifecycle

1. **Construction**: `McpServer::Create` — creates the session handler, wires handlers, starts message loop
2. **Registration**: Register tools, resources, prompts via `RegisterTool`, `RegisterResource`, `RegisterResourceTemplate`, `RegisterPrompt`
3. **Run**: `server->Run()` — blocks until `Close()` is called; the session handler processes messages asynchronously
4. **Shutdown**: `server->Close()` — waits for pending async calls, closes handler and transport

## Capability Derivation

Capabilities are automatically derived from registered primitives. For example, registering a tool sets `capabilities.tools.list_changed = true`. There is no `ServerOptions.capabilities` field — capabilities are always auto-derived from registered tools, resources, prompts, and task store.
