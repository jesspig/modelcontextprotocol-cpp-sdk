# Client Overview

The `McpClient` class connects to MCP servers, negotiates protocol versions, and invokes server capabilities.

## Creating a Client

```cpp
auto transport = std::make_shared<StdioClientTransport>("path/to/server");
ClientOptions opts;
opts.client_info = Implementation{"MyClient", "1.0.0"};

auto client = McpClient::Create(transport, opts);
```

## ClientOptions

| Field | Type | Description |
|-------|------|-------------|
| `client_info` | `Implementation` | Client identity |
| `capabilities` | `optional<ClientCapabilities>` | Declared capabilities |
| `connect_mode` | `ConnectMode` | `Auto` (discover → initialize), `Legacy`, `Pin` |
| `initialization_timeout` | `chrono::seconds` | Handshake timeout |

## Making Requests

```cpp
// List tools
auto tools = client->ListTools();

// Call a tool
auto result = client->CallTool("echo",
    nlohmann::json{{"text", "Hello"}});

// Read a resource
auto resource = client->ReadResource("file:///config.json");

// Get a prompt
auto prompt = client->GetPrompt("code_review",
    nlohmann::json{{"diff", "..."}});

// Complete a prompt/resource reference
auto completion = client->Complete(params);

// Ping (heartbeat)
client->Ping();
```

## Server-to-Client Handlers

Register handlers for server-initiated requests:

```cpp
client->SetElicitationHandler(
    [](const ElicitRequestParams& params) -> ElicitResult {
        // Prompt user for input, return result
        ElicitResult result;
        result.values = nlohmann::json{{"name", "Alice"}};
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
```

## Subscriptions

```cpp
// Subscribe to server notifications (2026-era)
SubscriptionsListenRequestParams subs;
subs.notifications.tools_list_changed = true;
subs.notifications.resources_list_changed = true;
client->SubscribeAsync(subs);
```

## Version Negotiation

The client auto-negotiates the protocol version:

1. **Auto** (default): Probe `server/discover`, fallback to `initialize` handshake
2. **Pin**: Force a specific version
3. **Legacy**: Only `initialize` handshake
