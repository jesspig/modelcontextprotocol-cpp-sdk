# Simple client, section by section

`examples/SimpleClient` runs **a server and a client inside one process**, joined by `InMemoryTransport::CreatePair()`, and walks the shortest complete loop: negotiate → list tools → call → tear down. It is the most convenient reproduction skeleton when something misbehaves. Source: [`examples/SimpleClient/main.cpp`](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/examples/SimpleClient/main.cpp).

## What this example demonstrates

- No processes, no network: the in-memory transport connects `McpServer` and `McpClient` directly.
- The four client-side reads: `GetServerInfo()`, `GetNegotiatedProtocolVersion()`, `ListTools()`, `CallTool()`.
- The server needs its own thread (because `Run()` blocks) and a matching teardown order.
- Even the exception path must leave no joinable thread behind.

## Assembling both ends in one process

```cpp
auto transport_pair = InMemoryTransport::CreatePair();

ServerOptions srv_opts;
srv_opts.server_info = Implementation{"EchoServer", "1.0.0"};
server = McpServer::Create(
    std::move(transport_pair.server), srv_opts);
```

From `examples/SimpleClient/main.cpp:16-21`. `CreatePair()` returns a `Pair` holding two `shared_ptr<ITransport>` members, `.client` and `.server` (`include/mcp/transport/InMemoryTransport.hpp:10-18`), and `McpServer::Create` takes the transport by move.

The server registers an `echo` tool (`:23-35`) whose handler reads arguments the idiomatic way:

```cpp
if (auto* v = ctx.Params().arguments->Find("text"); v && v->IsString())
    text = v->GetString();
```

It checks `ctx.Params().arguments` (a `std::optional<JsonValue>`) before dereferencing — touching an empty optional is undefined behaviour (`:28-31`).

## The server runs on its own thread

```cpp
server_thread = std::thread([&server]() { server->Run(); });
```

From `examples/SimpleClient/main.cpp:37`. `Run()` is a blocking message loop; creating the client on the same thread would deadlock, so the example parks the server on a thread and continues on the main one:

```cpp
ClientOptions cl_opts;
cl_opts.client_info = Implementation{"SimpleClient", "1.0.0"};
cl_opts.connect_mode = ConnectMode::Auto;

auto client = McpClient::Create(
    std::move(transport_pair.client), cl_opts);
```

From `:39-44`. `ConnectMode::Auto` probes first and then negotiates (the in-memory transport counts as stdio-like, so a failed probe falls back to `initialize` — see [/en/learn/protocol-versions](/en/learn/protocol-versions)); `McpClient::Create` performs `initialize` and `notifications/initialized` internally, so the client you receive is already usable.

## Reading the negotiation result

```cpp
std::cout << "Server: " << client->GetServerInfo().name
          << " v" << client->GetServerInfo().version << std::endl;
std::cout << "Protocol: " << client->GetNegotiatedProtocolVersion() << std::endl;
```

From `examples/SimpleClient/main.cpp:46-48`. Both reads are **free**: the values come from the cached `initialize` result and issue no request. When you wonder "which era did we agree on?", look at `GetNegotiatedProtocolVersion()` first — it decides how to interpret everything after it.

## Listing tools and calling a tool

```cpp
auto tools = client->ListTools();
std::cout << "Available tools:" << std::endl;
for (const auto& t : tools.tools) {
    std::cout << "  - " << t.name
              << (t.description ? ": " + *t.description : "")
              << std::endl;
}
```

From `examples/SimpleClient/main.cpp:50-56`. `ListTools()` fetches **one page** (more exist whenever `next_cursor` is set); use `ListToolsAll()` to collect everything. `t.description` is a `std::optional<std::string>`, and the ternary avoids printing an empty option.

```cpp
JsonValue args((JsonValue::Object{{"text", JsonValue("Hello, MCP!")}}));
auto result = client->CallTool("echo", args);
for (const auto& content : result.content) {
    if (auto* text = std::get_if<TextContent>(&content)) {
        std::cout << "Response: " << text->text << std::endl;
    }
}
```

From `:59-65`. Three idioms worth calling out:

- The second `CallTool(name, arguments)` parameter is a `std::optional<JsonValue>`; the example builds it inline with `JsonValue::Object{...}`.
- `result.content` is a `std::vector<ContentVariant>` — a variant array of mixed content types — so you must **test with `std::get_if<TextContent>` before touching `.text`**; assuming everything is text breaks on images or resource links.
- `CallTool` is a convenience method: protocol-level failures throw `McpError` (tool-level failures arrive as `result.is_error`), which is why the surrounding `catch` matters — next section.

## Teardown order and the exception path

```cpp
client->Close();
server->Close();
server_thread.join();
```

From `examples/SimpleClient/main.cpp:67-69`: close the client first, then the server, then join. The exception path must also retire the thread rather than leaving it joinable at exit:

```cpp
} catch (const std::exception& e) {
    if (server_thread.joinable()) {
        if (server) server->Close();
        server_thread.join();
    }
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
}
```

From `:73-80`. `Close()` is idempotent, so "close once on success, close again on failure" is safe (in-flight settlement semantics of `Close()` are in [/en/howto/connect-to-a-server](/en/howto/connect-to-a-server)).

## Running and measured output

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMCP_BUILD_EXAMPLES=ON
cmake --build build/debug --target SimpleClient
build/debug/examples/SimpleClient/SimpleClient
```

Measured output (Windows, clang-cl, return code 0):

```
Server: EchoServer v1.0.0
Protocol: 2026-07-28
Available tools:
  - echo: Echo input

Calling echo tool with 'Hello, MCP!'...
Response: Hello, MCP!

Done!
```

`Protocol: 2026-07-28` shows the in-memory pair negotiated the modern era under `Auto`; switching to `ConnectMode::Legacy` prints an earlier version, and the behavioural differences are in [/en/learn/protocol-versions](/en/learn/protocol-versions).

## Mapping to the how-to pages

| What you want to do | Go to |
|---|---|
| Connect to a real server over stdio or HTTP | [/en/howto/connect-to-a-server](/en/howto/connect-to-a-server) |
| Paging, search and cache hints | [/en/howto/call-tools-and-read-resources](/en/howto/call-tools-and-read-resources) |
| Cancellation and timeouts | [/en/howto/cancel-a-request](/en/howto/cancel-a-request) |
| Making sense of error codes | [/en/howto/handle-errors](/en/howto/handle-errors) |
| Full `McpClient` method table | [/en/reference/client](/en/reference/client) |

## Source behind this page

- `examples/SimpleClient/main.cpp`
- `examples/SimpleClient/CMakeLists.txt`
- `include/mcp/transport/InMemoryTransport.hpp`
- `include/mcp/client/McpClient.hpp`, `include/mcp/client/ClientOptions.hpp`
- `include/mcp/server/McpServer.hpp`, `include/mcp/server/RequestContext.hpp`
- `include/mcp/Content.hpp` (`ContentVariant`, `TextContent`)
