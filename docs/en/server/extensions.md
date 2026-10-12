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
