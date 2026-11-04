#pragma once

#include <cstddef>
#include <string_view>

namespace mcp {

inline constexpr std::string_view kLatestProtocolVersion = "2026-07-28";
inline constexpr std::string_view kLegacyProtocolVersion = "2025-11-25";
inline constexpr std::string_view kDefaultNegotiatedProtocolVersion = "2025-03-26";
inline constexpr std::string_view kModernProtocolVersion = "2026-07-28";
inline constexpr std::string_view kJsonRpcVersion = "2.0";

inline constexpr std::string_view kProtocolVersions[] = {
    "2024-11-05",
    "2025-03-26",
    "2025-06-18",
    "2025-11-25",
    "2026-07-28",
};

[[nodiscard]] constexpr bool IsModernProtocolVersion(std::string_view version) noexcept {
    return version >= kLatestProtocolVersion;
}

constexpr bool IsProtocolVersionTableStrictlyAscending() noexcept {
    for (std::size_t i = 1; i < std::size(kProtocolVersions); ++i) {
        if (!(kProtocolVersions[i - 1] < kProtocolVersions[i])) return false;
    }
    return true;
}

static_assert(IsProtocolVersionTableStrictlyAscending(),
    "kProtocolVersions must stay strictly ascending: negotiation walks it newest-first");
static_assert(IsModernProtocolVersion(kModernProtocolVersion),
    "the modern era threshold must itself count as modern");
static_assert(!IsModernProtocolVersion(kLegacyProtocolVersion),
    "the named legacy version must not count as modern");

} // namespace mcp
