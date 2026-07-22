#pragma once

#include <mcp/ProtocolVersion.hpp>
#include <mcp/ErrorCodes.hpp>

#include <mcp/JsonValue.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace mcp {

// ── RequestId = variant<int64_t, string> ──
using RequestId = std::variant<int64_t, std::string>;

// ── ErrorData ──
struct ErrorData {
    McpErrorCode code{McpErrorCode::InternalError};
    std::string message;
    std::optional<JsonValue> data = std::nullopt;
};

// ── JsonRpcRequest ──
struct JsonRpcRequest {
    std::string jsonrpc = "2.0";
    RequestId id;
    std::string method;
    std::optional<JsonValue> params;
    std::optional<JsonValue> meta;   // top-level _meta (2026 era)
};

// ── JsonRpcNotification ──
struct JsonRpcNotification {
    std::string jsonrpc = "2.0";
    std::string method;
    std::optional<JsonValue> params;
    std::optional<JsonValue> meta;   // top-level _meta (2026 era)
};

// ── JsonRpcResponse ──
struct JsonRpcResponse {
    std::string jsonrpc = "2.0";
    RequestId id;
    JsonValue result;
};

// ── JsonRpcErrorResponse ──
struct JsonRpcErrorResponse {
    std::string jsonrpc = "2.0";
    std::optional<RequestId> id;
    ErrorData error;
};

// ── JsonRpcMessage variant ──
using JsonRpcMessage = std::variant<
    JsonRpcRequest,
    JsonRpcNotification,
    JsonRpcResponse,
    JsonRpcErrorResponse>;

// ── Serialization functions (implemented in JsonRpc.cpp) ──
std::string SerializeMessage(const JsonRpcMessage& msg);
JsonRpcMessage DeserializeMessage(std::string_view json);

// ── Helpers ──
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

// ── RequestId serialization (implemented in JsonRpc.cpp) ──
JsonValue RequestIdToJson(const RequestId& id);
RequestId RequestIdFromJson(const JsonValue& j);

} // namespace mcp
