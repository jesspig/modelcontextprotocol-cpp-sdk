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

## Extension Convention

| Field | Description |
|-------|-------------|
| Key | Reverse-DNS identifier (e.g., `"io.modelcontextprotocol/tasks"`) |
| Value | Free-form JSON object with the extension's configuration |
