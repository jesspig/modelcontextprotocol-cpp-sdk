# 扩展

扩展（SEP-2133）允许可插拔模块贡献工具、资源以及拦截协议调用。

## 定义扩展

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
                result.content.push_back(TextContent{"text", "来自扩展的问候"});
                return result;
            },
            ToolOptions{}.Description("来自扩展的工具"));
        return {tool};
    }

    bool InterceptToolCall(
        const RequestContext<CallToolRequestParams>& ctx,
        CallToolResult& result) override {
        // 记录或修改工具调用
        return true; // 继续处理
    }
};

// 在服务端上注册
server->RegisterExtension(std::make_shared<MyExtension>());
```

## 扩展接口

| 方法 | 用途 |
|--------|---------|
| `Identifier()` | 唯一反向 DNS 名称（例如 `"io.modelcontextprotocol/tasks"`） |
| `Tools()` | 贡献的工具实例 |
| `Methods()` | 自定义 JSON-RPC 方法处理器 |
| `InterceptToolCall()` | 工具调用的前置/后置处理 |
