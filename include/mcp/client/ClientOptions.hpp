#pragma once
#include <mcp/Export.hpp>

#include <mcp/Capabilities.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/McpVersion.hpp>
#include <mcp/SpanHooks.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {

using SamplingHandler = std::function<CreateMessageResult(
    const CreateMessageRequestParams&)>;

using RootsHandler = std::function<ListRootsResult(
    const ListRootsRequestParams&)>;

using ElicitationHandler = std::function<ElicitResult(
    const ElicitRequestParams&)>;

using UrlElicitationHandler = std::function<ElicitResult(
    const ElicitRequestParams&)>;

enum class ConnectMode {
    Auto,
    Legacy,
    Pin,
};

struct MCP_API ClientOptions {
    Implementation client_info{"mcp-cpp-client", std::string(kSdkVersion)};
    std::optional<ClientCapabilities> capabilities;

    std::optional<ElicitationHandler> elicitation_handler;
    std::optional<UrlElicitationHandler> url_elicitation_handler;
    std::optional<SamplingHandler> sampling_handler;
    std::optional<RootsHandler> roots_handler;

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
    std::optional<InputRequiredConfig> input_required_config =
        InputRequiredConfig{};

    std::optional<JsonValue> extensions;

    bool reinit_on_expired_session{true};

    SpanHandler span_handler;

    bool enable_tasks_extension{false};
    bool enable_skills_extension{false};
    bool enable_apps_extension{false};
    bool enable_progressive_discovery{false};

    bool enable_etag_cache{true};

    std::function<std::string()> cache_auth_context;

    std::function<void()> on_transport_close;
    std::function<void(std::string_view)> on_transport_error;

};

MCP_API std::optional<ClientCapabilities> DeriveClientCapabilities(
    const ClientOptions& options);

} // namespace mcp
