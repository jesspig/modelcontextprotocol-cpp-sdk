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

// ═══════════════════════════════════════════════════════════════════════
// IncomingRequestMeta — extracted _meta from incoming 2026-era requests
// ═══════════════════════════════════════════════════════════════════════
struct IncomingRequestMeta {
    std::string protocol_version;
    std::optional<Implementation> client_info;
    std::optional<ClientCapabilities> client_capabilities;
    std::optional<LoggingLevel> log_level;
    std::optional<ProgressToken> progress_token;
    std::optional<std::string> subscription_id;
};

// ═══════════════════════════════════════════════════════════════════════
// PendingRequest — used by session handler for request/response tracking
// ═══════════════════════════════════════════════════════════════════════
struct PendingRequest {
    std::function<void(JsonValue)> callback;
    std::chrono::steady_clock::time_point deadline;
};

// ═══════════════════════════════════════════════════════════════════════
// Subscription — for subscriptions/listen tracking
// ═══════════════════════════════════════════════════════════════════════
struct Subscription {
    std::string id;
    SubscriptionFilter granted;
};

} // namespace mcp
