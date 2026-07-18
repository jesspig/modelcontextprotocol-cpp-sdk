# Echo Server Example

A minimal MCP server that demonstrates tools, resources, and prompts.

Source: [`examples/EchoServer/`](https://github.com/modelcontextprotocol/cpp-sdk/tree/main/examples/EchoServer)

## Features

- **Tool**: `echo` — echoes input text back
- **Resource**: `echo://static` — returns a stored message
- **Prompt**: `capitalize` — capitalizes input text

## Running

```bash
cmake --preset debug
cmake --build --preset debug
build/debug/examples/EchoServer/EchoServer
```

## Key Code

```cpp
asio::io_context io_ctx;
auto transport = std::make_shared<StdioServerTransport>(io_ctx);
ServerOptions opts;
opts.server_info = Implementation{"EchoServer", "1.0.0"};

auto server = McpServer::Create(transport, opts, &io_ctx);

// Tool
server->RegisterTool("echo",
    ToolOptions{}.Description("Echo input text back"),
    [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
        auto text = ctx.Params().arguments
            ? ctx.Params().arguments->value("text", "")
            : "";
        CallToolResult result;
        result.content.push_back(TextContent{"text", text});
        return result;
    });

server->Run();
```
