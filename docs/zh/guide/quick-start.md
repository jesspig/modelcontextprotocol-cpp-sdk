# 快速开始

## 构建

```bash
# 配置
cmake --preset debug

# 构建
cmake --build --preset debug

# 运行全部 216 个测试
ctest --preset debug --output-on-failure
```

预设项：`debug`、`release`。仅 Ninja 生成器。

## 最小服务器

```cpp
#include <mcp/server/McpServer.hpp>
#include <mcp/transport/StdioServerTransport.hpp>
#include <asio/io_context.hpp>

using namespace mcp;

int main() {
    asio::io_context io_ctx;
    auto transport = std::make_shared<StdioServerTransport>(io_ctx);

    ServerOptions opts;
    opts.server_info = Implementation{"MyServer", "1.0.0"};

    auto server = McpServer::Create(transport, opts, &io_ctx);

    server->RegisterTool("echo",
        ToolOptions{}.Description("Echo input text back"),
        [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
            auto text = ctx.Params().arguments
                ? ctx.Params().arguments->value("text", "")
                : "";
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
    auto transport = std::make_shared<StdioClientTransport>("./my-server");

    ClientOptions opts;
    opts.client_info = Implementation{"MyClient", "1.0.0"};

    auto client = McpClient::Create(transport, opts);

    auto tools = client->ListTools();
    for (const auto& tool : tools.tools)
        std::cout << tool.name << "\n";

    auto result = client->CallTool("echo",
        nlohmann::json{{"text", "Hello, MCP!"}});
    return 0;
}
```

## 重要注意事项

使用 `McpServer::Create` 或 `McpClient::Create` 配合传输层时，两者必须共享同一个 `asio::io_context`。始终将 io_context 作为第三个参数传入：

```cpp
auto transport = std::make_shared<StdioServerTransport>(io_ctx);
auto server = McpServer::Create(transport, opts, &io_ctx);
```

省略 io_context 参数会创建一个内部 io_context，导致传输通道上发生静默数据丢失。
