#pragma once
#include <mcp/Implementation.hpp>
#include <mcp/Capabilities.hpp>
#include <mcp/Meta.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/JsonValue.hpp>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <memory>

namespace mcp {

struct IncomingRequestMeta {
    std::string protocol_version;
    std::optional<Implementation> client_info;
    std::optional<ClientCapabilities> client_capabilities;
    std::optional<LoggingLevel> log_level;
    std::optional<ProgressToken> progress_token;
    std::optional<std::string> subscription_id;
    std::optional<std::string> traceparent;
    std::optional<std::string> tracestate;
    std::optional<std::string> baggage;
};

struct PendingRequest {
    std::function<void(JsonValue)> callback;
    std::chrono::steady_clock::time_point deadline;
    std::optional<ProgressToken> progress_token;
};

struct Subscription {
    std::string id;
    SubscriptionFilter granted;
};

} // namespace mcp
