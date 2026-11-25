#include <mcp/detail/HttpFraming.hpp>
#include <transport/detail/net/NetIoUtil.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    (void)mcp::detail::ParseCheckedDecimal(text, 4194304);
    (void)mcp::detail::ParseCheckedHex(text, 8388608);
    std::uint64_t number = 0;
    (void)mcp::detail::ParseCheckedUint64(text, number);

    std::size_t position = 0;
    while (position < text.size()) {
        const std::size_t end = text.find('\n', position);
        std::string line(text.substr(position, end == std::string_view::npos
                                                    ? std::string_view::npos
                                                    : end - position));
        mcp::detail::net::TrimInPlace(line);
        (void)mcp::detail::ParseCheckedDecimal(line, 65536);
        (void)mcp::detail::ParseCheckedHex(line, 65536);
        if (end == std::string_view::npos) break;
        position = end + 1;
    }
    return 0;
}
