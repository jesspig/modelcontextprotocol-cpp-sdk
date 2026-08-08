// SimpleClient — MCP client example
// Demonstrates creating a client, listing tools, and calling a tool
// Uses InMemoryTransport to connect to an embedded EchoServer

#include <mcp/client/McpClient.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <iostream>
#include <thread>

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
                std::string text;
                if (ctx.Params().arguments) {
                    if (auto* v = ctx.Params().arguments->Find("text"); v && v->IsString())
                        text = v->GetString();
                }
                CallToolResult r;
                r.content.push_back(TextContent{"text", text});
                return r;
            }));

    // 在后台线程运行服务器
    std::thread server_thread([&server]() { server->Run(); });
    server_thread.detach();

    // ── 客户端 ──
    // Create 阻塞直到协商完成,无需手动等待服务器就绪
    ClientOptions cl_opts;
    cl_opts.client_info = Implementation{"SimpleClient", "1.0.0"};
    cl_opts.connect_mode = ConnectMode::Auto;

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
    JsonValue args((JsonValue::Object{{"text", JsonValue("Hello, MCP!")}}));
    auto result = client->CallTool("echo", args);
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
