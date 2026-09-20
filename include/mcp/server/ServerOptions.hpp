#pragma once

#include <mcp/Export.hpp>

#include <mcp/Implementation.hpp>
#include <mcp/Capabilities.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/SpanHooks.hpp>

#include <mcp/server/McpTaskStore.hpp>
#include <mcp/protocol/MessageFilter.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

struct CacheableMethod;
struct CacheHint;

struct MCP_API ServerOptions {
    std::optional<Implementation> server_info;
    std::optional<std::string> protocol_version;
    std::optional<std::string> server_instructions;

    std::chrono::seconds initialization_timeout{60};


    std::optional<std::map<std::string, CacheHint, std::less<>>> cache_hints;

    std::function<bool(std::string_view)> request_state_verifier;

    std::optional<std::string> request_state_key;

    std::chrono::seconds request_state_ttl{0};

    struct InputRequiredConfig {
        int max_rounds{10};
        std::chrono::seconds round_timeout{600};
        bool legacy_shim{true};
    };
    std::optional<InputRequiredConfig> input_required_config;

    bool declare_logging{false};
    bool declare_completions{false};

    std::function<void(std::string_view method)> on_method_called;
    std::function<void(const Implementation& client_info)> on_client_connected;
    std::function<void()> on_initialized;
    std::function<void(std::string_view error)> on_protocol_error;

    std::function<void(std::string_view method, const JsonRpcRequest&)> on_request;
    std::function<void(const JsonRpcResponse&)> on_response;
    std::function<void(const JsonRpcErrorResponse&)> on_error;
    std::function<void(const JsonRpcNotification&)> on_notification;

    SpanHandler span_handler;

    std::shared_ptr<FilterPipeline> incoming_filters;
    std::shared_ptr<FilterPipeline> outgoing_filters;

    std::function<void()> on_transport_close;
    std::function<void(std::string_view)> on_transport_error;

    std::shared_ptr<class IMcpTaskStore> task_store;
};

} // namespace mcp
