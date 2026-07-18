# Quick Start

## Build

```bash
# Configure
cmake --preset debug

# Build
cmake --build --preset debug

# Run all 216 tests
ctest --preset debug --output-on-failure
```

Presets: `debug`, `release`. Ninja generator only.

## Minimal Server

```cpp
#include <mcp/server/McpServer.hpp>
#include <mcp/transport/StdioServerTransport.hpp>
#include <asio/io_context.hpp>

using namespace mcp;

int main() {
    asio::io_context io_ctx;
    auto transport = std::make_shared<StdioServerTransport>(io_ctx);

    ServerOptions opts;
    opts.server_info = Implementation{"MyServer", "1.0.0"};

    auto server = McpServer::Create(transport, opts, &io_ctx);

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
    return 0;
}
```

## Minimal Client

```cpp
#include <mcp/client/McpClient.hpp>
#include <mcp/transport/StdioClientTransport.hpp>

using namespace mcp;

int main() {
    StdioClientTransportOptions transport_opts;
    transport_opts.command = "./my-server";
    auto transport = std::make_shared<StdioClientTransport>(transport_opts);

    ClientOptions opts;
    opts.client_info = Implementation{"MyClient", "1.0.0"};

    auto client = McpClient::Create(transport, opts);

    auto tools = client->ListTools();
    for (const auto& tool : tools.tools)
        std::cout << tool.name << "\n";

    auto result = client->CallTool("echo",
        nlohmann::json{{"text", "Hello, MCP!"}});
    return 0;
}
```

## Important Caveat

When using `McpServer::Create` or `McpClient::Create` with a transport, both must share the same `asio::io_context`. Always pass the io_context as the third argument:

```cpp
auto transport = std::make_shared<StdioServerTransport>(io_ctx);
auto server = McpServer::Create(transport, opts, &io_ctx);
```

Omitting the io_context argument creates an internal one, causing silent data loss on the transport channel.
