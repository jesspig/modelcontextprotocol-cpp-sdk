#pragma once

#include <mcp/ProtocolVersion.hpp>
#include <mcp/ErrorCodes.hpp>

#include <mcp/JsonValue.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace mcp {

using RequestId = std::variant<int64_t, std::string>;

struct ErrorData {
    McpErrorCode code{McpErrorCode::InternalError};
    std::string message;
    std::optional<JsonValue> data = std::nullopt;
};

struct JsonRpcRequest {
    std::string jsonrpc = "2.0";
    RequestId id;
    std::string method;
    std::optional<JsonValue> params;
    std::optional<JsonValue> meta;
};

struct JsonRpcNotification {
    std::string jsonrpc = "2.0";
    std::string method;
    std::optional<JsonValue> params;
    std::optional<JsonValue> meta;
};

struct JsonRpcResponse {
    std::string jsonrpc = "2.0";
    RequestId id;
    JsonValue result;
};

struct JsonRpcErrorResponse {
    std::string jsonrpc = "2.0";
    std::optional<RequestId> id;
    ErrorData error;
};

using JsonRpcMessage = std::variant<
    JsonRpcRequest,
    JsonRpcNotification,
    JsonRpcResponse,
    JsonRpcErrorResponse>;

std::string SerializeMessage(const JsonRpcMessage& msg);
std::string SerializeMessage(JsonRpcMessage&& msg);
JsonRpcMessage DeserializeMessage(std::string_view json);

inline bool IsRequest(const JsonRpcMessage& msg) noexcept {
    return std::holds_alternative<JsonRpcRequest>(msg);
}
inline bool IsNotification(const JsonRpcMessage& msg) noexcept {
    return std::holds_alternative<JsonRpcNotification>(msg);
}
inline bool IsResponse(const JsonRpcMessage& msg) noexcept {
    return std::holds_alternative<JsonRpcResponse>(msg);
}
inline bool IsError(const JsonRpcMessage& msg) noexcept {
    return std::holds_alternative<JsonRpcErrorResponse>(msg);
}
inline const JsonRpcRequest* AsRequest(const JsonRpcMessage& msg) noexcept {
    return std::get_if<JsonRpcRequest>(&msg);
}
inline const JsonRpcNotification* AsNotification(const JsonRpcMessage& msg) noexcept {
    return std::get_if<JsonRpcNotification>(&msg);
}
inline const JsonRpcResponse* AsResponse(const JsonRpcMessage& msg) noexcept {
    return std::get_if<JsonRpcResponse>(&msg);
}
inline const JsonRpcErrorResponse* AsError(const JsonRpcMessage& msg) noexcept {
    return std::get_if<JsonRpcErrorResponse>(&msg);
}

JsonValue RequestIdToJson(const RequestId& id);
RequestId RequestIdFromJson(const JsonValue& j);

} // namespace mcp
