# Weather server, section by section

`examples/WeatherServer` demonstrates a **multi-tool server with input schemas**: both tools declare `inputSchema` plus `required`, so arguments are validated by the SDK before your handler ever runs. The contrast with `EchoServer` is the point of this page — with and without a schema changes which checks you may skip and which you must not. Source: [`examples/WeatherServer/main.cpp`](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/examples/WeatherServer/main.cpp).

## What this example demonstrates

- Giving `Description` and `InputSchema` in one go through **chained setters** on `ToolOptions`.
- Building JSON Schema on the spot with `JsonValue::Object{...}` / `JsonValue::Array{...}` (`JsonValue` is the repository's own type, not nlohmann).
- Two handlers reading the same `arguments` in two styles, and the precondition each style relies on.
- `server_instructions` as usage guidance for the model (tool selection hints).

## Assembly and instructions

```cpp
ServerOptions opts;
opts.server_info = Implementation{"WeatherServer", "1.0.0"};
opts.server_instructions =
    "Get weather alerts and forecasts for US states. "
    "Call get_alerts for weather alerts, get_forecast for detailed forecasts.";
```

From `examples/WeatherServer/main.cpp:24-28`; transport setup and creation match `EchoServer` (`:22-30`). `instructions` is a signpost for the client/model telling it **when to pick which tool**; it validates nothing, so it is only useful when it is specific. The data source is the file-level constant map `kWeatherData` (`:12-19`) including a `"default"` fallback key — the example uses it to show "return something readable when nothing matches" instead of throwing.

## Declaring an input schema with chained setters

```cpp
server->RegisterTool("get_alerts",
    ToolOptions{}
        .Description("Get weather alerts for a US state")
        .InputSchema(JsonValue(JsonValue::Object{
            {"type", JsonValue("object")},
            {"properties", JsonValue(JsonValue::Object{
                {"state", JsonValue(JsonValue::Object{
                    {"type", JsonValue("string")},
                    {"description", JsonValue("Two-letter US state code")}})},
            })},
            {"required", JsonValue(JsonValue::Array{JsonValue("state")})},
        })),
    ...);
```

From `examples/WeatherServer/main.cpp:32-43`. Notes:

- `Description` and `InputSchema` both return `*this`, so they chain; the remaining `ToolOptions` fields (e.g. `read_only_hint`) are plain member assignments with no setter.
- `JsonValue::Object{...}` / `JsonValue::Array{...}` are the construction entry points, and `JsonValue("string")` builds a string node.
- The real tool name is still the first `RegisterTool` argument, `"get_alerts"`.

## What the SDK does once a schema exists

As soon as `input_schema` is declared, the SDK validates `arguments` against it **before** handing the call to your handler: a type violation or a missing required property yields an `is_error` result whose text starts with `invalid arguments: ` (`src/server/McpServer.cpp:553-559`, `:2359`, `:2507`, `:2616`).

That explains the difference between the two handlers:

```cpp
auto& params = ctx.Params();
std::string state;
if (params.arguments && params.arguments->Contains("state")) {
    state = (*params.arguments)["state"].GetString();
}
```

From `examples/WeatherServer/main.cpp:46-50`. There is **no** `IsString()` check here, and that is legitimate only because the schema declares `state` as `string` and required — the validator already rejected anything else. `EchoServer` has no schema, so the same line would throw on an unexpected argument (`GetString` on a non-string node is misuse).

## Two handlers, two ways to read arguments

`get_forecast` uses the fully defensive read (`examples/WeatherServer/main.cpp:76-85`):

```cpp
std::string state;
double latitude = 0, longitude = 0;
if (params.arguments) {
    if (auto* v = params.arguments->Find("state"); v && v->IsString())
        state = v->GetString();
    if (auto* v = params.arguments->Find("latitude"); v && v->IsNumber())
        latitude = v->IsDouble() ? v->GetDouble() : static_cast<double>(v->GetInt());
    if (auto* v = params.arguments->Find("longitude"); v && v->IsNumber())
        longitude = v->IsDouble() ? v->GetDouble() : static_cast<double>(v->GetInt());
}
```

Two details deserve separate mention:

- `Find` returns `JsonValue*` (`nullptr` when absent), and `if (auto* v = ...; v && v->IsString())` is the repository's idiomatic guard.
- **Integers and doubles are two representations on the same node**: after `IsNumber()` you still need `IsDouble()` to decide between `GetDouble()` and `GetInt()`. Both `40` and `40.5` pass `"type": "number"` validation, so skipping this branch silently misreads integral input.

Result assembly (`:86-91`) interpolates the coordinates straight into the text; `get_alerts` looks up `kWeatherData.find(state)` and falls back to `"default"` (`:52-55`). Both return only `content` — neither declares `output_schema`.

## Running and observing

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMCP_BUILD_EXAMPLES=ON
cmake --build build/debug --target WeatherServer
build/debug/examples/WeatherServer/WeatherServer
```

Measured output (Windows, clang-cl):

```
WeatherServer starting on stdio...
```

The banner goes to stderr (`examples/WeatherServer/main.cpp:94`) and then `server->Run()` blocks (`:95`). To see validation immediately, call `get_alerts` from any client and **omit `state` on purpose**: you should get `is_error` with text starting `invalid arguments: `, not a call into your handler.

## Mapping to the how-to pages

| What you want to do | Go to |
|---|---|
| The full registration surface (titles, annotations, icons) | [/en/howto/register-a-tool](/en/howto/register-a-tool) |
| Input validation and failure returns | [/en/howto/validate-tool-input](/en/howto/validate-tool-input) |
| Structured output (`outputSchema`) | [/en/howto/register-a-tool](/en/howto/register-a-tool) |
| How errors reach the model | [/en/howto/handle-errors](/en/howto/handle-errors) |

## Source behind this page

- `examples/WeatherServer/main.cpp`
- `examples/WeatherServer/CMakeLists.txt`
- `include/mcp/JsonValue.hpp` (`Object`/`Array` construction, `Find`, `IsString`, `IsNumber`, `IsDouble`, `GetInt`, `GetDouble`)
- `include/mcp/McpTypes.hpp` (`ToolOptions`, `CallToolRequestParams`)
- `src/server/McpServer.cpp` (argument validation and the `invalid arguments:` prefix)
