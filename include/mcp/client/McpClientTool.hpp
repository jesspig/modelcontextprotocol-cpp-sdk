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

// ── Client-side resource representation (对应 C# McpClientResource) ──
struct McpClientResource {
    Resource protocol_resource;

    static McpClientResource FromProtocol(const Resource& resource) {
        McpClientResource cr;
        cr.protocol_resource = resource;
        return cr;
    }
};

// ── Client-side resource template (对应 C# McpClientResourceTemplate) ──
struct McpClientResourceTemplate {
    ResourceTemplate protocol_template;

    static McpClientResourceTemplate FromProtocol(const ResourceTemplate& rt) {
        McpClientResourceTemplate crt;
        crt.protocol_template = rt;
        return crt;
    }
};

// ── Client-side prompt (对应 C# McpClientPrompt) ──
struct McpClientPrompt {
    Prompt protocol_prompt;

    static McpClientPrompt FromProtocol(const Prompt& prompt) {
        McpClientPrompt cp;
        cp.protocol_prompt = prompt;
        return cp;
    }
};

} // namespace mcp
