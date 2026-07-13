#pragma once

#include <mcp/ErrorCodes.hpp>
#include <mcp/McpError.hpp>
#include <mcp/McpTypes.hpp>
#include <variant>
#include <cstdint>
#include <optional>
#include <nlohmann/json.hpp>

namespace mcp {

// ── Forward declarations ──
struct JsonRpcRequest;
struct JsonRpcNotification;
struct JsonRpcResponse;
struct JsonRpcError;

/// JSON-RPC 2.0 request message.
struct JsonRpcRequest {
    std::string jsonrpc{JsonRpcVersion};
    RequestId id;
    std::string method;
    std::optional<nlohmann::json> params;

    nlohmann::json ToJson() const;
    static JsonRpcRequest FromJson(const nlohmann::json& j);
};

/// JSON-RPC 2.0 notification message (no id).
struct JsonRpcNotification {
    std::string jsonrpc{JsonRpcVersion};
    std::string method;
    std::optional<nlohmann::json> params;

    nlohmann::json ToJson() const;
    static JsonRpcNotification FromJson(const nlohmann::json& j);
};

/// JSON-RPC 2.0 success response.
struct JsonRpcResponse {
    std::string jsonrpc{JsonRpcVersion};
    RequestId id;
    nlohmann::json result;

    nlohmann::json ToJson() const;
    static JsonRpcResponse FromJson(const nlohmann::json& j);
};

/// JSON-RPC 2.0 error response.
struct JsonRpcError {
    std::string jsonrpc{JsonRpcVersion};
    RequestId id;
    ErrorData error;

    nlohmann::json ToJson() const;
    static JsonRpcError FromJson(const nlohmann::json& j);
};

/// JSON-RPC 2.0 message variant (the wire format).
using JsonRpcMessage = std::variant<
    JsonRpcRequest,
    JsonRpcNotification,
    JsonRpcResponse,
    JsonRpcError>;

/// Serialize a JsonRpcMessage to JSON.
nlohmann::json JsonRpcMessageToJson(const JsonRpcMessage& msg);

/// Deserialize a JsonRpcMessage from JSON.
/// Returns the appropriate variant member based on JSON-RPC 2.0 dispatch rules:
/// - method present + id present  → JsonRpcRequest
/// - method present + no id       → JsonRpcNotification
/// - id present + result present  → JsonRpcResponse
/// - id present + error present   → JsonRpcError
JsonRpcMessage JsonRpcMessageFromJson(const nlohmann::json& j);

// ── Transport-level message with metadata ──

struct MessageMetadata {
    std::optional<std::string> session_id;
    std::optional<std::map<std::string, std::string>> headers;
    std::optional<RequestId> related_request_id;
};

struct SessionMessage {
    JsonRpcMessage message;
    MessageMetadata metadata;
};

} // namespace mcp
