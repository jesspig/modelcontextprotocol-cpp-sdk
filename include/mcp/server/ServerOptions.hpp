#pragma once

#include <mcp/Export.hpp>

#include <mcp/Implementation.hpp>
#include <mcp/Capabilities.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/SpanHooks.hpp>
#include <mcp/ext/ServerCard.hpp>

#include <mcp/server/McpTaskStore.hpp>
#include <mcp/protocol/MessageFilter.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

    std::optional<std::size_t> max_tool_input_elements;

    std::optional<std::vector<std::string>> supported_protocol_versions;
    bool subscriptions_enabled{true};
    std::function<std::optional<CacheHint>(std::string_view method)> cache_hint_resolver;

    std::shared_ptr<class IMcpTaskStore> task_store;

    bool enable_tasks_extension{false};
    bool enable_skills_extension{false};
    bool enable_apps_extension{false};
    bool enable_server_card{false};
    std::optional<ext::ServerCard> server_card;
    bool enable_etag_cache{false};
    bool enable_progressive_discovery{false};
    bool enable_file_input{false};
    bool enable_triggers_events{false};

    [[deprecated("set enable_tasks_extension=true explicitly")]] bool TasksImplicitlyEnabled() const {
        return enable_tasks_extension || task_store != nullptr;
    }
    [[deprecated("set enable_skills_extension=true explicitly")]] bool SkillsImplicitlyEnabled() const {
        return enable_skills_extension;
    }
};

} // namespace mcp
