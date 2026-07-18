# Extensions

Protocol extensions are negotiated via the `extensions` map on `ClientCapabilities` and `ServerCapabilities`. Each entry maps an extension identifier to its configuration value.

## Server-Side

```cpp
ServerCapabilities caps;
caps.extensions["io.modelcontextprotocol/tasks"] = {
    {"supported", true},
    {"version", "1.0.0"}
};

auto server = McpServer::Create(transport,
    ServerOptions{}.Capabilities(caps));
```

## Client-Side

```cpp
ClientCapabilities caps;
caps.extensions["io.modelcontextprotocol/tasks"] = {
    {"supported", true}
};

auto client = McpClient::Create(transport,
    ClientOptions{}.Capabilities(caps));
```

## Extension Convention

| Field | Description |
|-------|-------------|
| Key | Reverse-DNS identifier (e.g., `"io.modelcontextprotocol/tasks"`) |
| Value | Free-form JSON object with the extension's configuration |
