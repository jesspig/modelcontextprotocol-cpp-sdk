# Extensions

Protocol extensions are negotiated via the `extensions` map on `ClientCapabilities` and `ServerCapabilities`. Each entry maps an extension identifier to its configuration value.

## Server-Side Auto-Derivation

Setting a `task_store` on `ServerOptions` automatically declares the tasks extension:

```cpp
auto transport = std::make_shared<StdioServerTransport>();

ServerOptions opts;
opts.task_store = std::make_shared<FileTaskStore>("tasks.json");

auto server = McpServer::Create(transport, opts);
```

There is no `ServerOptions.capabilities` field — capabilities are always auto-derived:

| Condition | Declared extension key | Value |
|-----------|----------------------|-------|
| `task_store` is set | `"io.modelcontextprotocol/tasks"` | Empty object `{}` |
| At least one skill registered (`RegisterSkill`) **and** the resources capability is present | `"io.modelcontextprotocol/skills"` | Empty object; contains `"directoryRead": true` after `SetSkillsDirectoryRead(true)` |

For client-side declarations see [Client Overview](/en/client/overview) (`ClientOptions::capabilities.extensions` or `ClientOptions::extensions` carrying the tasks extension key); in the 2026 era a client that receives a task result without declaring the tasks extension throws `HandlerError`.

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

The 10 `ServerOptions` flags and 3 `ClientOptions` flags all default to `false` and must be explicitly set to `true`:

| Side | Flag | On | Off |
|------|------|----|-----|
| Server | `enable_tasks_extension` | Declares the tasks extension; `tasks/get\|update\|cancel` and taskified `tools/call` take effect | Without `task_store`, the corresponding methods return `MethodNotFound` (`tasks extension is disabled`); with only `task_store` set, implicitly enabled with a `Warning` (0.3.5→0.3.8 transition, see below) |
| Server | `enable_skills_extension` | With at least one registered skill and the resources capability, declares the skills extension; `skills/list\|get` and `directory/read` take effect | All three methods return `MethodNotFound` (`skills extension is disabled`); no implicit path |
| Server | `enable_apps_extension` / `enable_server_card` / `enable_etag_cache` / `enable_progressive_discovery` / `enable_file_input` / `enable_triggers_events` / `enable_dpop` / `enable_interceptor_alias` | Reserved: declared, default `false`, no gating wired yet | Same as left |
| Client | `enable_tasks_extension` | Declared when the `extensions` map carries the tasks key; `UpdateTask`/`PollTaskToCompletion` and the task-handle path take effect | Those calls throw `MethodNotFound` (`tasks extension is disabled`) |
| Client | `enable_skills_extension` | `ListSkills`/`GetSkill`/`ReadDirectory` take effect | All three calls throw `MethodNotFound` (`skills extension is disabled`) |
| Client | `enable_apps_extension` | Reserved: declared, default `false`, no gating wired yet | Same as left |

Compatibility transition (0.3.5→0.3.8, scope given by this task): `TasksImplicitlyEnabled()` / `SkillsImplicitlyEnabled()` are both marked `[[deprecated]]`, directing users to set the flags explicitly; only tasks has a real implicit path (setting just `task_store` enables it, accompanied by a `Warning` log), while skills has no implicit path and must be enabled explicitly.
