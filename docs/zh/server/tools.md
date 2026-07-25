# 工具

工具是 LLM 可以调用的函数，用于执行操作。它们是服务端暴露可执行功能的主要机制。

## 注册工具

```cpp
server->RegisterTool("get_weather",
    ToolOptions{}.Description("获取某个位置的当前天气"),
    [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
        std::string location;
        if (ctx.Params().arguments) {
            auto* v = ctx.Params().arguments->Find("location");
            if (v) location = v->GetString();
        }

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

## McpServerTool（可复用工具）

对于可复用的工具逻辑，可以显式创建 `McpServerTool` 对象：

```cpp
auto tool = McpServerTool::Create("get_weather",
    [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
        // ...
    },
    ToolOptions{}.Description("获取当前天气"));
server->RegisterTool(tool);
```

基于 lambda 的 `RegisterTool` 重载是此方式的简写——两种形式等价。

## ToolOptions 字段

所有 `ToolOptions` 字段：

| 字段 | 类型 | 默认值 | 说明 |
|-------|------|---------|------|
| `name` | `optional<string>` | — | 覆盖工具名称 |
| `title` | `optional<string>` | — | 人类可读的标题 |
| `description` | `optional<string>` | — | 工具描述 |
| `input_schema` | `optional<JsonValue>` | `{"type":"object","properties":{}}` | 输入参数的 JSON Schema |
| `output_schema` | `optional<JsonValue>` | — | 结构化输出的 JSON Schema |
| `use_structured_content` | `bool` | `false` | 选择启用结构化输出（将 `output_schema` 设为 `{"type":"object"}`） |
| `icons` | `vector<Icon>` | — | 工具图标 |
| `meta` | `optional<JsonValue>` | — | 附加元数据 |

注解字段（传播到 `ToolAnnotations`）：

| 字段 | 类型 | 说明 |
|-------|------|------|
| `destructive` | `optional<bool>` | 标记工具为破坏性操作 |
| `idempotent` | `optional<bool>` | 多次调用效果等同于一次 |
| `read_only_hint` | `optional<bool>` | 工具不修改状态 |
| `open_world_hint` | `optional<bool>` | 工具可能和外部系统交互 |

## 结构化内容

工具可以返回结构化 JSON 数据而非文本：

```cpp
CallToolResult result;
result.content.push_back(TextContent{"text", "已处理。"});
result.structured_content = JsonValue::Parse(R"({
    "records_updated": 42,
    "duration_ms": 153
})");
```

`structured_content` 字段是 2026-07-28 协议特性。在 `ToolOptions` 上设置 `use_structured_content = true` 以选择启用（将 `output_schema` 设为 `{"type": "object"}`）。也可以直接设置 `output_schema` 以实现更精细的模式控制。

## 输入模式

通过 `ToolOptions::InputSchema()` 或直接设置工具的 `input_schema`：

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
