#pragma once

#include <string_view>

namespace mcp {
namespace methods {

// ── Request methods ──
inline constexpr std::string_view kInitialize = "initialize";
inline constexpr std::string_view kPing = "ping";

inline constexpr std::string_view kListTools = "tools/list";
inline constexpr std::string_view kCallTool = "tools/call";

inline constexpr std::string_view kListResources = "resources/list";
inline constexpr std::string_view kReadResource = "resources/read";
inline constexpr std::string_view kListResourceTemplates = "resources/templates/list";
inline constexpr std::string_view kSubscribeResource = "resources/subscribe";
inline constexpr std::string_view kUnsubscribeResource = "resources/unsubscribe";

inline constexpr std::string_view kListPrompts = "prompts/list";
inline constexpr std::string_view kGetPrompt = "prompts/get";

inline constexpr std::string_view kComplete = "completion/complete";

inline constexpr std::string_view kListRoots = "roots/list";

inline constexpr std::string_view kCreateMessage = "sampling/createMessage";

inline constexpr std::string_view kElicit = "elicitation/create";

inline constexpr std::string_view kDiscover = "server/discover";

inline constexpr std::string_view kSubscribe = "subscriptions/listen";

inline constexpr std::string_view kGetTask = "tasks/get";
inline constexpr std::string_view kUpdateTask = "tasks/update";
inline constexpr std::string_view kCancelTask = "tasks/cancel";

inline constexpr std::string_view kSetLoggingLevel = "logging/setLevel";

inline constexpr std::string_view kListExtensions = "server/extensions/list";
inline constexpr std::string_view kExtensionMethodPrefix = "ext/";

} // namespace methods

namespace notifications {

inline constexpr std::string_view kInitialized = "notifications/initialized";
inline constexpr std::string_view kCancelled = "notifications/cancelled";
inline constexpr std::string_view kResourceUpdated = "notifications/resources/updated";
inline constexpr std::string_view kResourceListChanged = "notifications/resources/list_changed";
inline constexpr std::string_view kToolListChanged = "notifications/tools/list_changed";
inline constexpr std::string_view kPromptListChanged = "notifications/prompts/list_changed";
inline constexpr std::string_view kMessage = "notifications/message";

} // namespace notifications

} // namespace mcp
