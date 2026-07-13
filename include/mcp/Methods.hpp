#pragma once

#include <string_view>

namespace mcp::methods {

// ── Lifecycle ──
inline constexpr std::string_view Initialize = "initialize";
inline constexpr std::string_view Ping = "ping";

// ── Discovery ──
inline constexpr std::string_view ServerDiscover = "server/discover";

// ── Tools ──
inline constexpr std::string_view ToolsList = "tools/list";
inline constexpr std::string_view ToolsCall = "tools/call";

// ── Resources ──
inline constexpr std::string_view ResourcesList = "resources/list";
inline constexpr std::string_view ResourcesTemplatesList = "resources/templates/list";
inline constexpr std::string_view ResourcesRead = "resources/read";
inline constexpr std::string_view ResourcesSubscribe = "resources/subscribe";
inline constexpr std::string_view ResourcesUnsubscribe = "resources/unsubscribe";

// ── Prompts ──
inline constexpr std::string_view PromptsList = "prompts/list";
inline constexpr std::string_view PromptsGet = "prompts/get";

// ── Subscriptions (2026-07-28) ──
inline constexpr std::string_view SubscriptionsListen = "subscriptions/listen";

// ── Completion ──
inline constexpr std::string_view CompletionComplete = "completion/complete";

// ── Logging [deprecated] ──
inline constexpr std::string_view LoggingSetLevel = "logging/setLevel";

// ── Sampling [deprecated] ──
inline constexpr std::string_view SamplingCreateMessage = "sampling/createMessage";

// ── Roots [deprecated] ──
inline constexpr std::string_view RootsList = "roots/list";

// ── Elicitation ──
inline constexpr std::string_view ElicitationCreate = "elicitation/create";

} // namespace mcp::methods
