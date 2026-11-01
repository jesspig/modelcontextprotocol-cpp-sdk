# Echo server, section by section

`examples/EchoServer` is the repo's smallest server that covers all four primitives: one tool, one static resource, one resource template, one prompt, all over stdio. Reading each call's actual signature builds the mental model faster than a field table. Source: [`examples/EchoServer/main.cpp`](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/examples/EchoServer/main.cpp).

## What this example demonstrates

- Starting a server with `StdioServerTransport` rather than HTTP.
- `McpServer::Create` takes a `std::unique_ptr<ITransport>`, so ownership of the transport moves into the server.
- The minimal shapes of the four registration APIs: `RegisterTool`, `RegisterResource`, `RegisterResourceTemplate`, `RegisterPrompt`.
- Tool arguments must be read from `ctx.Params().arguments` (a `std::optional<JsonValue>`), so the emptiness check is mandatory.

## Assembly skeleton

```cpp
auto transport = std::make_unique<StdioServerTransport>();

ServerOptions opts;
opts.server_info = Implementation{"EchoServer", "1.0.0"};
opts.server_instructions = "An echo server — sends back what you send.";

auto server = McpServer::Create(std::move(transport), opts);
```

From `examples/EchoServer/main.cpp:13-19`. Three things worth noting:

- `server_instructions` becomes the `instructions` field of the `initialize` response and the client can read it (`McpClient::GetInstructions()`).
- `Implementation{name, version}` is the server identity, surfacing in `tools/list` `_meta` and the server card.
- The example does not set `ServerOptions::protocol_version`, so negotiation decides; pin it explicitly when you need determinism (see [/en/learn/protocol-versions](/en/learn/protocol-versions)).

## Registering the tool: echo

```cpp
server->RegisterTool("echo",
    ToolOptions{}.Description("Echo the input text back"),
    std::function<CallToolResult(const Ctx&)>(
        [](const Ctx& ctx) -> CallToolResult {
            auto& params = ctx.Params();
            std::string text;
            if (params.arguments && params.arguments->Contains("text")) {
                text = (*params.arguments)["text"].GetString();
            }
            CallToolResult result;
            result.content.push_back(TextContent{"text", text});
            return result;
        }));
```

From `examples/EchoServer/main.cpp:21-33`, with `using Ctx = RequestContext<CallToolRequestParams>;` at `:10`.

- The real tool name comes from the **first argument** `"echo"`; `ToolOptions` carries only the description (the example never sets `name`, and that field has no consumer).
- **No `InputSchema` is declared here**, so the SDK performs no argument pre-validation; the example guards itself with `Contains("text")`. To see what changes once you declare a schema, read [/en/howto/validate-tool-input](/en/howto/validate-tool-input).
- The explicit `std::function<CallToolResult(const Ctx&)>` wrapper is required: a bare lambda cannot be deduced against the overloaded registration interface.
- `TextContent{"text", text}` is aggregate initialisation in the order `{type, text, mime_type, annotations, meta}` (`include/mcp/Content.hpp:42-47`).

## Registering static and templated resources

```cpp
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
```

From `examples/EchoServer/main.cpp:35-44`. A resource handler receives only the `uri`; contents go into `contents`, and the `uri` field must echo back what was requested.

```cpp
server->RegisterResourceTemplate("echo-template",
    "echo://{text}",
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
```

From `examples/EchoServer/main.cpp:46-57`. A template handler gets the parsed variable map as a second argument, so `vars.at("text")` reads the `{text}` placeholder. Registration validates the template first and throws `InvalidParams` at the call site when it is malformed (`src/server/McpServer.cpp:329`) — never deferred to the first read.

## Registering the prompt: capitalize

```cpp
server->RegisterPrompt("capitalize",
    PromptOptions{}.Description("Capitalize the input text"),
    [](const std::string& name,
       const std::optional<JsonValue>& args) -> GetPromptResult {
        (void)name;
        std::string text;
        if (args && args->Contains("text")) {
            text = (*args)["text"].GetString();
        }
        for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        GetPromptResult r;
        PromptMessage pm;
        pm.role = "user";
        pm.content = TextContent{"text", text};
        r.messages = {std::move(pm)};
        return r;
    });
```

From `examples/EchoServer/main.cpp:59-75`.

- A prompt handler receives `name` and `args` and returns `GetPromptResult.messages`, each with a `role` and `content`.
- The cast to `unsigned char` before `std::toupper` is deliberate: `char` may be negative, and passing a negative value is undefined behaviour — copy this shape.
- The example declares no `PromptOptions::Arguments()`, so `prompts/list` advertises no argument spec; to tell clients what to pass, see [/en/howto/serve-a-prompt](/en/howto/serve-a-prompt).

## Running and observing

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMCP_BUILD_EXAMPLES=ON
cmake --build build/debug --target EchoServer
build/debug/examples/EchoServer/EchoServer
```

Measured output (Windows, clang-cl):

```
EchoServer starting on stdio...
```

The banner goes to **stderr** (`examples/EchoServer/main.cpp:77`) while stdout is reserved for protocol frames — exactly why `StdioClientTransportOptions::stderr_path` is worth configuring. The process then blocks in `server->Run()` (`:78`). This run also confirmed that redirecting stdin to `/dev/null` does **not** make the process exit on its own: a stdio server's lifetime is ended by the host, not by stdin EOF.

To watch it actually answer, use `SimpleClient` (see [/en/examples/simple-client](/en/examples/simple-client)) or any client such as Inspector that can call `tools/list` and `prompts/list`.

## Mapping to the how-to pages

| What you want to do | Go to |
|---|---|
| Add descriptions, titles, input schemas, annotations to a tool | [/en/howto/register-a-tool](/en/howto/register-a-tool) |
| Let the SDK validate arguments for you | [/en/howto/validate-tool-input](/en/howto/validate-tool-input) |
| Resource subscriptions and `list_changed` | [/en/howto/serve-a-resource](/en/howto/serve-a-resource) |
| Prompt arguments and completion | [/en/howto/serve-a-prompt](/en/howto/serve-a-prompt) |
| Serve over Streamable HTTP instead | [/en/howto/run-a-streamable-http-server](/en/howto/run-a-streamable-http-server) |

## Source behind this page

- `examples/EchoServer/main.cpp`
- `examples/EchoServer/CMakeLists.txt`
- `include/mcp/server/McpServer.hpp`, `include/mcp/server/RequestContext.hpp`, `include/mcp/server/ServerOptions.hpp`
- `include/mcp/transport/StdioServerTransport.hpp`
- `include/mcp/Content.hpp`, `include/mcp/McpTypes.hpp`, `include/mcp/Implementation.hpp`
