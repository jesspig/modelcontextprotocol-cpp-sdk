# Extensions

Protocol extensions are negotiated via the `extensions` map on `ClientCapabilities` and `ServerCapabilities`. Each entry maps an extension identifier to its configuration value.

## Server-Side Auto-Derivation

Setting a `task_store` on `ServerOptions` declares the tasks extension only when `enable_tasks_extension` is explicitly enabled:

```cpp
auto transport = std::make_shared<StdioServerTransport>();

ServerOptions opts;
opts.task_store = std::make_shared<FileTaskStore>("tasks.json");
opts.enable_tasks_extension = true;

auto server = McpServer::Create(transport, opts);
```

There is no `ServerOptions.capabilities` field — capabilities are always auto-derived:

| Condition | Declared extension key | Value |
|-----------|----------------------|-------|
| `task_store` is set and `enable_tasks_extension=true` | `"io.modelcontextprotocol/tasks"` | Empty object `{}` |
| `enable_skills_extension=true`, at least one skill registered (`RegisterSkill`) **and** the resources capability is present | `"io.modelcontextprotocol/skills"` | Empty object; contains `"directoryRead": true` after `SetSkillsDirectoryRead(true)` |

For client-side declarations see [Client Overview](/en/client/overview) (`ClientOptions::capabilities.extensions` or `ClientOptions::extensions` carrying the tasks extension key, with `enable_tasks_extension` enabled); in the 2026 era a client that receives a task result without declaring the tasks extension throws `HandlerError`.

## Task Notification Routing

In the 2026 era, task status notifications (the initial Working and terminal state of taskified `tools/call`, `tasks/completed`/`tasks/working` from `tasks/update`, `tasks/cancelled` from `tasks/cancel`) are no longer suppressed — they are routed via the server's subscription table to all current subscribers (task notifications bypass filters), with each frame carrying `io.modelcontextprotocol/subscriptionId` in `_meta` (echoing the ID passed to `subscriptions/listen`, or a server-assigned incrementing ID otherwise); with no subscribers the notification is dropped, and clients poll to the terminal state with `PollTaskToCompletion` (a positive task `poll_interval_ms` switches to the server-indicated interval, capped at 300s). The 2025 era keeps broadcast delivery (`notifications/tasks/*`).

## Skill URI Shape and Verification Helpers

A skill entry `uri` must have the shape `<scheme>://<name>/SKILL.md` (scheme-agnostic: only a non-empty `://`, a non-empty name segment, and the `/SKILL.md` suffix are required; `skill://` remains the conventional scheme). The server only enforces the shape; whether to trust a foreign-origin skill is decided client-side with the five helpers in `mcp/client/SkillVerify.hpp`: `CheckSkillLimits` (manifest ≤ 512 files, ≤ 16 MB total), `VerifyFileBytes` (`size` length match + `sha256:` digest recomputation), `VerifyFrontmatter` (both copies equal and `name` matching the URI name segment), `IsApprovalStillValid` (approval snapshot re-check), `ApprovalKey` (`"<host>\x1f<skill-uri>"` approval key).

## Extension Convention

| Field | Description |
|-------|-------------|
| Key | Reverse-DNS identifier (e.g., `"io.modelcontextprotocol/tasks"`) |
| Value | Free-form JSON object with the extension's configuration |

## Experimental Flags

The 8 `ServerOptions` flags all default to `false`; among the 4 `ClientOptions` flags, `enable_etag_cache` defaults to `true` and the rest to `false`. Set them to `true` explicitly as needed (flags sync at construction: `McpServer::Create` copies options by value, later mutations of the caller's `options` have no effect; etag is process-global — any server constructed with it on enables process-wide etag reads/writes):

| Side | Flag | On | Off |
|------|------|----|-----|
| Server | `enable_tasks_extension` | Declares the tasks extension; `tasks/get\|update\|cancel` and taskified `tools/call` take effect | Without `task_store`, the corresponding methods return `MethodNotFound` (`tasks extension is disabled`); with only `task_store` set, no longer declared — explicit `enable_tasks_extension=true` is required (implicit path removed, see below) |
| Server | `enable_skills_extension` | With at least one registered skill and the resources capability, declares the skills extension; `skills/list\|get` and `directory/read` take effect | All three methods return `MethodNotFound` (`skills extension is disabled`); no implicit path |
| Server | `enable_etag_cache` | List results carry a content-hash `etag` (skipped past the 8 MB cap); on a `notModified` reply clients fall back to the stored body, sending `ifNoneMatch` via the meta extensions | No `etag` attached; client store access short-circuits, unconditional sends |
| Server | `enable_progressive_discovery` | `tools/list` supports `query` filtering + `pageSize` paging; `listen` flushes and holds open, `list_changed` buffers per subscription | `tools/list` serves the cached full set; `listen` and `list_changed` use the legacy paths |
| Server | `enable_file_input` | `file://`/`resource://` references inside `tools/call` arguments are range-sliced and small files inlined (`http(s)` passes through; out-of-range/over-limit returns an error) | Arguments pass through untouched |
| Server | `enable_triggers_events` | Registry + type-matched emit, delivered via subscription-filtered trigger event notifications plus webhooks (failures only log a `Warning`) | Emit returns immediately; events are silently dropped |
| Server | `enable_apps_extension` | Declares the `io.modelcontextprotocol/ui` extension; `ui://` resource reads take the normal path and are validated by `ValidateUiMeta` | `ui://` `resources/read` returns `HandlerError` (`apps extension is disabled`); non-`ui://` resources are unaffected |
| Server | `enable_server_card` | Declares the `io.modelcontextprotocol/server-card` extension; `GetServerCard()` returns the explicit or default card validated by `ValidateCard` | `GetServerCard()` throws `HandlerError` (`server card is disabled`); HTTP `GET /server-card` is gated separately by the transport-side flag |
| Client | `enable_tasks_extension` | Declared when the flag is on and the `extensions` map carries the tasks key; `UpdateTask`/`PollTaskToCompletion` and the task-handle path take effect | Those calls throw `MethodNotFound` (`tasks extension is disabled`) |
| Client | `enable_skills_extension` | `ListSkills`/`GetSkill`/`ReadDirectory` take effect | All three calls throw `MethodNotFound` (`skills extension is disabled`) |
| Client | `enable_progressive_discovery` | `SearchTools` paging requests carry `query`/`pageSize` | Paged aggregation still works but without `query`/`pageSize` |
| Client | `enable_etag_cache` (default `true`) | With the global `SetEtagCacheEnabled` also on, the 8 read paths send `ifNoneMatch` via meta and fall back to the stored body on `notModified` | No conditional sends, no store reads; requests proceed unchanged |

Server-side DPoP is not a `ServerOptions` flag: the effective gate is transport-side `StreamableHttpServerOptions::BearerAuthConfig::enable_dpop` (requires `bearer_auth` to be configured first); the client-side counterpart is `OAuthClientOptions::enable_dpop`. `Interceptor` remains an ungated alias of `MessageFilter`, but has no corresponding flag.

Compatibility transition (0.3.5→0.3.8, scope given by this task): `TasksImplicitlyEnabled()` / `SkillsImplicitlyEnabled()` are both marked `[[deprecated]]`, directing users to set the flags explicitly; the implicit path has been removed — setting just `task_store` no longer declares the tasks extension and logs no `Warning`, explicit `enable_tasks_extension=true` is required; skills has no implicit path and must be enabled explicitly.
