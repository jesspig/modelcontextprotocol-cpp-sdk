#pragma once

#include <mcp/detail/StringUtils.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace mcp { namespace detail { namespace net {

inline constexpr std::size_t kMaxLineBytes = 8 * 1024;
inline constexpr std::size_t kMaxHeaderBytes = 64 * 1024;

inline std::string ToLower(std::string_view text) {
    return mcp::detail::ToLower(text);
}

inline void TrimInPlace(std::string& text) {
    std::size_t first = text.find_first_not_of(" \t");
    std::size_t last = text.find_last_not_of(" \t");
    if (first == std::string::npos) text.clear();
    else text = text.substr(first, last - first + 1);
}

inline std::chrono::milliseconds Remaining(const std::chrono::steady_clock::time_point& deadline) {
    auto left = deadline - std::chrono::steady_clock::now();
    if (left <= std::chrono::milliseconds(0)) return std::chrono::milliseconds(0);
    return std::chrono::duration_cast<std::chrono::milliseconds>(left);
}

}}} // namespace mcp::detail::net
