#pragma once

#include <mcp/McpTypes.hpp>

#include <functional>

namespace mcp {

// ── Server-to-client request handlers (对应 C# McpClientHandlers) ──

// SamplingHandler is deprecated in 2026-07-28 (SEP-2577).
// Use ElicitationHandler instead.
using SamplingHandler [[deprecated("Sampling is deprecated in protocol version 2026-07-28 (SEP-2577)")]] = std::function<CreateMessageResult(
    const CreateMessageRequestParams&)>;

// Roots (deprecated): server requests root directory list
using RootsHandler [[deprecated("Roots is deprecated in protocol version 2026-07-28")]] = std::function<ListRootsResult(
    const ListRootsRequestParams&)>;

// Elicitation: server requests user input
using ElicitationHandler = std::function<ElicitResult(
    const ElicitRequestParams&)>;

// Notification handler: server sends notification
using ClientNotificationHandler = std::function<void(
    const JsonRpcNotification&)>;

// ── ClientHandlers aggregate (对应 C# McpClientHandlers) ──
struct ClientHandlers {
    SamplingHandler sampling_handler;
    RootsHandler roots_handler;
    ElicitationHandler elicitation_handler;
    std::vector<std::pair<std::string, ClientNotificationHandler>> notification_handlers;
};

} // namespace mcp
