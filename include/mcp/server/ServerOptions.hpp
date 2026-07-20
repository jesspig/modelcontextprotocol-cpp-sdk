#pragma once

#include <mcp/Export.hpp>

#include <mcp/Implementation.hpp>
#include <mcp/Capabilities.hpp>
#include <mcp/McpTypes.hpp>

#include <mcp/server/McpTaskStore.hpp>
#include <mcp/protocol/MessageFilter.hpp>

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

    // Event callbacks (high-level shorthand)
    std::function<void(std::string_view method)> on_method_called;
    std::function<void(const Implementation& client_info)> on_client_connected;
    std::function<void()> on_initialized;
    std::function<void(std::string_view error)> on_protocol_error;

    // Full JSON-RPC message callbacks (beyond just method name / error message)
    std::function<void(std::string_view method, const JsonRpcRequest&)> on_request;
    std::function<void(const JsonRpcResponse&)> on_response;
    std::function<void(const JsonRpcErrorResponse&)> on_error;
    std::function<void(const JsonRpcNotification&)> on_notification;

    // Message filter pipelines for interception (auth, audit, rate-limiting, etc.)
    std::shared_ptr<FilterPipeline> incoming_filters;
    std::shared_ptr<FilterPipeline> outgoing_filters;

    // Transport-level events
    std::function<void()> on_transport_close;
    std::function<void(std::string_view)> on_transport_error;

    // Task store (enables tasks/get, tasks/update, tasks/cancel)
    std::shared_ptr<class IMcpTaskStore> task_store;
};

} // namespace mcp
