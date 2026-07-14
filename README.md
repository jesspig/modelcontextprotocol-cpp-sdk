# MCP C++ SDK

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
- [Build Instructions](#build-instructions)
- [Architecture](#architecture)
- [Transports](#transports)
- [Usage](#usage)
- [OAuth Support](#oauth-support)
- [Protocol Versions](#protocol-versions)
- [Examples](#examples)
- [Documentation](#documentation)
- [Contributing](#contributing)
- [Related Projects](#related-projects)
- [License](#license)

</details>

## What is MCP?

The [Model Context Protocol](https://modelcontextprotocol.io) lets you build servers that expose data and functionality to LLM applications in a secure, standardized way. Think of it like a web API, but designed for LLM interactions. With this SDK you can:

- **Build MCP servers** that expose tools, resources, and prompts to any MCP host
- **Build MCP clients** that connect to any MCP server
- Support every standard transport: stdio, Streamable HTTP, and SSE

## Requirements

| Dependency     | Minimum Version | Notes                        |
|----------------|----------------|------------------------------|
| CMake          | 4.2            | Generator: Ninja recommended |
| C++ Compiler   | C++17          | MSVC, Clang, GCC             |
| asio           | 1.30.2         | Fetched automatically        |
| nlohmann-json  | 3.11.3         | Fetched automatically        |
| OpenSSL        | (optional)     | Required for OAuth PKCE      |

Supported platforms: **Windows** (MSVC, clang-cl), **Linux** (GCC, Clang), **macOS** (Clang).

## Installation

Consume the SDK in your own CMake project via `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
    mcp-cpp-sdk
    GIT_REPOSITORY https://github.com/modelcontextprotocol/cpp-sdk
    GIT_TAG        main  # or a specific release tag
)
FetchContent_MakeAvailable(mcp-cpp-sdk)

target_link_libraries(your_target PRIVATE mcp-client mcp-server)
```

Available library targets: `mcp-core` (header-only), `mcp-transport`, `mcp-protocol`, `mcp-server`, `mcp-client`, `mcp-http`.

## Quick Start

```bash
# Configure
cmake --preset debug

# Build
cmake --build --preset debug

# Run tests
cd build/debug && ctest --output-on-failure
```

## Build Instructions

```bash
# Debug build (auto-detects compiler on any platform)
cmake --preset debug
cmake --build --preset debug

# Release build (optimized with LTO)
cmake --preset release
cmake --build --preset release

# CI build (RelWithDebInfo, no compiler cache)
cmake --preset ci
cmake --build --preset ci
ctest --preset ci
```

Platform-specific presets (`win-msvc-*`, `win-clang-cl-*`, `linux-gcc-*`, `linux-clang-*`) are also available when you need to target a specific compiler.

## Architecture

The SDK is organized into layered components:

```
┌─────────────────────────────────────┐
│  mcp-server        mcp-client       │  — Server & Client API
├─────────────────────────────────────┤
│  mcp-protocol                       │  — WireCodec, version negotiation
├─────────────────────────────────────┤
│  mcp-transport                      │  — I/O transports
├─────────────────────────────────────┤
│  mcp-core            mcp-http       │  — Types, JSON-RPC, HTTP serving
└─────────────────────────────────────┘
```

### Layer Descriptions

- **mcp-core** — Header-only interface library. Defines all MCP protocol types (`Tool`, `Resource`, `Prompt`), JSON-RPC message structures, error codes, capabilities, and the abstract `Transport` interface.
- **mcp-transport** — Transport implementations: stdio (client/server), SSE client, and in-memory transport for testing.
- **mcp-protocol** — Protocol layer with `WireCodec` era-gating (2025 vs 2026 protocol versions), request validation, and result encoding.
- **mcp-server** — Server implementation with tool/resource/prompt registration and request dispatching.
- **mcp-client** — Client implementation with server connection management, request/response handling, and OAuth support.
- **mcp-http** — HTTP server transport for Streamable HTTP mode and SSE endpoint serving.

## Transports

| Transport       | Client | Server | Description                             |
|-----------------|--------|--------|-----------------------------------------|
| Stdio           | Yes    | Yes    | Communicates over stdin/stdout pipes     |
| Streamable HTTP | Yes    | Yes    | HTTP POST with streaming responses       |
| SSE             | Yes    | No     | Server-Sent Events for server → client push |
| InMemory        | Yes    | Yes    | In-process transport for testing         |

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

The client supports the MCP OAuth authorization flow for servers that require authentication:

- **Authorization Code + PKCE** flow with S256 code challenge
- Configurable authorization server metadata
- Token refresh and storage via pluggable `OAuthTokenStore`

Enable OAuth by providing an `OAuthClientProvider` when creating the client:

```cpp
auto oauth = std::make_shared<OAuthClientProvider>(
    "https://auth.server.com/.well-known/oauth-authorization-server",
    "client-id");
client->SetOAuthProvider(oauth);
```

## Protocol Versions

The SDK supports two MCP protocol eras:

| Version     | Status  | Key Features                   |
|-------------|---------|--------------------------------|
| 2025-11-25  | Legacy  | `initialize` handshake         |
| 2026-07-28  | Current | `server/discover`, `_meta` envelope |

The `WireCodec` factory auto-selects the correct codec for the negotiated version:

```cpp
auto codec = MakeWireCodec("2026-07-28");
codec->StampOutgoingRequest(body, meta);
```

## Examples

Runnable examples are located in the [`examples/`](examples/) directory:

| Example                          | Description                                    |
|----------------------------------|------------------------------------------------|
| [EchoServer](examples/EchoServer/)   | Minimal server with tool, resource, and prompt |
| [WeatherServer](examples/WeatherServer/) | Server with external API integration     |
| [SimpleClient](examples/SimpleClient/) | Client that connects to a server in-process |

Build and run an example:

```bash
cmake --preset debug -DMCP_BUILD_EXAMPLES=ON
cmake --build --preset debug
./build/debug/examples/EchoServer/EchoServer
```

## Documentation

- [MCP Documentation](https://modelcontextprotocol.io/docs) — official MCP guides and concepts
- [MCP Specification](https://modelcontextprotocol.io/specification/latest) — protocol specification

## License

MIT — see [LICENSE](LICENSE).
