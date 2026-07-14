// EchoServer — 对应 Python examples/echo.py / TypeScript examples/tools
// 使用 StdioServerTransport，注册一个 echo 工具
// 通过 stdio 行分隔 JSON 与 MCP 客户端通信

#include <mcp/server/McpServer.hpp>
#include <mcp/transport/StdioServerTransport.hpp>

#include <asio/io_context.hpp>

#include <iostream>
#include <memory>
#include <string>

using namespace mcp;
using Ctx = RequestContext<CallToolRequestParams>;

int main() {
    // 创建 stdio 传输
    asio::io_context io_ctx;
    auto transport = std::make_unique<StdioServerTransport>(io_ctx);

    // 创建服务器
    ServerOptions opts;
    opts.server_info = Implementation{"EchoServer", "1.0.0"};
    opts.server_instructions = "An echo server — sends back what you send.";

    auto server = McpServer::Create(std::move(transport), opts, &io_ctx);

    // 注册 echo 工具
    server->RegisterTool("echo",
        ToolOptions{}.Description("Echo the input text back"),
        std::function<CallToolResult(const Ctx&)>(
            [](const Ctx& ctx) -> CallToolResult {
                auto& params = ctx.Params();
                std::string text;
                if (params.arguments && params.arguments->contains("text")) {
                    text = (*params.arguments)["text"].get<std::string>();
                }
                CallToolResult result;
                result.content.push_back(TextContent{"text", text});
                return result;
            }));

    // 注册静态资源
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

    // 注册模板资源
    server->RegisterResourceTemplate("echo-template",
        "echo://{text}",
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

    // 注册 prompt
    server->RegisterPrompt("capitalize",
        PromptOptions{}.Description("Capitalize the input text"),
        [](const std::string& name,
           const std::optional<nlohmann::json>& args) -> GetPromptResult {
            std::string text;
            if (args && args->contains("text")) {
                text = (*args)["text"].get<std::string>();
            }
            // Capitalize
            for (auto& c : text) c = static_cast<char>(std::toupper(c));
            GetPromptResult r;
            PromptMessage pm;
            pm.role = "user";
            pm.content = TextContent{"text", text};
            r.messages = {std::move(pm)};
            return r;
        });

    // 启动服务器（阻塞，等待 stdio 输入）
    std::cerr << "EchoServer starting on stdio..." << std::endl;
    server->Run();

    return 0;
}
