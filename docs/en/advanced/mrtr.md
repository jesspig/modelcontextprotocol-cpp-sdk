# MRTR (Multi Round-Trip Request)

MRTR (SEP-2322) allows server handlers to request additional input from the client during tool execution, without leaving the JSON-RPC context. This replaces the old server-to-client standalone request pattern.

## How It Works

```
Client                    Server
  │                         │
  ├── tools/call ──────────►│
  │                         ├── handler needs user input
  │◄── result:              │
  │    {                    │
  │      resultType:        │
  │        "input_required",│
  │      inputRequests: {   │
  │        elicit: {...}    │
  │      },                 │
  │      requestState: "..."│
  │    }                    │
  │                         │
  ├── Resolve elicit ──────►│  (client resolves via
  │                         │   ElicitationHandler)
  │◄── result: {...}        │
```

## Server Side

Tool handlers can request user input by calling `Elicit` or by using the MRTR helper:

```cpp
// Using Elicit directly (server sends elicit, awaits response)
server->RegisterTool("order_item",
    ToolOptions{}.Description("Place an order"),
    [&server](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
        // Requires user confirmation
        ElicitRequestParams confirm;
        confirm.message = "Confirm order for $" + amount + "?";
        auto elicit_result = server.Elicit(confirm).get();

        CallToolResult result;
        result.content.push_back(TextContent{"text",
            elicit_result.values ? "Confirmed" : "Cancelled"});
        return result;
    });
```

## Client Side

The `MrtrDriver` automatically handles the MRTR loop:

```cpp
client->SetElicitationHandler(
    [](const ElicitRequestParams& params) -> ElicitResult {
        // Prompt user with params.message
        // Return result based on user input
        ElicitResult result;
        result.values = nlohmann::json{{"confirmed", true}};
        return result;
    });
```

## InputRequired Result

The server can also return an `InputRequiredResult` directly (stateless mode):

```cpp
InputRequiredResult ir;
ir.input_requests.elicit = InputRequestElicit{"Confirm purchase?"};
// Server returns this as the tools/call result
// Client resolves and retries with input_responses + requestState
```

## Configuring MRTR

```cpp
ServerOptions opts;
opts.input_required_config = ServerOptions::InputRequiredConfig{
    .max_rounds = 8,
    .round_timeout = std::chrono::seconds(600),
    .legacy_shim = true
};
```
