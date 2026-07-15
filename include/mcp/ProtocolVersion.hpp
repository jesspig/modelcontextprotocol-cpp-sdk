#pragma once

#include <string_view>

namespace mcp {

inline constexpr std::string_view kLatestProtocolVersion = "2026-07-28";
inline constexpr std::string_view kJsonRpcVersion = "2.0";

inline constexpr std::string_view kProtocolVersions[] = {
    "2024-11-05",
    "2025-03-26",
    "2025-06-18",
    "2025-11-25",
    "2026-07-28",
};

constexpr bool IsModernProtocolVersion(std::string_view version) noexcept {
    return version >= "2026-07-28";
}

} // namespace mcp
