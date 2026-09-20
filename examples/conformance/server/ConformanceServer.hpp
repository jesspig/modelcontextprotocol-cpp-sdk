#pragma once

#include <mcp/McpTypes.hpp>
#include <mcp/server/McpServer.hpp>

#include <string>

namespace mcp::conformance {

inline constexpr const char* kTestImageBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==";

inline constexpr const char* kTestAudioBase64 =
    "UklGRiYAAABXQVZFZm10IBAAAAABAAEAQB8AAAB9AAACABAAZGF0YQIAAAA=";

inline constexpr const char* kWatchedResourceContent = "Watched resource content";

using ToolContext = RequestContext<CallToolRequestParams>;

inline CallToolResult MakeTextResult(std::string text)
{
    CallToolResult result;
    result.content.push_back(TextContent{"text", std::move(text)});
    return result;
}

void RegisterContentTools(McpServer& server);
void RegisterInteractionTools(McpServer& server);
void RegisterMrtrTools(McpServer& server);
void RegisterConformanceResources(McpServer& server);
void RegisterConformancePrompts(McpServer& server);

}
