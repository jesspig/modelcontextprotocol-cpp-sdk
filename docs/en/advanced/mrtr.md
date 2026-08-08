# MRTR (Multi Round-Trip Request)

MRTR (SEP-2322) allows server handlers to request additional input from the client during tool execution, without leaving the JSON-RPC context. This replaces the old server-to-client standalone request pattern.

## How It Works

In the stateless (2026-era) pattern, the server returns an `InputRequiredResult` with `resultType: "input_required"`. The client resolves the request internally via its `ElicitationHandler`, then retries the original tool call with `inputResponses` and `requestState`:

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
  │  (client resolves via   │
  │   ElicitationHandler)   │
  │                         │
  ├── tools/call ──────────►│  (retry with inputResponses
  │    (with inputResponses│   + requestState)
  │     + requestState)     │
  │◄── result: {...}        │
```

The `InputRequests` struct can contain either or both of:

| Field | Type | Purpose |
|-------|------|---------|
| `confirm` | `InputRequestElicit` | Simple yes/no confirmation |
| `elicit` | `InputRequestElicit` | Free-form input request |

Each `InputRequestElicit` has a `message` string and optional `requestedSchema` (JSON Schema).

## Server Side

Tool handlers can request user input via `Elicit`:

```cpp
// Using Elicit via RequestContext (inside a tool handler)
auto elicit_result = ctx.Server().Elicit(
    ElicitRequestParams{"Confirm order?", /* requested_schema */}).get();

CallToolResult result;
result.content.push_back(TextContent{"text",
    elicit_result.values ? "Confirmed" : "Cancelled"});
return result;
```

The `Elicit` method returns `std::future<ElicitResult>`.

Users can construct a typed result manually using `ElicitResultTyped<T>`:

```cpp
ElicitResultTyped<JsonValue> typed;
typed.action = "accept";
JsonValue obj(JsonValue::object_tag);
obj["confirmed"] = JsonValue(true);
typed.content = std::move(obj);
```

## Client Side

The client handles MRTR via `SetElicitationHandler`:

```cpp
client->SetElicitationHandler(
    [](const ElicitRequestParams& params) -> ElicitResult {
        ElicitResult result;
        JsonValue obj(JsonValue::object_tag);
        obj["confirmed"] = JsonValue(true);
        result.values = std::move(obj);
        return result;
    });
```

## InputRequired Result

The server can also return an `InputRequiredResult` directly (stateless mode):

```cpp
InputRequiredResult ir;
ir.input_requests.elicit = InputRequestElicit{"Provide value"};
ir.request_state = "state-token";
// Server returns this as the tools/call result
// Client resolves and retries with inputResponses + requestState
```

## Helper Functions

```cpp
// Build an input request payload for elicitation
JsonValue req = MakeInputRequestForElicitation(params);
// req == {"method": "elicitation/create", "params": {...}}

// Convert an elicited result to an input response
JsonValue resp = MakeInputResponseFromElicitResult(result);

// Check if a JSON result indicates input_required
if (IsInputRequiredResult(raw_result)) {
    auto input_requests = ExtractInputRequests(raw_result);
}
```

## Configuring MRTR

### Server Side

```cpp
ServerOptions opts;
opts.input_required_config = ServerOptions::InputRequiredConfig{
    .max_rounds = 8,
    .round_timeout = std::chrono::seconds(600),
    .legacy_shim = true
};
```

### Client Side

```cpp
ClientOptions opts;
opts.input_required_config = ClientOptions::InputRequiredConfig{
    .auto_fulfill = true,
    .max_rounds = 8,
    .round_timeout = std::chrono::seconds(600)
};
```

| Option | Server | Client | Description |
|--------|--------|--------|-------------|
| `max_rounds` | Yes | Yes | Maximum MRTR rounds (default: 8) |
| `round_timeout` | Yes | Yes | Per-round timeout (default: 600s) |
| `legacy_shim` | Yes | No | Placeholder field, not yet implemented |
| `auto_fulfill` | No | Yes | Auto-fulfill without prompting if possible |
