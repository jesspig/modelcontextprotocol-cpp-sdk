#pragma once

#include <cstdint>

namespace mcp {

enum class McpErrorCode : int32_t {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
    HeaderMismatch = -32020,
    MissingRequiredClientCapability = -32021,
    UnsupportedProtocolVersion = -32022,
    UrlElicitationRequired = -32042,
    ConnectionClosed = -32000,
    RequestTimeout = -32001,
    RequestCancelled = -32800,
};

} // namespace mcp
