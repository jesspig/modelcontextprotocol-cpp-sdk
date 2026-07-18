# 工具

工具是 LLM 可以调用的函数，用于执行操作。它们是服务端暴露可执行功能的主要机制。

## 注册工具

```cpp
server->RegisterTool("get_weather",
    ToolOptions{}.Description("获取某个位置的当前天气"),
    [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
        auto location = ctx.Params().arguments
            ? ctx.Params().arguments->value("location", "")
            : "";

        CallToolResult result;
        result.content.push_back(
            TextContent{"text", location + "的天气是晴天。"});
        return result;
    });
```

## 带注解的工具

```cpp
server->RegisterTool("delete_file",
    ToolOptions{}
        .Title("删除文件")
        .Description("按路径永久删除文件")
        .Destructive(true)  // 标记为 destructive_hint
        .Idempotent(false),
    [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
        // ...
    });
```

## 结构化内容

工具可以返回结构化 JSON 数据而非文本：

```cpp
CallToolResult result;
result.content.push_back(TextContent{"text", "已处理。"});
result.structured_content = nlohmann::json{
    {"records_updated", 42},
    {"duration_ms", 153}
};
```

`structured_content` 字段是 2026-07-28 协议特性。设置 `ToolOptions{}.use_structured_content = true` 以选择启用。

## 输入模式

工具的 `input_schema` 是一个原始的 `nlohmann::json` 字段，支持完整的 JSON Schema 2020-12：

```cpp
nlohmann::json schema;
schema["type"] = "object";
schema["properties"] = nlohmann::json::object();
schema["properties"]["location"] = {{"type", "string"}};
schema["properties"]["units"] = {{"type", "string"}, {"enum", {"celsius", "fahrenheit"}}};
schema["required"] = {"location"};
```
