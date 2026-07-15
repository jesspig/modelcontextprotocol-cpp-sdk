#pragma once

#include <cstdint>
#include <system_error>

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

// Enable std::error_code integration
inline std::error_code make_error_code(McpErrorCode e) noexcept;

} // namespace mcp

namespace std {
    template <>
    struct is_error_code_enum<mcp::McpErrorCode> : true_type {};
}

namespace mcp {

class McpErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override { return "MCP"; }
    std::string message(int ev) const override {
        switch (static_cast<McpErrorCode>(ev)) {
            case McpErrorCode::ParseError: return "Parse error";
            case McpErrorCode::InvalidRequest: return "Invalid request";
            case McpErrorCode::MethodNotFound: return "Method not found";
            case McpErrorCode::InvalidParams: return "Invalid params";
            case McpErrorCode::InternalError: return "Internal error";
            case McpErrorCode::HeaderMismatch: return "Header mismatch";
            case McpErrorCode::MissingRequiredClientCapability: return "Missing required client capability";
            case McpErrorCode::UnsupportedProtocolVersion: return "Unsupported protocol version";
            case McpErrorCode::UrlElicitationRequired: return "URL elicitation required";
            case McpErrorCode::ConnectionClosed: return "Connection closed";
            case McpErrorCode::RequestTimeout: return "Request timeout";
            case McpErrorCode::RequestCancelled: return "Request cancelled";
            default: return "Unknown MCP error";
        }
    }
    std::error_condition default_error_condition(int ev) const noexcept override {
        switch (static_cast<McpErrorCode>(ev)) {
            case McpErrorCode::ConnectionClosed:
            case McpErrorCode::RequestTimeout:
                return std::errc::connection_aborted;
            case McpErrorCode::InternalError:
                return std::errc::io_error;
            default:
                return std::error_condition(ev, *this);
        }
    }
};

inline const McpErrorCategory& GetMcpErrorCategory() {
    static const McpErrorCategory category;
    return category;
}

inline std::error_code make_error_code(McpErrorCode e) noexcept {
    return std::error_code(static_cast<int>(e), GetMcpErrorCategory());
}

} // namespace mcp
