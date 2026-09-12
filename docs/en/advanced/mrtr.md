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
    elicit_result.content ? "Confirmed" : "Cancelled"});
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
        result.content = std::move(obj);
        return result;
    });
```

## InputRequired Result

The server can also return an `InputRequiredResult` directly by setting `input_required` on the `CallToolResult` (stateless mode):

```cpp
CallToolResult result;
InputRequiredResult ir;
ir.input_requests.elicit = InputRequestElicit{"Provide value"};
result.input_required = std::move(ir);
return result;
```

When `request_state_key` is configured, the server automatically signs `request_state` on `input_required` results before sending them (handlers don't need to — and cannot — supply one themselves). When the client retries with `inputResponses` + `requestState`, tampered or expired states are rejected before reaching the handler (-32602, `data.reason="invalid_request_state"`).

The underlying mint/verify helpers live in `include/mcp/server/RequestState.hpp` (`MintRequestState` / `VerifyRequestState`); for custom verification logic, provide `ServerOptions::request_state_verifier` instead.

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
    .max_rounds = 10,
    .round_timeout = std::chrono::seconds(600),
    .legacy_shim = true
};
opts.request_state_key = "server-secret";              // auto-signs requestState
opts.request_state_ttl = std::chrono::seconds(300);    // state lifetime; 0 = never expires
```

### Client Side

```cpp
ClientOptions opts;
opts.input_required_config = ClientOptions::InputRequiredConfig{
    .auto_fulfill = true,
    .max_rounds = 10,
    .round_timeout = std::chrono::seconds(600),
    .max_total_timeout = std::chrono::seconds(0)
};
```

| Option | Server | Client | Description |
|--------|--------|--------|-------------|
| `max_rounds` | Yes | Yes | Maximum MRTR rounds (default: 10) |
| `round_timeout` | Yes | Yes | Per-round timeout (default: 600s) |
| `legacy_shim` | Yes | No | Placeholder field, not yet implemented |
| `auto_fulfill` | No | Yes | Auto-fulfill without prompting if possible |
| `max_total_timeout` | No | Yes | Hard budget for the whole MRTR flow (default 0 = unlimited; `round_timeout` applies per round) |
| `request_state_key` / `request_state_ttl` | Yes | No | Server-side requestState signing key and lifetime |
