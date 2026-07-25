# Client Overview

The `McpClient` class connects to MCP servers, negotiates protocol versions, and invokes server capabilities.

## Creating a Client

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

| Field | Type | Description |
|-------|------|-------------|
| `client_info` | `Implementation` | Client identity (default `{"mcp-cpp-client", "0.1.0"}`) |
| `capabilities` | `optional<ClientCapabilities>` | Declared capabilities |
| `connect_mode` | `ConnectMode` | `Auto` (discover → initialize), `Legacy`, `Pin` |
| `initialization_timeout` | `chrono::seconds` | Handshake timeout (default 60s) |
| `pin_protocol_version` | `optional<string>` | Pin to a specific protocol version (used with `Pin` mode) |
| `discover_probe_timeout` | `chrono::seconds` | Server discovery probe timeout (default 5s) |
| `input_required_config` | `optional<InputRequiredConfig>` | MRTR elicitation config: `auto_fulfill=true`, `max_rounds=8`, `round_timeout=600s` |
| `extensions` | `optional<JsonValue>` | Protocol extension declarations |

## Making Requests

```cpp
// List tools (with optional cursor for pagination)
auto tools = client->ListTools();

// Call a tool (with optional arguments, RequestOptions, and MRTR support)
auto result = client->CallTool("echo",
    JsonValue(JsonValue::Object{{"text", "Hello"}}));

// Read a resource (with CacheableRequestOptions)
auto resource = client->ReadResource("file:///config.json");

// Get a prompt (with optional arguments and RequestOptions)
auto prompt = client->GetPrompt("code_review",
    JsonValue(JsonValue::Object{{"diff", "..."}}));

// Complete a prompt/resource reference
auto completion = client->Complete(params);

// Ping (heartbeat, deprecated in 2026-07-28)
client->Ping();

// Discover server capabilities (re-negotiate)
auto discover = client->Discover();

// List resources and templates
auto resources = client->ListResources();
auto templates = client->ListResourceTemplates();

// List prompts
auto prompts = client->ListPrompts();

// Subscribe/unsubscribe to resource changes
client->SubscribeResource("file:///config.json");
client->UnsubscribeResource("file:///config.json");

// Task operations
auto task = client->GetTask("task-123");
client->UpdateTask("task-123", result_json);
client->CancelTask("task-123", "no longer needed");

// Poll task until completion (with configurable interval and timeout)
auto completed = client->PollTaskToCompletionAsync("task-123");
```

## Server-to-Client Handlers

Register handlers for server-initiated requests:

```cpp
client->SetElicitationHandler(
    [](const ElicitRequestParams& params) -> ElicitResult {
        // Prompt user for input, return result
        ElicitResult result;
        result.values = JsonValue(JsonValue::Object{{"name", "Alice"}});
        return result;
    });

client->SetSamplingHandler(
    [](const CreateMessageRequestParams& params) -> CreateMessageResult {
        // Deprecated: use Elicitation instead
    });

client->SetRootsHandler(
    [](const ListRootsRequestParams& params) -> ListRootsResult {
        // Deprecated: provide root directories
    });

client->SetNotificationHandler("custom/notification",
    [](const JsonRpcNotification& notif) {
        // Handle server-sent notifications
    });

client->SetLoggingHandler(
    [](const LoggingMessageNotificationParams& params) {
        // Handle logging messages from server
    });
```

## Subscriptions

```cpp
// Subscribe to server notifications (2026-era)
SubscriptionsListenRequestParams subs;
subs.notifications.tools_list_changed = true;
subs.notifications.resources_list_changed = true;
subs.notifications.resource_subscriptions = {"file:///config.json"};
client->SubscribeAsync(subs);
```

## Version Negotiation

The client auto-negotiates the protocol version:

1. **Auto** (default): Probe `server/discover`, fallback to `initialize` handshake
2. **Pin**: Force a specific version (via `pin_protocol_version`, defaults to latest `kLatestProtocolVersion`)
3. **Legacy**: Only `initialize` handshake
