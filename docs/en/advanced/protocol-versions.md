# Protocol Versions

The SDK supports two MCP protocol eras via a dual `WireCodec` architecture.

## Version Comparison

| Version     | Status  | Key Features |
|-------------|---------|--------------|
| 2024-11-05  | Legacy  | Original spec revision |
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

`HandleDiscover` returns the full `kProtocolVersions` table (5 versions, 2024-11-05 through 2026-07-28) and calls `SetNegotiatedProtocolVersion` to set the negotiated version (`options_.protocol_version` when configured, otherwise `kLatestProtocolVersion`) — the modern client also drives version selection per-request via `_meta.protocolVersion` as needed.

### Key Differences Between Eras

| Aspect | 2025-11-25 | 2026-07-28 |
|--------|-----------|------------|
| Connection | `initialize` handshake | `server/discover` per-request `_meta` |
| Capabilities | Negotiated once | Per-request via `_meta` |
| Sampling | Standalone request | Removed (use Elicitation) |
| Logging | `logging/setLevel` RPC | Per-request `_meta.logLevel` |
| Subscriptions | `subscribe`/`unsubscribe` | `subscriptions/listen` stream |
| Error codes | Direct values | Internal errors (`-32001`/`-32003`/`-32004`) remapped to `InternalError` (-32603); protocol codes (`-32020`/`-32021`/`-32022`/`-32042`) pass through |
| Results | Plain JSON (identity encode/decode) | Typed with `resultType` field (auto-stamps `"complete"`) |
| `_meta` validation | Not required | Required on all requests except `server/discover` |

### Wire Validation (2026-era)

`Rev2026Codec::ValidateRequest` performs two checks:
1. **Era membership**: Returns `NotInEra` if the method is not in the 2026-era method set.
2. **_meta presence**: Returns `Invalid` if the request (except `server/discover`) lacks the `_meta` envelope.

This enforces the stateless protocol design where `server/discover` is the only bootstrap call without prior context.

### Response Validation (2026-era)

`Rev2026Codec::ValidateResponse` validates outgoing responses:
1. **`resultType` field**: Must be present on all responses. Returns `Invalid` if missing.
2. **List methods**: For `tools/list`, `resources/list`, `resources/templates/list`, and `prompts/list`, the `resultType` must be `"complete"`. List results cannot be partial/input_required.

### Notification Validation (2026-era)

`Rev2026Codec::ValidateNotification` ensures notifications do not contain `id`, `result`, or `error` fields — only `method` and `params` are allowed.

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
| `traceparent`     | `traceparent` |
| `tracestate`      | `tracestate` |
| `baggage`         | `baggage` |

Outgoing request `_meta` is written by `McpSessionHandler::SendRequest` via `SerializeRequestMeta`, which serializes `protocolVersion`, `clientInfo`, and `clientCapabilities` (`WireCodec::StampOutgoingRequest` likewise stamps only these three fields, and has no call sites). Trace/distributed tracing fields (`traceparent`, `tracestate`, `baggage`) are serialized only when explicitly set on the request meta.

### Era-Gated Methods

The codec defines per-era method sets:

| Set | Methods |
|-----|---------|
| Common (both eras) | `tools/list`, `tools/call`, `resources/list`, `resources/read`, `resources/templates/list`, `prompts/list`, `prompts/get`, `completion/complete` |
| 2025-only | `initialize`, `ping`, `resources/subscribe`, `resources/unsubscribe`, `logging/setLevel`, `roots/list`, `sampling/createMessage`, `elicitation/create`, `tasks/get`, `tasks/update`, `tasks/cancel`, `tasks/result`, `tasks/list` |
| 2026-only | `server/discover`, `subscriptions/listen` |

Note that `ping`, `elicitation/create`, and all `tasks/*` methods are 2025-only: they are not available in the 2026 era (`server/extensions/list` is in no era's method set).

### Era-Gated Notifications

| Set | Notifications |
|-----|---------------|
| Common (both eras) | `notifications/cancelled`, `notifications/progress`, `notifications/message`, `notifications/resources/updated`, `notifications/resources/list_changed`, `notifications/tools/list_changed`, `notifications/prompts/list_changed` |
| 2025-only | `notifications/initialized`, `notifications/roots/list_changed`, `notifications/elicitation/complete`, `notifications/tasks/status` |
| 2026-only | `notifications/subscriptions/acknowledged` |

The remaining `notifications/tasks/working`, `notifications/tasks/completed`, `notifications/tasks/failed`, `notifications/tasks/cancelled`, and `notifications/tasks/input_required` notification constants are still defined (`Methods.hpp`) but belong to no era's set.

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
pipeline->AddFilter(std::make_shared<MessageFilterFuncAdapter>(
    [](const JsonRpcMessage& msg, MessageFilterNext next) {
        // Inspect/modify, then call next(filtered) or short-circuit
        next(msg);
    }));
```

Incoming filters wrap handler dispatch; outgoing filters wrap transport send. Both are optional and configured via `ServerOptions::incoming_filters` / `outgoing_filters`.

## Error Code Remapping (2026-era)

`Rev2026Codec::EncodeErrorCode` no longer disguises internal errors as protocol error codes:

| Error | 2026 Value |
|-------|-----------|
| `RequestTimeout`, `ConnectionRefused`, `TlsHandshakeFailed` | logged and remapped to `InternalError` (-32603) |

Protocol error codes (`HeaderMismatch` -32020, `MissingRequiredClientCapability` -32021, `UnsupportedProtocolVersion` -32022, `UrlElicitationRequired` -32042) pass through unchanged and carry canonical `data` in their real scenarios (e.g. `requiredCapabilities`, `requested`/`supported`).
