#pragma once

#include <mcp/Progress.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/Capabilities.hpp>
#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace mcp {

/// Request _meta envelope. Every request carries protocol version,
/// client info, and capabilities per-request (2026-07-28 style).
struct RequestMeta {
    std::optional<ProgressToken> progress_token;
    std::string protocol_version;
    Implementation client_info;
    ClientCapabilities client_capabilities;
    std::optional<LoggingLevel> log_level;  // [deprecated]
    std::optional<nlohmann::json> additional;

    nlohmann::json ToJson() const;
    static RequestMeta FromJson(const nlohmann::json& j);

    static RequestMeta Default(
        const Implementation& client_info,
        std::string_view protocol_version = LatestProtocolVersion);
};

/// Notification _meta envelope (2026-07-28 style).
struct NotificationMeta {
    std::optional<ProgressToken> progress_token;
    std::optional<std::string> subscription_id;
    std::optional<nlohmann::json> additional;

    nlohmann::json ToJson() const;
    static NotificationMeta FromJson(const nlohmann::json& j);
};

/// Logging levels (RFC 5424).
enum class LoggingLevel {
    Debug = 0,
    Info = 1,
    Notice = 2,
    Warning = 3,
    Error = 4,
    Critical = 5,
    Alert = 6,
    Emergency = 7,
};

std::string_view LoggingLevelToString(LoggingLevel level);
std::optional<LoggingLevel> LoggingLevelFromString(std::string_view s);

} // namespace mcp
