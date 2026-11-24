#pragma once

#include <algorithm>
#include <string>
#include <string_view>

namespace mcp { namespace detail {

inline void SecureClear(std::string& value) {
    std::fill(value.begin(), value.end(), '\0');
    value.clear();
}

[[nodiscard]] inline bool IsNonEmptySecret(std::string_view value) {
    if (value.empty() || value.size() > 4096) return false;
    for (char c : value) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x21 || u == 0x7F) return false;
    }
    return true;
}

}} // namespace mcp::detail
