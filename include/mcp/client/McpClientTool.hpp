#pragma once

#include <mcp/McpTypes.hpp>

namespace mcp {

// ── Client-side tool representation (对应 C# McpClientTool) ──
// Wraps the protocol Tool with client-side metadata.
struct McpClientTool {
    Tool protocol_tool;

    // Optional: cached schema for validation
    std::optional<nlohmann::json> resolved_input_schema;
    std::optional<nlohmann::json> resolved_output_schema;

    // Factory
    static McpClientTool FromProtocol(const Tool& tool) {
        McpClientTool ct;
        ct.protocol_tool = tool;
        return ct;
    }
};



} // namespace mcp
