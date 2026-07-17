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


private:
    McpErrorCode code_;
    std::string message_;
};



} // namespace mcp
