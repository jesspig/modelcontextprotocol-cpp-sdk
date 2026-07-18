# Echo 服务器示例

一个极简的 MCP 服务器，演示工具、资源和提示词。

源代码：[`examples/EchoServer/`](https://github.com/modelcontextprotocol/cpp-sdk/tree/main/examples/EchoServer)

## 特性

- **工具**：`echo` — 回显输入文本
- **资源**：`echo://static` — 返回存储的消息
- **提示词**：`capitalize` — 将输入文本转换为大写

## 运行

```bash
cmake --preset debug
cmake --build --preset debug
build/debug/examples/EchoServer/EchoServer
```

## 关键代码

```cpp
asio::io_context io_ctx;
auto transport = std::make_shared<StdioServerTransport>(io_ctx);
ServerOptions opts;
opts.server_info = Implementation{"EchoServer", "1.0.0"};

auto server = McpServer::Create(transport, opts, &io_ctx);

// 工具
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
```
