#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace mcp { namespace detail {

enum class HttpNumberError { None, Empty, InvalidDigit, Overflow, LimitExceeded };

struct HttpNumberResult {
    std::size_t value = 0;
    HttpNumberError error = HttpNumberError::None;
};

[[nodiscard]] inline HttpNumberResult ParseCheckedDecimal(std::string_view text,
                                                          std::size_t limit) {
    HttpNumberResult out;
    if (text.empty()) {
        out.error = HttpNumberError::Empty;
        return out;
    }
    std::size_t value = 0;
    bool exceeds_limit = false;
    for (char c : text) {
        if (c < '0' || c > '9') {
            out.error = HttpNumberError::InvalidDigit;
            return out;
        }
        const std::size_t digit = static_cast<std::size_t>(c - '0');
        if (value > ((std::numeric_limits<std::size_t>::max)() - digit) / 10) {
            out.error = HttpNumberError::Overflow;
            return out;
        }
        value = value * 10 + digit;
        if (value > limit) exceeds_limit = true;
    }
    if (exceeds_limit) {
        out.error = HttpNumberError::LimitExceeded;
        return out;
    }
    out.value = value;
    return out;
}

[[nodiscard]] inline HttpNumberResult ParseCheckedHex(std::string_view text,
                                                      std::size_t limit) {
    HttpNumberResult out;
    if (text.empty()) {
        out.error = HttpNumberError::Empty;
        return out;
    }
    std::size_t value = 0;
    bool exceeds_limit = false;
    for (char c : text) {
        std::size_t digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<std::size_t>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<std::size_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<std::size_t>(c - 'A' + 10);
        else {
            out.error = HttpNumberError::InvalidDigit;
            return out;
        }
        if (value > ((std::numeric_limits<std::size_t>::max)() - digit) / 16) {
            out.error = HttpNumberError::Overflow;
            return out;
        }
        value = value * 16 + digit;
        if (value > limit) exceeds_limit = true;
    }
    if (exceeds_limit) {
        out.error = HttpNumberError::LimitExceeded;
        return out;
    }
    out.value = value;
    return out;
}

[[nodiscard]] inline bool ParseCheckedUint64(std::string_view text, uint64_t& out) {
    if (text.empty()) return false;
    uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > ((std::numeric_limits<uint64_t>::max)() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

}} // namespace mcp::detail
