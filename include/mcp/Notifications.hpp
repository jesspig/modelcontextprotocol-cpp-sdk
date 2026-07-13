#pragma once

#include <string_view>

namespace mcp::notifications {

// ── Lifecycle ──
inline constexpr std::string_view Initialized = "notifications/initialized";
inline constexpr std::string_view Cancelled = "notifications/cancelled";
inline constexpr std::string_view Progress = "notifications/progress";

// ── Resources ──
inline constexpr std::string_view ResourcesListChanged = "notifications/resources/list_changed";
inline constexpr std::string_view ResourcesUpdated = "notifications/resources/updated";

// ── Tools ──
inline constexpr std::string_view ToolsListChanged = "notifications/tools/list_changed";

// ── Prompts ──
inline constexpr std::string_view PromptsListChanged = "notifications/prompts/list_changed";

// ── Logging [deprecated] ──
inline constexpr std::string_view LoggingMessage = "notifications/message";

// ── Roots [deprecated] ──
inline constexpr std::string_view RootsListChanged = "notifications/roots/list_changed";

// ── Subscriptions ──
inline constexpr std::string_view SubscriptionsAcknowledged = "notifications/subscriptions/acknowledged";

// ── Elicitation ──
inline constexpr std::string_view ElicitationComplete = "notifications/elicitation/complete";

} // namespace mcp::notifications
