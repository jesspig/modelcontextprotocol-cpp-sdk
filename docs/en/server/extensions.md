# Extensions

Extensions (SEP-2133) allow pluggable modules to contribute tools, resources, and intercept protocol calls.

## Defining an Extension

```cpp
class MyExtension : public Extension {
public:
    std::string Identifier() const override {
        return "com.example/my-extension";
    }

    std::vector<std::shared_ptr<McpServerTool>> Tools() override {
        auto tool = McpServerTool::Create("my_tool",
            [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
                CallToolResult result;
                result.content.push_back(TextContent{"text", "Hello from extension"});
                return result;
            },
            ToolOptions{}.Description("A tool from an extension"));
        return {tool};
    }

    bool InterceptToolCall(
        const RequestContext<CallToolRequestParams>& ctx,
        CallToolResult& result) override {
        // Log or modify tool calls
        return true; // continue processing
    }
};

// Register on the server
server->RegisterExtension(std::make_shared<MyExtension>());
```

## Extension Interface

| Method | Purpose |
|--------|---------|
| `Identifier()` | Unique reverse-DNS name (e.g., `"io.modelcontextprotocol/tasks"`) |
| `Tools()` | Contributed tool instances |
| `Methods()` | Custom JSON-RPC method handlers |
| `InterceptToolCall()` | Pre/post-processing for tool invocations |
