# Register a tool

`McpServer::RegisterTool` turns a local function into an MCP tool: once registered, clients can discover it through `tools/list`, invoke it through `tools/call`, and have their arguments checked against the `inputSchema` you declare. Signature and field tables live only in [/en/reference/server](/en/reference/server) and [/en/reference/types](/en/reference/types); this page covers how to wire them together.

## Prerequisites

- Headers: `mcp/server/McpServer.hpp`, `mcp/server/RequestContext.hpp`, `mcp/server/ServerOptions.hpp`, `mcp/McpTypes.hpp`, `mcp/JsonValue.hpp`, `mcp/Content.hpp`, `mcp/Implementation.hpp`, `mcp/McpError.hpp`; the build only passes `-I include`, so write the full subdirectory path.
- Transport: hand any `std::shared_ptr<ITransport>` to `McpServer::Create`; the snippets below use the `.server` side of `InMemoryTransport::CreatePair()`, while a real server swaps in a stdio or HTTP server transport (constructors in [/en/reference/transports](/en/reference/transports)).
- Protocol era: registration and the call path are not era-trimmed; all five versions behave the same (see "Protocol-era differences").
- Switches: nothing extra is required; `ServerOptions::max_tool_input_elements` and `enable_file_input` are optional tightening knobs.

## Register a minimal tool

The third argument of `RegisterTool(name, options, fn)` is a `std::function<CallToolResult(const RequestContext<CallToolRequestParams>&)>`, so a capture-less lambda works directly. With `ToolOptions{}` the SDK still synthesizes an `input_schema` of `{"type":"object","properties":{}}` (`McpServerTool.hpp:46-53`), so a tool with no constraints is still valid.

<<< @/snippets/register-a-tool.cpp#minimal

Tool names are validated against `[A-Za-z0-9._-]` with a 128-character cap (`IsValidToolName`); you may register right after `Create()` returns, and registering after the handshake automatically emits `tools/list_changed`.

## Add description, title and input schema

`ToolOptions` exposes exactly three chainable setters returning `*this` — `Description`, `Title`, `InputSchema`; every other field is a plain member assignment. `ToolOptions::name` is never consumed: the effective name is the first argument of `RegisterTool`. Build the schema with the in-house `JsonValue` (`JsonValue::Object` / `JsonValue::Array`), or read it from a string with `JsonValue::Parse(R"({ ... })")`.

<<< @/snippets/register-a-tool.cpp#schema

`Title` lands in `Tool::title` and `Description` in `Tool::description`; both appear verbatim in `tools/list`. Put the human-readable name into `Title` rather than burying it in the description, because clients render the former.

## Declare behaviour hints and icons

Behaviour hints are four `std::optional<bool>` members: `read_only_hint`, `destructive`, `idempotent`, `open_world_hint`. The SDK assembles a `ToolAnnotations` only when at least one of them carries a value; with all four left `nullopt`, `Tool::annotations` stays unset. Icons and metadata go through the member fields `icons` (`std::vector<Icon>`) and `meta` — there are no setters for them.

<<< @/snippets/register-a-tool.cpp#hints

## Read the request context inside the handler

The handler receives a single `RequestContext<CallToolRequestParams>`: `Params()` gives you `name`/`arguments`/`meta`, `SessionId()` gives the protocol session of this request, `Log()` emits log notifications, `LogLevel()`/`IsCancellationRequested()` read the per-request log threshold and the cancellation flag, and `Server()` is the entry point for server-initiated requests.

<<< @/snippets/register-a-tool.cpp#context

::: warning
When passing a trailing session argument to `Elicit`/`ElicitUrl`, use `RequestContext::SessionId()` only; `McpServer::SessionId()` is a random per-instance identifier and means something else entirely. Member table: [/en/reference/server](/en/reference/server).
:::

## Return structured output

Declare `ToolOptions::output_schema` (or set `use_structured_content = true` and the SDK supplies `{"type":"object"}`), then fill both `content` and `structured_content` in the handler. Before the result leaves the server, the SDK validates `structured_content` with the same validator used for arguments.

<<< @/snippets/register-a-tool.cpp#structured

::: danger
A structured-output mismatch is a **protocol error**, not `is_error`: the wire message is `tool output does not match outputSchema: <reason>` with code `InvalidParams` (`McpServer.cpp:2673-2681`). If you declare a schema, you must honour it.
:::

## Catch registration-time errors

Every registration-time failure is `McpError(McpErrorCode::InvalidParams)` and it throws at the `RegisterTool` call site — never lazily on first invocation. Bundling registrations into one function lets the startup path decide between aborting and skipping.

