# MCP C++ SDK

> **中文版文档**：[README_zh.md](README_zh.md)

C++17 implementation of the [Model Context Protocol (MCP)](https://modelcontextprotocol.io), providing both client and server libraries for building MCP-based AI tooling integrations.
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
- [Unsupported Declarations](#unsupported-declarations)
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
| OpenSSL        | (optional)     | Required for TLS (WebSocket, SSE HTTPS); OAuth PKCE works without it (built-in SHA-256 fallback). Install: `vcpkg install openssl` / `apt install libssl-dev` / `brew install openssl` |

Supported platforms: **Windows** (MSVC, clang-cl), **Linux** (GCC, Clang), **macOS** (Clang).

## Installation

Consume the SDK in your own CMake project via `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
    mcp-cpp-sdk
    GIT_REPOSITORY https://github.com/jesspig/modelcontextprotocol-cpp-sdk.git
    GIT_TAG        master
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
│  mcp-protocol                       │  WireCodec, McpSessionHandler
├─────────────────────────────────────┤
│  mcp-transport       mcp-http       │  Transports, Streamable HTTP, SSE
├─────────────────────────────────────┤
│  mcp-core                           │  Types, JSON-RPC, interfaces
└─────────────────────────────────────┘
```

Library dependency chain: `mcp-core` (STATIC) → `mcp-transport` → `mcp-protocol` → `mcp-server | mcp-client`. `mcp-http` depends on `mcp-transport`. All libraries are static.

- **mcp-core** — Core types, JSON-RPC message structures, error codes, capabilities, content types, transport interfaces.
- **mcp-transport** — Transport implementations: StdioServerTransport, StdioClientTransport, SseClientTransport, InMemoryTransport, WebSocketClientTransport, StreamableHttpServerTransport, StreamableHttpClientTransport. Platform-specific I/O in `detail/` (posix, win32).
- **mcp-protocol** — `McpSessionHandler` (JSON-RPC engine), dual-era `WireCodec` (2025-11-25 / 2026-07-28), request/response correlation with timeout, `MessageFilter` pipeline.
- **mcp-server** — `McpServer` with tool/resource/prompt registration, `IMcpTaskStore` (incl. `FileTaskStore`), MRTR (`InputRequiredResult`), server → client elicitation, subscription management.
- **mcp-client** — `McpClient` with server discovery (`server/discover` → initialize fallback), version negotiation, OAuth (PKCE/DCR), MRTR (InputRequired loop), tool cache, `FileTokenCache`.
- **mcp-http** — HTTP server for Streamable HTTP mode: `HttpServer`, `EventStore` (SSE event persistence), `StreamableHttpServerTransport`, `StreamableHttpClientTransport`.

### All transports

| Transport       | Client | Server | Description                                      |
|-----------------|--------|--------|--------------------------------------------------|
| Stdio           | Yes    | Yes    | stdin/stdout pipes, subprocess communication     |
| Streamable HTTP | Yes    | Yes    | HTTP POST with JSON/session-mode responses       |
| SSE             | Yes    | Yes¹   | Server-Sent Events for push notifications        |
| WebSocket       | Yes    | No     | TCP-based bidirectional (self-hosted WebSocketClient)  |
| InMemory        | Yes    | Yes    | In-process transport for testing                 |

> ¹ SSE server-side is provided through `StreamableHttpServerTransport` when `enable_legacy_sse` is `true`, which serves an SSE stream via HTTP GET.

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
    JsonValue::Object{{"text", "Hello, MCP!"}});
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

**113 conformance tests** covering protocol type serialization across both eras:

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

| Language   | Repository                                                       | Tier   |
|------------|-----------------------------------------------------------------|--------|
| Python     | [modelcontextprotocol/python-sdk](https://github.com/modelcontextprotocol/python-sdk) | Tier 1 |
| TypeScript | [modelcontextprotocol/typescript-sdk](https://github.com/modelcontextprotocol/typescript-sdk) | Tier 1 |
| Go         | [modelcontextprotocol/go-sdk](https://github.com/modelcontextprotocol/go-sdk) | Tier 1 |
| C#         | [modelcontextprotocol/csharp-sdk](https://github.com/modelcontextprotocol/csharp-sdk) | Tier 1 |
| Rust       | [modelcontextprotocol/rust-sdk](https://github.com/modelcontextprotocol/rust-sdk) | Tier 1 |
| Ruby       | [modelcontextprotocol/ruby-sdk](https://github.com/modelcontextprotocol/ruby-sdk) | Tier 1 |
| Java       | [modelcontextprotocol/java-sdk](https://github.com/modelcontextprotocol/java-sdk) | Tier 2 |
| Swift      | [modelcontextprotocol/swift-sdk](https://github.com/modelcontextprotocol/swift-sdk) | Tier 3 |
| PHP        | [modelcontextprotocol/php-sdk](https://github.com/modelcontextprotocol/php-sdk) | Tier 3 |
| Kotlin     | [modelcontextprotocol/kotlin-sdk](https://github.com/modelcontextprotocol/kotlin-sdk) | Tier 3 |

> Tiers follow the [official SDK list (2026-07-28 revision)](https://modelcontextprotocol.io/docs/2026-07-28/sdk): 6 Tier 1 (TypeScript, Python, C#, Go, Rust, Ruby — Rust promoted to Tier 1 via [PR #3287](https://github.com/modelcontextprotocol/modelcontextprotocol/pull/3287) on 2026-08-21), Java is Tier 2, Swift/PHP/Kotlin are Tier 3. Tier rules: [SDK Tiering System](https://modelcontextprotocol.io/community/sdk-tiers) — experimental features and protocol extensions (such as Tasks and MCP Apps) are not required for any tier.

### Ruby SDK cross-check

Implementation notes on the official Ruby SDK (`mcp` gem, `main` branch as of 2026-10-11; latest release `1.7.0` dated 2026-10-05, see [CHANGELOG.md](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/CHANGELOG.md) and [version.rb](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/lib/mcp/version.rb)), compared with this SDK per dimension:

| Dimension | Ruby (`mcp` gem) | This SDK |
|-----------|------------------|----------|
| Spec revisions | 2025-06-18, 2025-11-25, 2026-07-28 ([ROADMAP.md](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/ROADMAP.md), Current Status) | 2024-11-05…2026-07-28, dual-era codec (see Protocol Versions above) |
| `server/discover` | Modern era never negotiated via `initialize` (CHANGELOG §1.2.0); `server/discover` finalized with modern client support; per-transport era lock on stdio, sessionless modern path on Streamable HTTP ([methods.rb](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/lib/mcp/methods.rb)) | Auto mode with transport-classified fallback ([version-negotiation](wiki/concepts/version-negotiation.md)) |
| MRTR | SEP-2322 `input_required` results with `inputRequests`/`inputResponses` + `requestState`, fulfilled on the legacy wire too (CHANGELOG §0.24.0/§1.2.0); `resultType` is only `complete`/`input_required` ([result_type.rb](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/lib/mcp/result_type.rb)) | `InputRequiredResult` + elicitation loop ([mrtr](wiki/concepts/mrtr.md)); additional `task` resultType for task handles |
| Headers | Sends SEP-2243 `Mcp-Method`/`Mcp-Name`, mirrors `x-mcp-header` params into `Mcp-Param-*` (CHANGELOG §0.22.0/§1.2.0; [mcp_param_headers.rb](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/lib/mcp/client/mcp_param_headers.rb)); `Mcp-Method` required on the modern path | `Mcp-Name` validation incl. tasks `taskId` check ([mcp-param-headers](wiki/concepts/mcp-param-headers.md), [tasks-extension](wiki/concepts/tasks-extension.md)) |
| Cache | Server stamps SEP-2549 `ttlMs`/`cacheScope` (CHANGELOG §0.24.0/§1.2.0); no client-side response cache found under `lib/mcp/client/`【假设：未实现，待源码核实】 | Client response cache with TTL + public/private partitions ([response-cache](wiki/concepts/response-cache.md)) |
| Auth | Full OAuth 2.1 client (DCR, PKCE, CIMD, `client_credentials`, `private_key_jwt`, `jwt-bearer`, RFC 9207 `iss`, PRM-first discovery; CHANGELOG §0.17.0–§1.6.0) + server resource-server protection ([README Features](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/README.md)) | PKCE/DCR + `FileTokenCache`, server Bearer auth (see OAuth Support above) |
| Tasks (SEP-2663) | Not implemented — "remains planned" ([ROADMAP.md](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/ROADMAP.md), [#391](https://github.com/modelcontextprotocol/ruby-sdk/issues/391)) | `tasks/get\|update\|cancel` dual-era + `FileTaskStore` ([tasks-extension](wiki/concepts/tasks-extension.md)) |
| Skills (SEP-2640) | Not implemented【假设：`methods.rb` 方法全集、`lib/mcp/`、`lib/mcp/client/`、`test/mcp/` 均无 skills 条目，CHANGELOG 全版本无 skills 记载】 | `skills/list\|get` + `resources/directory/read`, 2026-only ([skills-extension](wiki/concepts/skills-extension.md)) |

## Unsupported Declarations

The following items are explicitly declared unsupported in this round; when a peer sends them, this SDK degrades gracefully (ignores the unknown field or falls back to the baseline path) instead of failing the session.

- **Apps UI rendering** — server-pushed UI components are not rendered and pass through as opaque data only.
- **EMA** — EMA negotiation is not implemented and peers fall back to baseline capability negotiation.
- **DPoP** — DPoP-bound tokens are neither sent nor validated, and the Bearer-token path in OAuth Support above remains the supported flow.
- **jwt-bearer** — `jwt-bearer` / `private_key_jwt` client authentication is not supported, and the public-client PKCE/DCR flow remains the supported path.
- **ETags** — `ETag` / `If-None-Match` validators are not stored or compared, and cached responses are governed by TTL only.
- **Progressive discovery** — incremental capability discovery is not implemented, and the full `server/discover` (modern) / `initialize` (legacy) handshake is always used.
- **File Uploads** — resumable file-upload primitives are not implemented, and file content moves through existing tool/resource payloads only.
- **Server Cards** — server cards are not surfaced, and `serverInfo` from the handshake remains the identity source.

### Breaking changes in this round

Per the 2026-07-28 spec revision, each change is summarized in one sentence followed by its migration note.

- **URL elicitation modern semantics** — `url` elicitation follows modern-era semantics only. Migration: in the modern era `ElicitUrl` no longer waits on `elicitationId` + `notifications/elicitation/complete`; instead the server converts the request into an MRTR input round-trip (`inputRequests["elicitation"]`, `requestState`-signed when `request_state_key` is set) fulfilled via `tasks/update` `inputResponses`, and the client handles it with `SetUrlElicitationHandler` (decline by default); the legacy `elicitationId` wait path is unchanged.
- **`_meta` omission** — empty per-request `_meta` is omitted from the wire instead of being sent as an empty object. Migration: peers must treat a missing `params._meta` as "no metadata" rather than assuming an empty object arrives, and senders need not backfill one; legacy `SendRequest` also drops `params` entirely when it would be empty/null.
- **Redirect tightening** — HTTP redirect handling is stricter than the legacy behavior. Migration: the default policy is `HttpRedirectPolicy::SameOriginOnly` (same scheme/host/port, with `http`→`https` upgrade allowed; cross-origin hops are rejected and strip the `Authorization` header, `301`/`302` POST flips to `GET`, `303` always flips to `GET`); opt out per call via `HttpRequestSpec::redirect_policy` (`FollowAll`/`NoFollow`) and `max_redirects` (default 5), or per transport via `SseClientTransportOptions` / `HttpClientTransportOptions`.
- **Default upper limits** — built-in default caps apply when the peer omits the corresponding limits. Migration: HTTP responses 8 MB (`HttpRequestSpec::max_response_bytes`), SSE events 1 MB (`max_sse_event_bytes` on the SSE/Streamable-HTTP client options), CIMD documents 1 MB (hard cap, best-effort fallback), stdio frames 8 MB (`StdioClientTransportOptions::max_frame_bytes` / `StdioServerTransport` ctor arg), server request bodies 4 MB (`max_request_body_bytes`), tool arguments `ServerOptions::max_tool_input_elements` (over-budget calls return an `is_error` tool result, not a protocol error), skill manifests 512 files / 16 MB total (client-side `CheckSkillLimits`); raise the matching knob, or shard/page the payload, when a peer legitimately exceeds a default.
- **Async cancellation** — `notifications/cancelled` delivery is asynchronous and unordered relative to the cancelled call. Migration: the pending-request callback now runs on a detached thread (inline only as a thread-spawn fallback), so callers must treat cancellation as eventually consistent — keep handlers idempotent and poll `RequestContext::IsCancellationRequested()` for cooperative exit instead of assuming the cancel lands before the response.

> Sources: [official SDK list (2026-07-28 revision)](https://modelcontextprotocol.io/docs/2026-07-28/sdk); [SDK Tiering System](https://modelcontextprotocol.io/community/sdk-tiers) — experimental features and protocol extensions (such as Tasks and MCP Apps) are not required for any tier.

## License

MIT — see [LICENSE](LICENSE).
