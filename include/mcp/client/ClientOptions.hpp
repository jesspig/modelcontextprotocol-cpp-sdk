#pragma once

#include <mcp/Export.hpp>

#include <mcp/Capabilities.hpp>
#include <mcp/Implementation.hpp>
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

    // Protocol support
    std::vector<std::string> supported_protocol_versions{
        "2025-11-25", "2026-07-28"
    };

    // MRTR (InputRequired) config
    struct InputRequiredConfig {
        bool auto_fulfill{true};
        int max_rounds{8};
        std::chrono::seconds round_timeout{600};
    };
    std::optional<InputRequiredConfig> input_required_config;

    // Response caching
    struct CacheConfig {
        int64_t default_ttl_ms{30000};
        bool enabled{true};
    };
    std::optional<CacheConfig> cache_config;

    // Extensions declaration map
    std::optional<nlohmann::json> extensions;

    // Other
    bool enforce_strict_capabilities{false};
    int list_max_pages{64};
};

} // namespace mcp
