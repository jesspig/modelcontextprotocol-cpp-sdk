#pragma once

#include <mcp/ErrorCodes.hpp>
#include <mcp/JsonRpc.hpp>

#include <stdexcept>
#include <string>
#include <system_error>

namespace mcp {

// ── McpError ──
class McpError : public std::runtime_error {
public:
    explicit McpError(McpErrorCode code, std::string message)
        : std::runtime_error(message)
        , code_(code)
        , message_(std::move(message)) {}

    McpErrorCode Code() const noexcept { return code_; }
    std::string_view ErrorMessage() const noexcept { return message_; }
    std::error_code ToErrorCode() const noexcept {
        return make_error_code(code_);
    }

    static McpError FromJsonRpcError(const JsonRpcErrorResponse& err) {
        return McpError(
            static_cast<McpErrorCode>(err.error.code),
            err.error.message);
    }

private:
    McpErrorCode code_;
    std::string message_;
};

// ── McpProtocolError ──
class McpProtocolError : public McpError {
public:
    using McpError::McpError;
};

// ── MissingRequiredClientCapabilityError ──
class MissingRequiredClientCapabilityError : public McpError {
public:
    explicit MissingRequiredClientCapabilityError(const std::string& capability)
        : McpError(McpErrorCode::MissingRequiredClientCapability,
                   "missing required client capability: " + capability)
        , capability_(capability) {}

    std::string_view Capability() const noexcept { return capability_; }

    nlohmann::json ToErrorData() const {
        nlohmann::json data = nlohmann::json::object();
        data["requiredCapabilities"] = nlohmann::json::array({capability_});
        return data;
    }

private:
    std::string capability_;
};

// ── SDK errors (local only) ──
class SdkError : public std::runtime_error {
public:
    enum Code {
        ConnectionClosed = 1,
        RequestTimeout,
        NotConnected,
        UnsupportedResult,
        Internal,
    };

    explicit SdkError(Code code, std::string message)
        : std::runtime_error(message)
        , code_(code) {}

    Code SdkCode() const noexcept { return code_; }

private:
    Code code_;
};

} // namespace mcp
