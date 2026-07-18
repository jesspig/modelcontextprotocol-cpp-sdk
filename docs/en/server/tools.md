# Tools

Tools are functions that an LLM can call to perform actions. They are the primary mechanism for servers to expose executable functionality.

## Registering a Tool

```cpp
server->RegisterTool("get_weather",
    ToolOptions{}.Description("Get current weather for a location"),
    [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
        auto location = ctx.Params().arguments
            ? ctx.Params().arguments->value("location", "")
            : "";

        CallToolResult result;
        result.content.push_back(
            TextContent{"text", "The weather in " + location + " is sunny."});
        return result;
    });
```

## Tool with Annotations

```cpp
server->RegisterTool("delete_file",
    ToolOptions{}
        .Title("Delete File")
        .Description("Permanently delete a file by path")
        .Destructive(true)  // marks as destructive_hint
        .Idempotent(false),
    [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
        // ...
    });
```

## Structured Content

Tools can return structured JSON data instead of text:

```cpp
CallToolResult result;
result.content.push_back(TextContent{"text", "Processed."});
result.structured_content = nlohmann::json{
    {"records_updated", 42},
    {"duration_ms", 153}
};
```

The `structured_content` field is a 2026-07-28 protocol feature. Set `ToolOptions{}.use_structured_content = true` to opt in.

## Input Schema

The tool's `input_schema` is a raw `nlohmann::json` field supporting full JSON Schema 2020-12:

```cpp
nlohmann::json schema;
schema["type"] = "object";
schema["properties"] = nlohmann::json::object();
schema["properties"]["location"] = {{"type", "string"}};
schema["properties"]["units"] = {{"type", "string"}, {"enum", {"celsius", "fahrenheit"}}};
schema["required"] = {"location"};
```
