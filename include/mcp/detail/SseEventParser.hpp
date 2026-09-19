// SseEventParser.hpp - SSE 事件块的行迭代与字段行解析

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace mcp { namespace detail {

// 按行迭代 SSE 事件块并剥离行尾 CR。语义与 std::getline 一致：块末尾没有
// 换行时不产生额外的空行。
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
    // 冒号之后是否至少有一个字符。区分 "data:"（无负载）与 "data: "（有负载
    // 但值为空），两侧对这两种形态的历史处理不同，故交由调用方决策。
    bool has_payload = false;
};

// 解析一行 SSE 字段行，value 已剥离冒号后的前导空格与制表符。
// 空行与注释行（以 ':' 开头）返回 false。
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

// 同一事件内的多行 data 按 SSE 规范以 '\n' 连接。
inline void AppendSseData(std::string& data, std::string_view value) {
    if (!data.empty()) data += '\n';
    data += value;
}

}} // namespace mcp::detail
