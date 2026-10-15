#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace mcp { namespace detail {

template <typename Fn>
inline void ForEachSseLine(std::string_view block, Fn&& fn) {
    std::size_t pos = 0;
    while (pos < block.size()) {
        std::size_t end = block.find('\n', pos);
        std::string_view line = (end == std::string_view::npos)
            ? block.substr(pos)
            : block.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        fn(line);
        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
}

struct SseFieldLine {
    std::string_view name;
    std::string_view value;
    bool has_payload = false;
};

inline bool ParseSseFieldLine(std::string_view line, SseFieldLine& out) {
    if (line.empty() || line.front() == ':') return false;
    std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    out.name = line.substr(0, colon);
    std::string_view rest = line.substr(colon + 1);
    std::size_t first = rest.find_first_not_of(" \t");
    out.value = (first == std::string_view::npos) ? std::string_view{} : rest.substr(first);
    out.has_payload = !rest.empty();
    return true;
}

inline void AppendSseData(std::string& data, std::string_view value) {
    if (!data.empty()) data += '\n';
    data += value;
}

}} // namespace mcp::detail
