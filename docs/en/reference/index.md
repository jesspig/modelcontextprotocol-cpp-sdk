# Reference Overview

The reference layer answers exactly three things: **what the signature is, what each field defaults to, and what happens on failure**. Every "how to do it" procedure lives in the task layer, and every "why it is designed this way" lives in the concepts layer. This page defines the reading conventions and maps pages to sources.

## Reading conventions

| Convention | Meaning |
|---|---|
| Source locations | Each page ends with a "Source files behind this page" list naming the relevant files; line numbers are deliberately not quoted — they drift with every change, so locate code by symbol name |
| Era markers | Some capabilities exist in only one protocol era. Conclusions link back to [Protocol Versions](/en/learn/protocol-versions); no other page duplicates that matrix |
| Deprecation | An interface marked "deprecated" still exists and is still callable; the page states "callable but deprecated" plus the replacement. Only genuinely deleted items are marked "removed" |
| Internal symbols | Types and functions under `detail/` are not user API. Reference pages mention them only to explain behavior, never in interface tables |

## Type and ownership conventions

| Convention | Fact |
|---|---|
| Factory return | `McpServer::Create` / `McpClient::Create` return `std::unique_ptr` |
| Transport ownership | `IClientTransport::Connect()` returns `std::shared_ptr`; transports passed to factories are `std::shared_ptr<ITransport>` |
| JSON value type | `JsonValue` is this repository's own class, **not nlohmann/json**; access via `Find`/`IsString`/`IsNumber`/`GetInt`/`Dump`, field tables in [Types](/en/reference/types) |
| Optional fields | Always `std::optional`, never sentinel values |
| By-value parameters | `JsonValue` arguments are passed by value |
| Error propagation | Throw `McpError(McpErrorCode, message)`, derived from `std::runtime_error`; user handler exceptions follow the convergence rules of each layer, see [Errors](/en/reference/errors) |

## Page to source mapping

| Reference page | Scope | Main sources |
|---|---|---|
| [Server facade](/en/reference/server) | All `McpServer` methods, registration APIs, `RequestContext`, `ServerOptions`, capability derivation, tool-call error semantics | `include/mcp/server/` |
| [Client facade](/en/reference/client) | All `McpClient` methods, request handles and cancellation, MRTR, inbound handlers, `ClientOptions`, connect modes | `include/mcp/client/` |
| [Transports](/en/reference/transports) | Transport contract, the five transports with every Options field, HTTP server substrate | `include/mcp/Transport.hpp`, `include/mcp/transport/`, `include/mcp/http/` |
| [Types](/en/reference/types) | `JsonValue`, content and every `*Result`/`*RequestParams`, capabilities, task and MRTR types, logging and spans | `include/mcp/McpTypes.hpp` and core headers |
| [Errors](/en/reference/errors) | Full `McpErrorCode` table, throw sites, era remapping, `RequestOutcome` variants | `include/mcp/ErrorCodes.hpp`, `src/protocol/` |
| [Process and storage](/en/reference/process-and-storage) | `mcp::Spawn` and `SpawnedProcess`, task/event/session stores, `HttpServer` | `include/mcp/process/`, `include/mcp/storage/`, `include/mcp/http/` |
| [Auth and extensions](/en/reference/auth-and-extensions) | OAuth and PKCE, DPoP, token cache, server card/apps/triggers, skill verification, method and version constants | `include/mcp/ext/`, `include/mcp/client/auth/` |

## Related pages

- [Concepts: protocol versions](/en/learn/protocol-versions) — the single authoritative dual-era matrix
- [Concepts: architecture](/en/learn/architecture) — library layering, threading model, capability derivation boundaries
- [Concepts: extensions](/en/learn/extensions) — explicit declaration requirements for the `enable_*` switches
- [How-to: register a tool](/en/howto/register-a-tool) — from field tables to running code
- [How-to: handle server-initiated requests](/en/howto/handle-server-initiated-requests) — passive callbacks and follow-up rounds
