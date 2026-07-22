# Protocol Versions

The SDK supports two MCP protocol eras via a dual `WireCodec` architecture.

## Version Comparison

| Version     | Status  | Key Features |
|-------------|---------|--------------|
| 2024-11-05  | Legacy  | Original specification |
| 2025-03-26  | Legacy  | Stable handshake |
| 2025-06-18  | Legacy  | Intermediate |
| 2025-11-25  | Legacy  | `initialize` handshake, standalone server-to-client requests |
| 2026-07-28  | Current | `server/discover`, per-request `_meta`, MRTR, `subscriptions/listen` |

## WireCodec

The `WireCodec` factory auto-selects the correct codec via string comparison:

```cpp
auto codec = MakeWireCodec("2026-07-28");
// Returns Rev2026Codec if version >= "2026-07-28"
// Falls back to Rev2025Codec for legacy versions
```

`SetNegotiatedProtocolVersion(version)` both stores the version AND recreates the `WireCodec` via `MakeWireCodec(version)`, switching between `Rev2025Codec` (no `_meta` envelope) and `Rev2026Codec` (per-request `_meta`).

Note that `HandleDiscover` returns `{"2025-11-25", "2026-07-28"}` but does NOT call `SetNegotiatedProtocolVersion` — the modern client drives version selection per-request via `_meta.protocolVersion`.

### Key Differences Between Eras

| Aspect | 2025-11-25 | 2026-07-28 |
|--------|-----------|------------|
| Connection | `initialize` handshake | `server/discover` per-request `_meta` |
| Capabilities | Negotiated once | Per-request via `_meta` |
| Sampling | Standalone request | Removed (use Elicitation) |
| Logging | `logging/setLevel` RPC | Per-request `_meta.logLevel` |
| Subscriptions | `subscribe`/`unsubscribe` | `subscriptions/listen` stream |
| Error codes | Direct values | Remapped (`-32001` → `-32020`, etc.) |
| Results | Plain JSON (identity encode/decode) | Typed with `resultType` field (auto-stamps `"complete"`) |
| `_meta` validation | Not required | Required on all requests except `server/discover` |

### Wire Validation (2026-era)

`Rev2026Codec::ValidateRequest` rejects requests missing the `_meta` envelope, except for `server/discover` which is the bootstrap call. This enforces the stateless protocol design.

### Result Encoding

- **2025-era**: `EncodeResult`/`DecodeResult` are identity — raw JSON passes through unchanged.
- **2026-era**: `EncodeResult` auto-stamps `resultType: "complete"` on the result if not already present. The `resultType` field enables downstream discrimination of normal results vs `input_required` (MRTR) results.

### IncomingRequestMeta

The `IncomingRequestMeta` struct extracts these fields from the 2026-era `_meta` envelope:

| Field | `_meta` Key |
|-------|-------------|
| `protocol_version` | `io.modelcontextprotocol/protocolVersion` |
| `client_info` | `io.modelcontextprotocol/clientInfo` |
| `client_capabilities` | `io.modelcontextprotocol/clientCapabilities` |
| `log_level` | `io.modelcontextprotocol/logLevel` |
| `progress_token` | `progressToken` |
| `subscription_id` | `io.modelcontextprotocol/subscriptionId` |

The `StampOutgoingMeta` helper stamps `protocolVersion`, `clientInfo`, `clientCapabilities`, and `logLevel` onto outgoing request `_meta`.

### Era-Gated Methods

The codec defines per-era method sets:

| Set | Methods |
|-----|---------|
| Common (both eras) | `tools/list`, `tools/call`, `resources/list`, `resources/read`, `resources/templates/list`, `prompts/list`, `prompts/get`, `completion/complete`, `elicitation/create` |
| 2025-only | `ping`, `initialize`, `resources/subscribe`, `resources/unsubscribe`, `logging/setLevel`, `roots/list`, `sampling/createMessage` |
| 2026-only | `server/discover`, `server/extensions/list`, `subscriptions/listen`, `tasks/get`, `tasks/update`, `tasks/cancel` |

### Era-Gated Notifications

| Set | Notifications |
|-----|---------------|
| Common (both eras) | `notifications/cancelled`, `notifications/progress`, `notifications/resources/updated`, `notifications/resources/list_changed`, `notifications/tools/list_changed`, `notifications/prompts/list_changed`, `notifications/subscriptions/acknowledged` |
| 2025-only | `notifications/initialized`, `notifications/message`, `notifications/roots/list_changed`, `notifications/elicitation/complete` |
| 2026-only | `notifications/tasks/status`, `notifications/tasks/working`, `notifications/tasks/completed`, `notifications/tasks/failed`, `notifications/tasks/cancelled`, `notifications/tasks/input_required` |

### Subscription System

The 2026-era subscription system uses `SubscriptionFilter` to declare interest:

```cpp
struct SubscriptionFilter {
    std::optional<bool> tools_list_changed;
    std::optional<bool> prompts_list_changed;
    std::optional<bool> resources_list_changed;
    std::vector<std::string> resource_subscriptions;
};
```

Clients call `subscriptions/listen` with the filter; the server tracks entries via `AddSubscription`/`AddSubscriptionEntry` and dispatches notifications via `NotifySubscribers`, which matches the notification type against each subscription's filter. Notifications include `io.modelcontextprotocol/subscriptionId` in `_meta`.

### Semantic Helpers

Both `McpSession` and `McpSessionHandler` provide:

```cpp
bool IsJuly2026OrLater() const;
// Returns true if negotiated_version_ >= "2026-07-28"
```

This gates protocol-era-specific behavior in application code.

## Protocol Infrastructure

### MessageChannel

`MessageChannel` provides a bounded async message queue with backpressure, built on `std::queue`, `std::mutex`, and `std::condition_variable`:

- `AsyncReceive(callback)` — blocks until a message arrives or channel is closed
- `Send(message)` — blocks if buffer full (backpressure)
- `TrySend(message)` — non-blocking send
- `Close()` — wakes all waiters

Used by `McpSessionHandler` for the async message loop.

### MessageFilter Pipeline

`FilterPipeline` chains multiple `MessageFilter` instances for interception (auth, audit, rate-limiting, request modification):

```cpp
auto pipeline = std::make_shared<FilterPipeline>();
pipeline->AddFilter(std::make_shared<IncomingMessageFilter>(
    [](const JsonRpcMessage& msg, MessageFilterNext next) {
        // Inspect/modify, then call next(filtered) or short-circuit
        next(msg);
    }));
```

Incoming filters wrap handler dispatch; outgoing filters wrap transport send. Both are optional and configured via `ServerOptions::incoming_filters` / `outgoing_filters`.

### X-Mcp-Header Annotations

`XMcpHeaders.hpp` provides helpers for `x-mcp-header` annotations in JSON Schema:

- `ScanXMcpHeaders(schema)` — extracts `paramName → headerName` mappings from `x-mcp-header` declarations
- `ExtractXMcpHeaderValues(params, decls)` — extracts header values from params for primitive types (string, integer, boolean)

## Error Code Remapping (2026-era)

| Code | Name | 2025 Value | 2026 Value |
|------|------|-----------|-----------|
| HeaderMismatch | Header mismatch | -32001 | -32020 |
| MissingRequiredClientCapability | Missing capability | -32003 | -32021 |
| UnsupportedProtocolVersion | Version mismatch | -32004 | -32022 |