<<< @/snippets/register-a-tool.cpp#registration-errors

::: tip
For a custom execution body (async completion, reading metadata through `ProtocolTool()`), build the object with `McpServerTool::Create` and register the resulting `shared_ptr`; the object path performs no null check, so an empty `shared_ptr` dereferences null and crashes. Async shape: [/en/reference/server](/en/reference/server).
:::

## Complete code

Server identity, instructions, element-count cap, schema, behaviour hints, context use and structured output in one piece:

<<< @/snippets/register-a-tool.cpp#full

## Verify

Snippets are syntax-checked only (`/Zs` or `-fsyntax-only`); no process needs to run. From the worktree root:

```bash
cmake -S . -B build/p2a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p2a --target mcp-doc-snippet-register-a-tool
```

Expected output: configure prints `[mcp-docs] syntax-checking <N> snippet(s) with '/Zs'` (clang-cl) or `'-fsyntax-only'` (Clang/GCC), the build stops at `[N/N] Building CXX object ...` with exit code 0 and no `error:` line; under clang-cl each snippet also emits an unrelated `argument unused during compilation: '-c'` warning, which can be ignored. To check every snippet at once use the aggregate target `--target mcp-docs-snippets` (what CI runs).

Runtime sanity check: `tools/list` must return the name you registered together with `description`/`title`; if nothing shows up, first confirm the transport handed to `Create()` and the one the client connected to are the same pair.

## Protocol-era differences

Neither the registration API nor the serialization of `inputSchema`/`outputSchema`/`annotations` is era-gated; the only differences are the routing of `tools/list_changed` (subscribers in the modern era, broadcast in the legacy era) and `RequestContext::Log()`, which requires the request to carry a `_meta` log level — see [/en/learn/protocol-versions](/en/learn/protocol-versions).

## Common errors

| Wire behaviour | Cause | Remedy |
|---|---|---|
| Registration throws `invalid tool name: <name>` | Name is empty, longer than 128 characters, or contains characters outside `[A-Za-z0-9._-]` | Rename to `snake_case` / `dotted.name` and retry |
| Registration throws `tool already registered: <name>` | Duplicate name; the SDK never overwrites | Choose another name, or turn the variable part into a tool argument |
| Registration throws `x-mcp-header ...` | The `x-mcp-header` annotation is not statically reachable through `properties`, is not unique, or sits on a non string/integer/boolean property | Keep the annotation on scalar string/integer/boolean properties only |
| Call fails with protocol error `tool not found: <name>` (`InvalidParams`) | Client name differs from the registered name (case, separator) | Use the `name` returned by `tools/list` |
| Call returns `is_error` with `invalid arguments: ...` | Arguments violate `input_schema`; the handler never ran | See [/en/howto/validate-tool-input](/en/howto/validate-tool-input) |
| `is_error` text lacks the error code you threw | When a handler throws `McpError`, the code and `data` are dropped; only `ErrorMessage()` survives | Put the detail into the message text, or return an `is_error` result |
| `is_error` text is the generic `Error executing tool <name>` | The handler threw a non-`McpError` exception; the original `what()` goes to internal logs only (off by default) | Return an explicit `is_error` result for expected failures |
| Client receives `structuredContent` but the call still errors | `structured_content` violates `output_schema` | Tighten result construction per the danger note above |

## Next steps

- Argument constraints and the three layers of defence: [/en/howto/validate-tool-input](/en/howto/validate-tool-input)
- Registering resources and prompts: [/en/howto/serve-a-resource](/en/howto/serve-a-resource), [/en/howto/serve-a-prompt](/en/howto/serve-a-prompt)
- Long tasks and progress: [/en/howto/run-long-tasks](/en/howto/run-long-tasks), [/en/howto/report-progress](/en/howto/report-progress)
- Full signature and field tables: [/en/reference/server](/en/reference/server), [/en/reference/types](/en/reference/types)

## Source files behind this page

- `include/mcp/server/McpServer.hpp`
- `include/mcp/server/McpServerTool.hpp`
- `include/mcp/server/RequestContext.hpp`
- `include/mcp/server/ServerOptions.hpp`
- `include/mcp/McpTypes.hpp`
- `include/mcp/McpError.hpp`
- `include/mcp/Content.hpp`
- `include/mcp/JsonValue.hpp`
- `include/mcp/Implementation.hpp`
- `include/mcp/transport/InMemoryTransport.hpp`
- `src/server/McpServer.cpp` (registration validation and the `tools/call` branch)
