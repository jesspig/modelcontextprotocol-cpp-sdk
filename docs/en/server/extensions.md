# Extensions

Protocol extensions are negotiated via the `extensions` map on `ClientCapabilities` and `ServerCapabilities`. Each entry maps an extension identifier to its configuration value.

## Server-Side Auto-Derivation

Setting a `task_store` on `ServerOptions` automatically enables the extensions capability with an empty map:

```cpp
ServerOptions opts;
opts.task_store = std::make_shared<FileTaskStore>("tasks.json");

auto server = McpServer::Create(transport, opts);
```

The `capabilities` field on `ServerOptions` is declared but currently not consumed by the server. Extensions are always auto-derived when a `task_store` is present.

## Extension Convention

| Field | Description |
|-------|-------------|
| Key | Reverse-DNS identifier (e.g., `"io.modelcontextprotocol/tasks"`) |
| Value | Free-form JSON object with the extension's configuration |
