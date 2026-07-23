# Tools

Tools are functions that an LLM can call to perform actions. They are the primary mechanism for servers to expose executable functionality.

## Registering a Tool

```cpp
server->RegisterTool("get_weather",
    ToolOptions{}.Description("Get current weather for a location"),
    [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
        std::string location;
        if (ctx.Params().arguments) {
            auto* v = ctx.Params().arguments->Find("location");
            if (v) location = v->GetString();
        }

        CallToolResult result;
        result.content.push_back(
            TextContent{"text", "The weather in " + location + " is sunny."});
        return result;
    });
```

## Tool with Annotations

```cpp
ToolOptions opts;
opts.title = "Delete File";
opts.description = "Permanently delete a file by path";
opts.destructive = true;   // marks as destructive_hint
opts.idempotent = false;

server->RegisterTool("delete_file", opts,
    [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
        // ...
    });
```

Available annotations via `ToolOptions`:

| Field | Type | Description |
|-------|------|-------------|
| `destructive` | `optional<bool>` | Marks the tool as destructive |
| `idempotent` | `optional<bool>` | Calling multiple times has same effect as once |
| `read_only_hint` | `optional<bool>` | Tool does not modify state |
| `open_world_hint` | `optional<bool>` | Tool may interact with external systems |

## Structured Content

Tools can return structured JSON data instead of text:

```cpp
CallToolResult result;
result.content.push_back(TextContent{"text", "Processed."});
result.structured_content = JsonValue::Parse(R"({
    "records_updated": 42,
    "duration_ms": 153
})");
```

The `structured_content` field is a 2026-07-28 protocol feature. Set `use_structured_content = true` on `ToolOptions` to opt in, which sets `output_schema` to `{"type": "object"}`.

## Input Schema

Set the tool's `input_schema` via `ToolOptions::InputSchema()` or directly:

```cpp
auto schema = JsonValue::Parse(R"({
    "type": "object",
    "properties": {
        "location": {"type": "string"},
        "units": {"type": "string", "enum": ["celsius", "fahrenheit"]}
    },
    "required": ["location"]
})");

server->RegisterTool("get_weather",
    ToolOptions{}.InputSchema(std::move(schema)),
    [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
        // ...
    });
```
