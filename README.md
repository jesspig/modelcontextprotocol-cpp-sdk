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
| libhv          | 1.3.4          | Fetched automatically        |
| simdjson       | 3.12.3         | Fetched automatically        |
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

Available library targets: `mcp-core`, `mcp-transport`, `mcp-protocol`, `mcp-server`, `mcp-client`, `mcp-http`. All libraries are static.

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

Library dependency chain: `mcp-core` (STATIC) → `mcp-transport` → `mcp-protocol` → `mcp-server | mcp-client`. `mcp-http` depends on `mcp-transport`. All libraries are static.

- **mcp-core** — Core types, JSON-RPC message structures, error codes, capabilities, transport interfaces.
- **mcp-transport** — Transport implementations: stdio (client/server), SSE client, WebSocket (simplified), in-memory for testing.
- **mcp-protocol** — `McpSessionHandler` (JSON-RPC engine), dual-era `WireCodec` (2025-11-25 / 2026-07-28), request/response correlation, `MessageFilter` pipeline.
- **mcp-server** — `McpServer` with tool/resource/prompt registration, `IMcpTaskStore` (incl. `FileTaskStore`), MRTR (`InputRequiredResult`), server → client elicitation.
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

using namespace mcp;

int main() {
    auto transport = std::make_shared<StdioServerTransport>();

    ServerOptions opts;
    opts.server_info = Implementation{"MyServer", "1.0.0"};

    auto server = McpServer::Create(transport, opts);

    server->RegisterTool("echo",
        ToolOptions{}.Description("Echo input text back"),
        [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
            std::string text;
            if (ctx.Params().arguments) {
                auto* v = ctx.Params().arguments->Find("text");
                if (v) text = v->GetString();
            }
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

StdioClientTransportOptions transport_opts;
transport_opts.command = "path/to/server";
auto factory = std::make_shared<StdioClientTransport>(transport_opts);
auto transport = factory->Connect();
ClientOptions opts;
opts.client_info = Implementation{"MyClient", "1.0.0"};

auto client = McpClient::Create(transport, opts);

auto tools = client->ListTools();
for (const auto& tool : tools.tools) {
    std::cout << tool.name << "\n";
}

auto result = client->CallTool("echo",
    JsonValue::FromObject({{"text", "Hello, MCP!"}}));
```

## OAuth Support

The client supports the MCP OAuth authorization flow:

- **Authorization Code + PKCE** flow with S256 code challenge
- Dynamic Client Registration (DCR)
- Token refresh and revocation
- Pluggable token cache (`ITokenCache`), persistent `FileTokenCache` included

```cpp
OAuthClientOptions oauth_opts;
oauth_opts.server_url = "https://auth.server.com/.well-known/oauth-authorization-server";
oauth_opts.client_id = "client-id";
oauth_opts.redirect_uri = "http://localhost:3000/callback";

OAuthClientProvider auth(oauth_opts);
auth.Authenticate();
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
