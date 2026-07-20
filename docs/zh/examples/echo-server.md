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
#include <mcp/server/McpServer.hpp>
#include <mcp/transport/StdioServerTransport.hpp>
#include <asio/io_context.hpp>

using namespace mcp;
using Ctx = RequestContext<CallToolRequestParams>;

asio::io_context io_ctx;
auto transport = std::make_unique<StdioServerTransport>(io_ctx);

ServerOptions opts;
opts.server_info = Implementation{"EchoServer", "1.0.0"};
opts.server_instructions = "An echo server — sends back what you send.";

auto server = McpServer::Create(std::move(transport), opts, &io_ctx);

// 工具 — 回显输入文本
server->RegisterTool("echo",
    ToolOptions{}.Description("Echo the input text back"),
    std::function<CallToolResult(const Ctx&)>(
        [](const Ctx& ctx) -> CallToolResult {
            auto text = ctx.Params().arguments
                ? ctx.Params().arguments->value("text", "")
                : "";
            CallToolResult result;
            result.content.push_back(TextContent{"text", text});
            return result;
        }));

// 静态资源
server->RegisterResource("echo-static", "echo://static",
    ResourceOptions{}.Description("Static echo resource"),
    [](const std::string& uri) -> ReadResourceResult {
        TextResourceContents trc;
        trc.uri = uri;
        trc.text = "Echo!";
        ReadResourceResult rr;
        rr.contents = {ResourceContents{trc}};
        return rr;
    });

// 模板资源 — 回显 URI 参数
server->RegisterResourceTemplate("echo-template", "echo://{text}",
    ResourceOptions{}.Description("Echo the URI parameter"),
    [](const std::string& uri,
       const std::map<std::string, std::string>& vars) -> ReadResourceResult {
        TextResourceContents trc;
        trc.uri = uri;
        trc.text = "Echo: " + vars.at("text");
        ReadResourceResult rr;
        rr.contents = {ResourceContents{trc}};
        return rr;
    });

// 提示词 — 将输入文本转换为大写
server->RegisterPrompt("capitalize",
    PromptOptions{}.Description("Capitalize the input text"),
    [](const std::string& name,
       const std::optional<nlohmann::json>& args) -> GetPromptResult {
        std::string text;
        if (args && args->contains("text"))
            text = (*args)["text"].get<std::string>();
        for (auto& c : text) c = static_cast<char>(std::toupper(c));
        GetPromptResult r;
        r.messages = {PromptMessage{"user", TextContent{"text", text}}};
        return r;
    });

server->Run();
```
