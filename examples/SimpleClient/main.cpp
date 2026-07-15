// SimpleClient — 对应 C# QuickstartClient
// 演示如何创建客户端、列出工具、调用工具
// 使用 InMemoryTransport 连接嵌入式 EchoServer

#include <mcp/client/McpClient.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <iostream>

using namespace mcp;
using Ctx = RequestContext<CallToolRequestParams>;

int main() {
    // 创建内存传输对
    auto transport_pair = InMemoryTransport::CreatePair();

    // ── 服务端 ──
    ServerOptions srv_opts;
    srv_opts.server_info = Implementation{"EchoServer", "1.0.0"};
    auto server = McpServer::Create(
        std::move(transport_pair.server), srv_opts);

    server->RegisterTool("echo",
        ToolOptions{}.Description("Echo input"),
        std::function<CallToolResult(const Ctx&)>(
            [](const Ctx& ctx) -> CallToolResult {
                auto text = ctx.Params().arguments
                    ? ctx.Params().arguments->value("text", "")
                    : "";
                CallToolResult r;
                r.content.push_back(TextContent{"text", text});
                return r;
            }));

    // 在后台线程运行服务器
    std::thread server_thread([&server]() { server->Run(); });
    server_thread.detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // ── 客户端 ──
    ClientOptions cl_opts;
    cl_opts.client_info = Implementation{"SimpleClient", "1.0.0"};
    cl_opts.connect_mode = ConnectMode::Pin;

    auto client = McpClient::Create(
        std::move(transport_pair.client), cl_opts);

    // 列出工具
    std::cout << "Server: " << client->GetServerInfo().name
              << " v" << client->GetServerInfo().version << std::endl;
    std::cout << "Protocol: " << client->GetNegotiatedProtocolVersion() << std::endl;

    auto tools = client->ListTools();
    std::cout << "Available tools:" << std::endl;
    for (const auto& t : tools.tools) {
        std::cout << "  - " << t.name
                  << (t.description ? ": " + *t.description : "")
                  << std::endl;
    }

    // 调用 echo 工具
    std::cout << "\nCalling echo tool with 'Hello, MCP!'..." << std::endl;
    auto result = client->CallTool("echo", nlohmann::json{{"text", "Hello, MCP!"}});
    for (const auto& content : result.content) {
        if (auto* text = std::get_if<TextContent>(&content)) {
            std::cout << "Response: " << text->text << std::endl;
        }
    }

    client->Close();
    server->Close();

    std::cout << "\nDone!" << std::endl;
    return 0;
}
