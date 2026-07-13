#pragma once

#include <mcp/ErrorCodes.hpp>
#include <stdexcept>
#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace mcp {

/// Structured error data carried in JSON-RPC error responses.
struct ErrorData {
    McpErrorCode code;
    std::string message;
    std::optional<nlohmann::json> data;

    nlohmann::json ToJson() const;
    static ErrorData FromJson(const nlohmann::json& j);
};

/// SDK-level exception raised when a protocol error arrives over wire.
class McpError : public std::runtime_error {
public:
    explicit McpError(ErrorData error);
    explicit McpError(McpErrorCode code, std::string_view message);

    McpErrorCode Code() const noexcept { return error_.code; }
    const ErrorData& Data() const noexcept { return error_; }
    const std::optional<nlohmann::json>& Details() const noexcept { return error_.data; }

    static McpError FromJsonRpcResponse(const nlohmann::json& error_obj);

private:
    ErrorData error_;
};

/// Protocol-level error (crosses the wire).
class McpProtocolError : public McpError {
public:
    explicit McpProtocolError(McpErrorCode code, std::string_view message = {});
};

/// SDK-internal error (does not cross the wire).
class McpSdkError : public McpError {
public:
    explicit McpSdkError(McpErrorCode code, std::string_view message = {});
};

/// Transport-level error.
class TransportError : public McpSdkError {
public:
    explicit TransportError(std::string_view message);
};

/// Connection closed error.
class ConnectionClosedError : public McpSdkError {
public:
    explicit ConnectionClosedError(std::string_view message = "Connection closed");
};

/// Missing back-channel for server-initiated request.
class NoBackChannelError : public McpSdkError {
public:
    explicit NoBackChannelError(std::string_view method);
};

/// Resource not found error.
class ResourceNotFoundError : public McpProtocolError {
public:
    explicit ResourceNotFoundError(std::string_view uri);
};

/// URL elicitation required error.
class UrlElicitationRequiredError : public McpProtocolError {
public:
    explicit UrlElicitationRequiredError(const nlohmann::json& elicitations);
};

} // namespace mcp
