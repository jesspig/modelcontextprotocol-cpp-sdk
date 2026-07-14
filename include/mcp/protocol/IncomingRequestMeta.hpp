#pragma once
#include <mcp/Implementation.hpp>
#include <mcp/Capabilities.hpp>
#include <mcp/Meta.hpp>
#include <asio/steady_timer.hpp>
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
    std::function<void(nlohmann::json)> callback;
    std::shared_ptr<asio::steady_timer> timer;
};

// ═══════════════════════════════════════════════════════════════════════
// Subscription — for subscriptions/listen tracking
// ═══════════════════════════════════════════════════════════════════════
struct Subscription {
    std::string id;
    std::optional<std::string> tools_filter;
    std::optional<std::string> resources_filter;
    std::optional<std::string> prompts_filter;
};

} // namespace mcp
