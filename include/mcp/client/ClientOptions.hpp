#pragma once
// ClientOptions.hpp
// Client connection options and configuration
#include <mcp/Export.hpp>

#include <mcp/Capabilities.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpTypes.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

// ── Connect mode ──
enum class ConnectMode {
    Auto,      // probe server/discover then fallback initialize
    Legacy,    // force initialize handshake
    Pin,       // pin to specific protocol version
};

// ── ClientOptions (对应 C# McpClientOptions) ──
struct MCP_API ClientOptions {
    // Client identity
    Implementation client_info{"mcp-cpp-client", "0.1.0"};
    std::optional<ClientCapabilities> capabilities;

    // Connection mode
    ConnectMode connect_mode{ConnectMode::Auto};
    std::optional<std::string> pin_protocol_version;

    // Timeouts
    std::chrono::seconds initialization_timeout{60};
    std::chrono::seconds discover_probe_timeout{5};

    // MRTR (InputRequired) config
    struct InputRequiredConfig {
        bool auto_fulfill{true};
        int max_rounds{8};
        std::chrono::seconds round_timeout{600};

        // Hard budget for the whole MRTR flow across all rounds. Zero
        // disables the cap (round_timeout applies per round).
        std::chrono::seconds max_total_timeout{0};
    };
    std::optional<InputRequiredConfig> input_required_config;

    // Extensions declaration map
    std::optional<JsonValue> extensions;

};

} // namespace mcp
