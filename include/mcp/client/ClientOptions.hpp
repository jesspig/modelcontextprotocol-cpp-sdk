#pragma once
#include <mcp/Export.hpp>

#include <mcp/Capabilities.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/McpVersion.hpp>
#include <mcp/SpanHooks.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

enum class ConnectMode {
    Auto,
    Legacy,
    Pin,
};

struct MCP_API ClientOptions {
    Implementation client_info{"mcp-cpp-client", std::string(kSdkVersion)};
    std::optional<ClientCapabilities> capabilities;

    ConnectMode connect_mode{ConnectMode::Auto};
    std::optional<std::string> pin_protocol_version;

    std::chrono::seconds initialization_timeout{60};
    std::chrono::seconds discover_probe_timeout{5};

    std::chrono::seconds max_total_timeout{0};

    struct InputRequiredConfig {
        bool auto_fulfill{true};
        int max_rounds{10};
        std::chrono::seconds round_timeout{600};

        std::chrono::seconds max_total_timeout{0};
    };
    std::optional<InputRequiredConfig> input_required_config;

    std::optional<JsonValue> extensions;

    bool reinit_on_expired_session{true};

    SpanHandler span_handler;

};

} // namespace mcp
