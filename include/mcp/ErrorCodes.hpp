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

    // ── 细粒度错误子分类 ──
    DeserializeFailed = -32002,     // 反序列化失败
    ConnectionRefused = -32003,     // 连接被拒绝
    TlsHandshakeFailed = -32004,    // TLS 握手失败
    ProtocolViolation = -32005,     // 协议违规（消息格式等）
    TaskNotFound = -32006,          // 任务未找到
    HandlerError = -32007,          // 用户 handler 抛出异常
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
            case McpErrorCode::DeserializeFailed: return "Deserialize failed";
            case McpErrorCode::ConnectionRefused: return "Connection refused";
            case McpErrorCode::TlsHandshakeFailed: return "TLS handshake failed";
            case McpErrorCode::ProtocolViolation: return "Protocol violation";
            case McpErrorCode::TaskNotFound: return "Task not found";
            case McpErrorCode::HandlerError: return "Handler error";
            default: return "Unknown MCP error";
        }
    }
    std::error_condition default_error_condition(int ev) const noexcept override {
        switch (static_cast<McpErrorCode>(ev)) {
            case McpErrorCode::ConnectionClosed:
            case McpErrorCode::RequestTimeout:
            case McpErrorCode::ConnectionRefused:
            case McpErrorCode::TlsHandshakeFailed:
                return std::errc::connection_aborted;
            case McpErrorCode::InternalError:
            case McpErrorCode::DeserializeFailed:
            case McpErrorCode::ProtocolViolation:
            case McpErrorCode::HandlerError:
                return std::errc::io_error;
            case McpErrorCode::TaskNotFound:
                return std::errc::no_such_process;
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
