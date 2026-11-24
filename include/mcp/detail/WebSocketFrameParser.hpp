#pragma once

#include <mcp/McpError.hpp>
#include <mcp/transport/detail/Limits.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mcp { namespace detail {

struct WebSocketFrameHeader {
    bool fin = false;
    int opcode = 0;
    uint64_t length = 0;
    std::size_t bytes_consumed = 0;
};

[[nodiscard]] inline WebSocketFrameHeader ParseWebSocketFrameHeader(std::string_view bytes) {
    WebSocketFrameHeader out;
    if (bytes.size() < 2)
        throw McpError(McpErrorCode::ProtocolViolation, "WebSocket frame header truncated");
    const uint8_t b0 = static_cast<uint8_t>(static_cast<unsigned char>(bytes[0]));
    const uint8_t b1 = static_cast<uint8_t>(static_cast<unsigned char>(bytes[1]));
    out.fin = (b0 & 0x80) != 0;
    if ((b0 & 0x70) != 0)
        throw McpError(McpErrorCode::ProtocolViolation, "WebSocket frame has non-zero RSV bits");
    out.opcode = b0 & 0x0F;
    if ((b1 & 0x80) != 0)
        throw McpError(McpErrorCode::ProtocolViolation,
                       "WebSocket frame from server must not be masked");
    uint64_t length = b1 & 0x7F;
    std::size_t consumed = 2;
    if (length == 126) {
        if (bytes.size() < 4)
            throw McpError(McpErrorCode::ProtocolViolation, "WebSocket extended length truncated");
        length = (static_cast<uint64_t>(static_cast<unsigned char>(bytes[2])) << 8) |
                 static_cast<unsigned char>(bytes[3]);
        consumed = 4;
    } else if (length == 127) {
        if (bytes.size() < 10)
            throw McpError(McpErrorCode::ProtocolViolation, "WebSocket extended length truncated");
        if ((static_cast<unsigned char>(bytes[2]) & 0x80) != 0)
            throw McpError(McpErrorCode::ProtocolViolation,
                           "WebSocket frame length exceeds 63 bits");
        length = 0;
        for (int index = 0; index < 8; ++index)
            length = (length << 8) | static_cast<unsigned char>(bytes[2 + static_cast<std::size_t>(index)]);
        consumed = 10;
    }
    if (length > kMaxMessageSize)
        throw McpError(McpErrorCode::ProtocolViolation, "WebSocket frame exceeds size limit");
    const bool control = (out.opcode & 0x08) != 0;
    if (control) {
        if (!out.fin)
            throw McpError(McpErrorCode::ProtocolViolation,
                           "WebSocket control frame must not be fragmented");
        if (length > 125)
            throw McpError(McpErrorCode::ProtocolViolation,
                           "WebSocket control frame payload too large");
    }
    out.length = length;
    out.bytes_consumed = consumed;
    return out;
}

}} // namespace mcp::detail
