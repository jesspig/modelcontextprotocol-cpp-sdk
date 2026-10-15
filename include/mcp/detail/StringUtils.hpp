#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace mcp { namespace detail {

inline std::string ToLower(std::string_view text) {
    std::string result(text);
    for (char& c : result)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

inline void ToLowerInPlace(std::string& text) {
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

}} // namespace mcp::detail
