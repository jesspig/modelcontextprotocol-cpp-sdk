# Quick Start

Get a server and a client talking in five minutes: register a tool, list tools, call a tool, read the result back. Everything runs in one process over the in-memory transport, so no external host is required.

## Prerequisites

- CMake 3.28+ and Ninja configured as described in [Installation and Integration](/en/start/installation).
- A checkout of this repository (example programs are not built by default; the switch must be turned on).

## 1. Run the shipped example

```bash
cmake --preset debug -DMCP_BUILD_EXAMPLES=ON
cmake --build --preset debug --target SimpleClient
./build/debug/examples/SimpleClient/SimpleClient        # SimpleClient.exe on Windows
```

Expected output (measured):

```text
Server: EchoServer v1.0.0
Protocol: 2026-07-28
Available tools:
  - echo: Echo input

Calling echo tool with 'Hello, MCP!'...
Response: Hello, MCP!

Done!
```

Note the second line: the in-process `InMemoryTransport` pair lets the client probe `server/discover` in the default `Auto` mode, and a successful probe negotiates the modern era (`2026-07-28`). If the handshake goes through `initialize`, the negotiated result can only land in the legacy era (at most `2025-11-25`) — see [Protocol Versions](/en/learn/protocol-versions).

## 2. Write a minimal server

```cpp
#include <mcp/Content.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/RequestContext.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/transport/StdioServerTransport.hpp>

using namespace mcp;

int main() {
    ServerOptions options;
    options.server_info = Implementation{"my-server", "1.0.0"};

    auto server = McpServer::Create(std::make_shared<StdioServerTransport>(), options);

    server->RegisterTool(
        "greet",
        ToolOptions{}.Description("Say hello"),
        [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
            CallToolResult result;
            result.content.push_back(TextContent{"text", "hello"});
            return result;
        });

    server->Run();
    return 0;
}
```

Key points:

- `McpServer::Create` returns `std::unique_ptr`; the transport is passed as `std::shared_ptr<ITransport>`.
- `ToolOptions{}.Description(...)` is a chainable setter; assign the remaining fields directly.
- `Run()` blocks in the server loop and only returns after `Close()`; never call `Close()` from a callback running on the IO thread — it would join itself.
- Full registration shapes (resources, templates, prompts, skills) live in [Register a Tool](/en/howto/register-a-tool) and [Server Reference](/en/reference/server).

## 3. Connect to it

- stdio: point any MCP host (Claude Desktop, IDE plugins) at your executable, including the `.exe` suffix on Windows.
- End-to-end self test: `examples/SimpleClient/main.cpp` starts both ends in one process through `InMemoryTransport::CreatePair()` — the fastest way to debug.
- Out-of-process check: build `EchoServer` (`cmake --build --preset debug --target EchoServer`) and connect the official MCP Inspector over stdio.

## 4. Verify your changes

```bash
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Expect: a clean build and the full suite passing (currently 18 test targets, 982 registered cases on Windows). When you only touched an example, build just that target: `cmake --build --preset debug --target SimpleClient`.

## Common failures

| Symptom | Cause | Fix |
|---|---|---|
| `No rule to make target 'SimpleClient'` | `-DMCP_BUILD_EXAMPLES=ON` was not passed | Reconfigure, then build that target |
| `cannot open file '.../SimpleClient'` | Missing `.exe` on Windows, or the preset output directory is not `build/debug` | Add the suffix per platform |
| Host connects but nothing happens | Server writes business logs to stdout and corrupts stdio framing | Internal logging is off by default; debug with `MCP_LOG_LEVEL=4` and never log to stdout |
| Tool call returns `is_error` with `invalid arguments: ...` | Arguments did not match `input_schema`; the SDK rejects them before the handler runs | See [Validate Tool Input](/en/howto/validate-tool-input) |

## Next steps

- Server capabilities in depth: [Register a Tool](/en/howto/register-a-tool), [Serve a Resource](/en/howto/serve-a-resource), [Collect User Input](/en/howto/collect-user-input)
- Transport choice: [Transports](/en/learn/transports)
- Signatures and fields: [Server Reference](/en/reference/server), [Client Reference](/en/reference/client)

## Source files behind this page

- `examples/SimpleClient/main.cpp`, `examples/EchoServer/main.cpp`, `include/mcp/server/McpServer.hpp`, `include/mcp/transport/InMemoryTransport.hpp`, `CMakePresets.json`
