// WeatherServer — 对应 C# QuickstartWeatherServer
// 演示工具定义、参数描述、多工具注册
// 使用 StdioServerTransport

#include <mcp/server/McpServer.hpp>
#include <mcp/transport/StdioServerTransport.hpp>

#include <asio/io_context.hpp>

#include <iostream>
#include <map>
#include <memory>
#include <string>

using namespace mcp;
using Ctx = RequestContext<CallToolRequestParams>;

// 模拟天气数据
static const std::map<std::string, std::string> kWeatherData = {
    {"CA", "California: Sunny, 72°F"},
    {"NY", "New York: Cloudy, 65°F"},
    {"TX", "Texas: Partly Cloudy, 85°F"},
    {"FL", "Florida: Thunderstorms, 80°F"},
    {"WA", "Washington: Rainy, 55°F"},
    {"default", "Unknown location — no data available"},
};

int main() {
    asio::io_context io_ctx;
    auto transport = std::make_unique<StdioServerTransport>(io_ctx);

    ServerOptions opts;
    opts.server_info = Implementation{"WeatherServer", "1.0.0"};
    opts.server_instructions =
        "Get weather alerts and forecasts for US states. "
        "Call get_alerts for weather alerts, get_forecast for detailed forecasts.";

    auto server = McpServer::Create(std::move(transport), opts, &io_ctx);

    // 工具: 获取天气警报
    server->RegisterTool("get_alerts",
        ToolOptions{}.Description("Get weather alerts for a US state"),
        std::function<CallToolResult(const Ctx&)>(
            [](const Ctx& ctx) -> CallToolResult {
                auto& params = ctx.Params();
                std::string state;
                if (params.arguments && params.arguments->contains("state")) {
                    state = (*params.arguments)["state"].get<std::string>();
                }
                CallToolResult result;
                auto it = kWeatherData.find(state);
                std::string alert = (it != kWeatherData.end())
                    ? it->second : kWeatherData.at("default");
                result.content.push_back(TextContent{"text", "Alerts: " + alert});
                return result;
            }));

    // 工具: 获取天气预报
    server->RegisterTool("get_forecast",
        ToolOptions{}.Description("Get weather forecast for a location"),
        std::function<CallToolResult(const Ctx&)>(
            [](const Ctx& ctx) -> CallToolResult {
                auto& params = ctx.Params();
                std::string state;
                double latitude = 0, longitude = 0;
                if (params.arguments) {
                    state = params.arguments->value("state", "");
                    latitude = params.arguments->value("latitude", 0.0);
                    longitude = params.arguments->value("longitude", 0.0);
                }
                CallToolResult result;
                std::string forecast = "Forecast for " + state +
                    " (" + std::to_string(latitude) + ", " +
                    std::to_string(longitude) + "): Mild temperatures";
                result.content.push_back(TextContent{"text", forecast});
                return result;
            }));

    std::cerr << "WeatherServer starting on stdio..." << std::endl;
    server->Run();
    return 0;
}
