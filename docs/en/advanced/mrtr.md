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
  │        "name": {        │
  │          method: ...,   │
  │          params: {...}  │
  │        }                │
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

`InputRequests` maps a server-assigned key to a request object (`std::map<std::string, InputRequest, std::less<>>`). Each `InputRequest` carries a `method` and its `params`:

| Field | Type | Purpose |
|-------|------|---------|
| `method` | `std::string` | Request method: `elicitation/create`, `sampling/createMessage`, `roots/list` |
| `params` | `JsonValue` | The params object of that method |

Keys are server-chosen semantic names (e.g. `"name"`, `"confirm"`, `"paths"`); the client echoes the **same keys** in `inputResponses`, each holding a bare result.

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
        result.action = "accept";
        JsonValue obj(JsonValue::object_tag);
        obj["confirmed"] = JsonValue(true);
        result.content = std::move(obj);
        return result;
    });
```

The client dispatches on each input request's `method`: `elicitation/create` → `ElicitationHandler`, `sampling/createMessage` → `SamplingHandler`, `roots/list` → `RootsHandler`. An unregistered handler or an unknown method raises `McpError` (`MethodNotFound`).

`inputResponses` values are **bare results** (elicitation: `{action, content}`, sampling: `{role, content, model, stopReason}`, roots: `{roots}`) with no `resultType` / `meta` envelope.

## InputRequired Result

The server can also return an `InputRequiredResult` directly by setting `input_required` on the `CallToolResult` (stateless mode):

```cpp
CallToolResult result;
InputRequiredResult ir;
ElicitRequestParams params;
params.message = "Provide value";
ir.input_requests["name"] = MakeInputRequestForElicitation(params);
result.input_required = std::move(ir);
return result;
```

When `request_state_key` is configured, the server automatically signs `request_state` on `input_required` results before sending them (handlers don't need to — and cannot — supply one themselves). When the client retries with `inputResponses` + `requestState`, tampered or expired states are rejected before reaching the handler (-32602, `data.reason="invalid_request_state"`).

The underlying mint/verify helpers live in `include/mcp/server/RequestState.hpp` (`MintRequestState` / `VerifyRequestState`); for custom verification logic, provide `ServerOptions::request_state_verifier` instead.

## Helper Functions

```cpp
// Build an elicitation input request (a {method, params} request object)
InputRequest request = MakeInputRequestForElicitation(params);
// request.method == "elicitation/create"
// Siblings: MakeInputRequestForSampling / MakeInputRequestForRoots

// Convert an elicited result to a bare input response (no resultType / meta)
JsonValue resp = MakeInputResponseFromElicitResult(result);
// Siblings: MakeInputResponseFromCreateMessageResult / MakeInputResponseFromListRootsResult

// Check if a JSON result indicates input_required
if (IsInputRequiredResult(raw_result)) {
    std::optional<InputRequests> input_requests = ExtractInputRequests(raw_result);
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
