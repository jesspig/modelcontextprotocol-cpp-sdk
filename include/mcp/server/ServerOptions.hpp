#pragma once

#include <mcp/Export.hpp>

#include <mcp/Implementation.hpp>
#include <mcp/Capabilities.hpp>
#include <mcp/McpTypes.hpp>

#include <mcp/server/McpTaskStore.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

struct CacheableMethod;
struct CacheHint;

// ── ServerOptions (对应 C# McpServerOptions) ──
struct MCP_API ServerOptions {
    // Server identity
    std::optional<Implementation> server_info;
    std::optional<ServerCapabilities> capabilities;
    std::optional<std::string> protocol_version;
    std::optional<std::string> server_instructions;

    // Timeouts
    std::chrono::seconds initialization_timeout{60};

    // Handlers (low-level override, matching C# McpServerHandlers)
    // Normally auto-wired from RegisterTool/RegisterResource/RegisterPrompt

    // Caching
    std::optional<std::map<std::string, CacheHint>> cache_hints;

    // Request state security (HMAC/AEAD verification)
    std::function<bool(std::string_view)> request_state_verifier;

    // Input required (MRTR) config
    struct InputRequiredConfig {
        int max_rounds{8};
        std::chrono::seconds round_timeout{600};
        bool legacy_shim{true};
    };
    std::optional<InputRequiredConfig> input_required_config;

    // JSON Schema validation
    bool validate_tool_input{false};
    bool validate_tool_output{false};

    // Extensions declaration map (SEP-2133)
    std::optional<nlohmann::json> extensions;

    // Task store (enables tasks/get, tasks/update, tasks/cancel)
    std::shared_ptr<class IMcpTaskStore> task_store;
};

} // namespace mcp
