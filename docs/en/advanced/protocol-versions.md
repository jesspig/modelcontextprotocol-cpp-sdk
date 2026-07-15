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

### Key Differences Between Eras

| Aspect | 2025-11-25 | 2026-07-28 |
|--------|-----------|------------|
| Connection | `initialize` handshake | `server/discover` per-request `_meta` |
| Capabilities | Negotiated once | Per-request via `_meta` |
| Sampling | Standalone request | Removed (use Elicitation) |
| Logging | `logging/setLevel` RPC | Per-request `_meta.logLevel` |
| Subscriptions | `subscribe`/`unsubscribe` | `subscriptions/listen` stream |
| Error codes | Direct values | Remapped (`-32001` → `-32020`, etc.) |
| Results | Plain JSON | Typed with `resultType` field |

## Error Code Remapping (2026-era)

| Code | Name | 2025 Value | 2026 Value |
|------|------|-----------|-----------|
| HeaderMismatch | Header mismatch | -32001 | -32020 |
| MissingRequiredClientCapability | Missing capability | -32003 | -32021 |
| UnsupportedProtocolVersion | Version mismatch | -32004 | -32022 |
