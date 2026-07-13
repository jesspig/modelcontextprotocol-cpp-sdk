#pragma once

#include <cstdint>
#include <variant>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace mcp {

/// Progress token used to associate progress notifications with the original request.
using ProgressToken = std::variant<int64_t, std::string>;

/// Progress notification params (RFC 5427 style).
struct ProgressNotificationParams {
    ProgressToken progress_token;
    double progress;
    std::optional<double> total;
    std::optional<std::string> message;

    nlohmann::json ToJson() const;
    static ProgressNotificationParams FromJson(const nlohmann::json& j);
};

} // namespace mcp
