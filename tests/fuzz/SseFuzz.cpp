#include <mcp/detail/SseEventParser.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    mcp::detail::SseIncrementalParser parser(1048576);
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t chunk = text.size() - offset < 7 ? text.size() - offset : 7;
        const mcp::detail::SseFeedResult fed = parser.Feed(text.substr(offset, chunk));
        (void)fed;
        offset += chunk;
    }
    const mcp::detail::SseFeedResult flushed = parser.Flush(true);
    (void)flushed;
    for (std::size_t position = 0; position < text.size();) {
        const std::size_t end = text.find('\n', position);
        mcp::detail::SseFieldLine field;
        (void)mcp::detail::ParseSseFieldLine(
            text.substr(position, end == std::string_view::npos ? std::string_view::npos : end - position),
            field);
        if (end == std::string_view::npos) break;
        position = end + 1;
    }
    return 0;
}
