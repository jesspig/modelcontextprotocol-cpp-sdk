#include <mcp/McpError.hpp>
#include <mcp/detail/WebSocketFrameParser.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    try {
        const mcp::detail::WebSocketFrameHeader header = mcp::detail::ParseWebSocketFrameHeader(bytes);
        (void)header;
    } catch (const mcp::McpError&) {
    } catch (...) {
    }
    return 0;
}
