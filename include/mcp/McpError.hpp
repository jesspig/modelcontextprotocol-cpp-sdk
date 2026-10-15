#pragma once

#include <mcp/ErrorCodes.hpp>
#include <mcp/JsonRpc.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

namespace mcp {

class McpError : public std::runtime_error {
public:
    explicit McpError(McpErrorCode code, std::string message)
        : std::runtime_error(message)
        , code_(code)
        , message_(std::move(message)) {}

    McpError(McpErrorCode code, std::string message, std::optional<JsonValue> data)
        : std::runtime_error(message)
        , code_(code)
        , message_(std::move(message))
        , data_(std::move(data)) {}

    McpErrorCode Code() const noexcept { return code_; }
    std::string_view ErrorMessage() const noexcept { return message_; }
    const std::optional<JsonValue>& Data() const noexcept { return data_; }


private:
    McpErrorCode code_;
    std::string message_;
    std::optional<JsonValue> data_;
};



} // namespace mcp
