# MCP C++ SDK

> **中文版文档**：[README_zh.md](README_zh.md)

C++17 implementation of the [Model Context Protocol (MCP)](https://modelcontextprotocol.io), providing both client and server libraries for building MCP-based AI tooling integrations.
[![MCP](https://badge.mcpx.dev/?type=plugin&plugin_id=github.com/jesspig/GodotMind&logo=true)](https://modelcontextprotocol.io)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platform](https://img.shields.io/badge/platform-windows%20%7C%20linux%20%7C%20macos-lightgrey.svg)

<details>
<summary>Table of Contents</summary>

- [What is MCP?](#what-is-mcp)
- [Requirements](#requirements)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Architecture](#architecture)
- [Transports](#transports)
- [Usage](#usage)
- [OAuth Support](#oauth-support)
- [Protocol Versions](#protocol-versions)
- [Conformance Tests](#conformance-tests)
- [Examples](#examples)
- [Related Projects](#related-projects)
- [License](#license)

</details>

## What is MCP?

The [Model Context Protocol](https://modelcontextprotocol.io) lets you build servers that expose data and functionality to LLM applications in a secure, standardized way. With this SDK you can:

- **Build MCP servers** that expose tools, resources, and prompts to any MCP host
- **Build MCP clients** that connect to any MCP server
- Support every standard transport: stdio, Streamable HTTP, SSE, WebSocket

## Requirements

| Dependency     | Minimum Version | Notes                        |
|----------------|----------------|------------------------------|
| CMake          | 3.28           | Generator: Ninja recommended |
| C++ Compiler   | C++17          | MSVC, Clang, GCC             |
| asio           | 1.30.2         | Fetched automatically        |
| nlohmann-json  | 3.11.3         | Fetched automatically        |
| OpenSSL        | (optional)     | Required for TLS (WebSocket, SSE HTTPS, OAuth). Install: `vcpkg install openssl` / `apt install libssl-dev` / `brew install openssl` |

Supported platforms: **Windows** (MSVC, clang-cl), **Linux** (GCC, Clang), **macOS** (Clang).

## Installation

Consume the SDK in your own CMake project via `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
    mcp-cpp-sdk
    GIT_REPOSITORY https://github.com/modelcontextprotocol/cpp-sdk
    GIT_TAG        main
)
FetchContent_MakeAvailable(mcp-cpp-sdk)

target_link_libraries(your_target PRIVATE mcp-client mcp-server)
```

Available library targets: `mcp-core` (header-only), `mcp-transport`, `mcp-protocol`, `mcp-server`, `mcp-client`, `mcp-http`.

## Quick Start

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Configure presets: `debug`, `release`. Ninja generator required.

## Architecture

```
┌─────────────────────────────────────┐
│  mcp-server        mcp-client       │  Server & Client API
├─────────────────────────────────────┤
│  mcp-protocol                       │  WireCodec, version negotiation
├─────────────────────────────────────┤
│  mcp-transport                      │  Stdio, SSE, WebSocket, Streamable HTTP
├─────────────────────────────────────┤
│  mcp-core            mcp-http       │  Types, JSON-RPC, HTTP serving
└─────────────────────────────────────┘
```

Library dependency chain: `mcp-core` (INTERFACE) → `mcp-transport` → `mcp-protocol` → `mcp-server | mcp-client`. `mcp-http` depends on `mcp-transport`.

- **mcp-core** — Header-only. All MCP protocol types (`Tool`, `Resource`, `Prompt`, `ElicitResult`, etc.), JSON-RPC message structures, error codes, capabilities, transport interfaces.
- **mcp-transport** — Transport implementations: stdio (client/server), SSE client, WebSocket (simplified), in-memory for testing.
- **mcp-protocol** — `McpSessionHandler` (JSON-RPC engine), dual-era `WireCodec` (2025-11-25 / 2026-07-28), request/response correlation, `MessageFilter` pipeline.
- **mcp-server** — `McpServer` with tool/resource/prompt registration, `Extension` framework, `IMcpTaskStore` (incl. `FileTaskStore`), MRTR (`InputRequiredResult`), server → client elicitation.
- **mcp-client** — `McpClient` with server discovery, version negotiation, OAuth (PKCE/DCR), MRTR driver, tool cache, `FileTokenCache`.
- **mcp-http** — HTTP server for Streamable HTTP mode and SSE endpoint serving.

### All transports

| Transport       | Client | Server | Description                                   |
|-----------------|--------|--------|-----------------------------------------------|
| Stdio           | Yes    | Yes    | stdin/stdout pipes                            |
| Streamable HTTP | Yes    | Yes    | HTTP POST with streaming responses            |
| SSE             | Yes    | No     | Server-Sent Events for server → client push   |
| WebSocket       | Yes    | No     | TCP-based bidirectional (simplified)          |
| InMemory        | Yes    | Yes    | In-process transport for testing              |

## Usage

### Server

```cpp
#include <mcp/server/McpServer.hpp>
#include <mcp/transport/StdioServerTransport.hpp>
#include <asio/io_context.hpp>

using namespace mcp;

int main() {
    asio::io_context io_ctx;
    auto transport = std::make_unique<StdioServerTransport>(io_ctx);

    ServerOptions opts;
    opts.server_info = Implementation{"MyServer", "1.0.0"};

    auto server = McpServer::Create(std::move(transport), opts, &io_ctx);

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

### Client

```cpp
#include <mcp/client/McpClient.hpp>
#include <mcp/transport/StdioClientTransport.hpp>

using namespace mcp;

auto transport = std::make_unique<StdioClientTransport>("path/to/server");
ClientOptions opts;
opts.client_info = Implementation{"MyClient", "1.0.0"};

auto client = McpClient::Create(std::move(transport), opts);

auto tools = client->ListTools();
for (const auto& tool : tools.tools) {
    std::cout << tool.name << "\n";
}

auto result = client->CallTool("echo", nlohmann::json{{"text", "Hello, MCP!"}});
```

## OAuth Support

The client supports the MCP OAuth authorization flow:

- **Authorization Code + PKCE** flow with S256 code challenge
- Dynamic Client Registration (DCR)
- Token refresh and revocation
- Pluggable token cache (`ITokenCache`), persistent `FileTokenCache` included

```cpp
auto oauth = std::make_shared<OAuthClientProvider>(
    "https://auth.server.com/.well-known/oauth-authorization-server",
    "client-id");
client->SetOAuthProvider(oauth);
```

## Protocol Versions

| Version     | Status  | Key Features                      |
|-------------|---------|-----------------------------------|
| 2025-11-25  | Legacy  | `initialize` handshake, standalone sampling/roots/list |
| 2026-07-28  | Current | `server/discover`, per-request `_meta`, MRTR (`InputRequiredResult`), `subscriptions/listen` |

The `WireCodec` factory auto-selects the correct codec for the negotiated version via a simple string comparison (`version >= "2026-07-28"`).

## Conformance Tests

**122 conformance tests** covering protocol type serialization across both eras:

- JSON-RPC message round-trips (request, notification, response, error)
- WireCodec era-gating (2025 vs 2026 method/notification sets)
- Tool, Resource, Prompt serialization with annotations and icons
- Content variant dispatch (text, image, audio, embedded resource)
- Elicitation and ElicitResultTyped<T>
- MRTR (InputRequiredResult, InputRequests, factory helpers)
- Structured meta (RequestMetaObject, NotificationMetaObject)
- Extensions capability, ResultType enum, SubscriptionFilter
- Tasks (get/update/cancel), Logging (8 levels)
- Pagination, caching, protocol version helpers

## Examples

Runnable examples in [`examples/`](examples/):

| Example                          | Description                                    |
|----------------------------------|------------------------------------------------|
| [EchoServer](examples/EchoServer/)   | Minimal server with tool, resource, and prompt |
| [WeatherServer](examples/WeatherServer/) | Server with external API integration     |
| [SimpleClient](examples/SimpleClient/) | Client that connects to a server in-process |

Build and run:

```bash
cmake --preset debug -DMCP_BUILD_EXAMPLES=ON
cmake --build --preset debug
# Run the echo server:
build/debug/examples/EchoServer/EchoServer
```

## References

This SDK was developed against the official MCP protocol specification and reference implementations:

| Language   | Repository                                                       |
|------------|-----------------------------------------------------------------|
| Python     | [modelcontextprotocol/python-sdk](https://github.com/modelcontextprotocol/python-sdk) |
| TypeScript | [modelcontextprotocol/typescript-sdk](https://github.com/modelcontextprotocol/typescript-sdk) |
| Go         | [modelcontextprotocol/go-sdk](https://github.com/modelcontextprotocol/go-sdk) |
| C#         | [modelcontextprotocol/csharp-sdk](https://github.com/modelcontextprotocol/csharp-sdk) |
| Java       | [modelcontextprotocol/java-sdk](https://github.com/modelcontextprotocol/java-sdk) |
| Rust       | [modelcontextprotocol/rust-sdk](https://github.com/modelcontextprotocol/rust-sdk) |

## License

MIT — see [LICENSE](LICENSE).
