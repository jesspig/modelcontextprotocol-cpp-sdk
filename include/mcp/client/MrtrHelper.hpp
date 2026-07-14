#pragma once
#include <mcp/McpTypes.hpp>
#include <functional>
#include <optional>

namespace mcp {

// ═══════════════════════════════════════════════════════════════════════
// MrtrDriver — drives the MRTR loop (max_rounds retries)
// ═══════════════════════════════════════════════════════════════════════
// Mirrors the C# pattern: detect InputRequiredResult, resolve, retry
class MrtrDriver {
public:
    using RequestSender = std::function<nlohmann::json(
        nlohmann::json params, const std::optional<nlohmann::json>& input_responses,
        const std::optional<std::string>& request_state)>;

    using InputResolver = std::function<nlohmann::json(
        const InputRequests& requests)>;

    MrtrDriver(int max_rounds = 8) : max_rounds_(max_rounds) {}

    // Execute a request with MRTR support
    nlohmann::json Execute(
        RequestSender send,
        InputResolver resolve,
        nlohmann::json initial_params);

    int max_rounds_;

    static bool IsInputRequired(const nlohmann::json& result) {
        auto it = result.find("resultType");
        return it != result.end() && *it == "input_required";
    }
};

} // namespace mcp
