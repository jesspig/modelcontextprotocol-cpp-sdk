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
ToolOptions opts;
opts.title = "删除文件";
opts.description = "按路径永久删除文件";
opts.destructive = true;   // 标记为 destructive_hint
opts.idempotent = false;

server->RegisterTool("delete_file", opts,
    [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
        // ...
    });
```

通过 `ToolOptions` 可用的注解：

| 字段 | 类型 | 描述 |
|-------|------|-------------|
| `destructive` | `optional<bool>` | 标记工具为破坏性操作 |
| `idempotent` | `optional<bool>` | 多次调用效果等同于一次 |
| `read_only_hint` | `optional<bool>` | 工具不修改状态 |
| `open_world_hint` | `optional<bool>` | 工具可能和外部系统交互 |

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

`structured_content` 字段是 2026-07-28 协议特性。在 `ToolOptions` 上设置 `use_structured_content = true` 以选择启用，这会设置 `output_schema` 为 `{"type": "object"}`。

## 输入模式

通过 `ToolOptions::InputSchema()` 或直接设置工具的 `input_schema`：

```cpp
nlohmann::json schema;
schema["type"] = "object";
schema["properties"] = nlohmann::json::object();
schema["properties"]["location"] = {{"type", "string"}};
schema["properties"]["units"] = {{"type", "string"}, {"enum", {"celsius", "fahrenheit"}}};
schema["required"] = {"location"};

server->RegisterTool("get_weather",
    ToolOptions{}.InputSchema(std::move(schema)),
    [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
        // ...
    });
```
