# 快速开始

## 构建

```bash
# 配置
cmake --preset debug

# 构建
cmake --build --preset debug

# 运行全部测试
ctest --preset debug --output-on-failure
```

预设项：`debug`、`release`。仅 Ninja 生成器。

## 最小服务器

```cpp
#include <mcp/server/McpServer.hpp>
#include <mcp/transport/StdioServerTransport.hpp>

using namespace mcp;

int main() {
    auto transport = std::make_unique<StdioServerTransport>();

    ServerOptions opts;
    opts.server_info = Implementation{"MyServer", "1.0.0"};

    auto server = McpServer::Create(std::move(transport), opts);

    server->RegisterTool("echo",
        ToolOptions{}.Description("Echo input text back"),
        [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
            auto& params = ctx.Params();
            std::string text;
            if (params.arguments && params.arguments->Contains("text")) {
                text = (*params.arguments)["text"].GetString();
            }
            CallToolResult result;
            result.content.push_back(TextContent{"text", text});
            return result;
        });

    server->Run();
    return 0;
}
```

## 最小客户端

```cpp
#include <mcp/client/McpClient.hpp>
#include <mcp/transport/StdioClientTransport.hpp>

using namespace mcp;

int main() {
    StdioClientTransportOptions transport_opts;
    transport_opts.command = "./my-server";
    auto transport = std::make_shared<StdioClientTransport>(transport_opts);

    ClientOptions opts;
    opts.client_info = Implementation{"MyClient", "1.0.0"};

    auto client = McpClient::Create(transport, opts);

    auto tools = client->ListTools();
    for (const auto& tool : tools.tools)
        std::cout << tool.name << "\n";

    JsonValue args((JsonValue::Object{{"text", JsonValue("Hello, MCP!")}}));
    auto result = client->CallTool("echo", args);
    return 0;
}
```


