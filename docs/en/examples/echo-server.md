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
#include <mcp/server/McpServer.hpp>
#include <mcp/transport/StdioServerTransport.hpp>

using namespace mcp;
using Ctx = RequestContext<CallToolRequestParams>;

auto transport = std::make_unique<StdioServerTransport>();

ServerOptions opts;
opts.server_info = Implementation{"EchoServer", "1.0.0"};
opts.server_instructions = "An echo server — sends back what you send.";

auto server = McpServer::Create(std::move(transport), opts);

// Tool — echoes input text
server->RegisterTool("echo",
    ToolOptions{}.Description("Echo the input text back"),
    std::function<CallToolResult(const Ctx&)>(
        [](const Ctx& ctx) -> CallToolResult {
            std::string text;
            if (ctx.Params().arguments && ctx.Params().arguments->Contains("text")) {
                text = (*ctx.Params().arguments)["text"].GetString();
            }
            CallToolResult result;
            result.content.push_back(TextContent{"text", text});
            return result;
        }));

// Static resource
server->RegisterResource("echo-static", "echo://static",
    ResourceOptions{}.Description("Static echo resource"),
    [](const std::string& uri) -> ReadResourceResult {
        TextResourceContents trc;
        trc.uri = uri;
        trc.text = "Echo!";
        ReadResourceResult rr;
        rr.contents = {ResourceContents{trc}};
        return rr;
    });

// Template resource — echoes the URI parameter
server->RegisterResourceTemplate("echo-template", "echo://{text}",
    ResourceOptions{}.Description("Echo the URI parameter"),
    [](const std::string& uri,
       const std::map<std::string, std::string>& vars) -> ReadResourceResult {
        TextResourceContents trc;
        trc.uri = uri;
        trc.text = "Echo: " + vars.at("text");
        ReadResourceResult rr;
        rr.contents = {ResourceContents{trc}};
        return rr;
    });

// Prompt — capitalizes input text
server->RegisterPrompt("capitalize",
    PromptOptions{}.Description("Capitalize the input text"),
    [](const std::string& name,
       const std::optional<JsonValue>& args) -> GetPromptResult {
        std::string text;
        if (args && args->Contains("text"))
            text = (*args)["text"].GetString();
        for (auto& c : text) c = static_cast<char>(std::toupper(c));
        GetPromptResult r;
        r.messages = {PromptMessage{"user", TextContent{"text", text}}};
        return r;
    });

server->Run();
```
