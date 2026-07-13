#pragma once

#include <mcp/ProtocolVersion.hpp>
#include <mcp/ErrorCodes.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace mcp {

// ── RequestId = variant<int64_t, string> ──
using RequestId = std::variant<int64_t, std::string>;

inline void to_json(nlohmann::json& j, const RequestId& id) {
    std::visit([&j](const auto& v) { j = v; }, id);
}
inline RequestId request_id_from_json(const nlohmann::json& j) {
    if (j.is_number_integer()) return j.get<int64_t>();
    return j.get<std::string>();
}

// ── ErrorData ──
struct ErrorData {
    McpErrorCode code{McpErrorCode::InternalError};
    std::string message;
    std::optional<nlohmann::json> data;
};
inline void to_json(nlohmann::json& j, const ErrorData& v) {
    j = nlohmann::json::object();
    j["code"] = static_cast<int32_t>(v.code);
    j["message"] = v.message;
    if (v.data) j["data"] = *v.data;
}
inline void from_json(const nlohmann::json& j, ErrorData& v) {
    v.code = static_cast<McpErrorCode>(j.at("code").get<int32_t>());
    v.message = j.at("message").get<std::string>();
    if (auto it = j.find("data"); it != j.end()) v.data = *it;
}

// ── JsonRpcRequest ──
struct JsonRpcRequest {
    std::string jsonrpc = "2.0";
    RequestId id;
    std::string method;
    std::optional<nlohmann::json> params;
    std::optional<nlohmann::json> meta;   // top-level _meta (2026 era)
};
inline void to_json(nlohmann::json& j, const JsonRpcRequest& v) {
    j = nlohmann::json::object();
    j["jsonrpc"] = v.jsonrpc;
    std::visit([&j](const auto& val) { j["id"] = val; }, v.id);
    j["method"] = v.method;
    if (v.params) j["params"] = *v.params;
    if (v.meta)   j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, JsonRpcRequest& v) {
    j.at("jsonrpc").get_to(v.jsonrpc);
    v.id = request_id_from_json(j.at("id"));
    v.method = j.at("method").get<std::string>();
    if (auto it = j.find("params"); it != j.end()) v.params = *it;
    if (auto it = j.find("_meta"); it != j.end())  v.meta = *it;
}

// ── JsonRpcNotification ──
struct JsonRpcNotification {
    std::string jsonrpc = "2.0";
    std::string method;
    std::optional<nlohmann::json> params;
};
inline void to_json(nlohmann::json& j, const JsonRpcNotification& v) {
    j = nlohmann::json::object();
    j["jsonrpc"] = v.jsonrpc;
    j["method"] = v.method;
    if (v.params) j["params"] = *v.params;
}
inline void from_json(const nlohmann::json& j, JsonRpcNotification& v) {
    j.at("jsonrpc").get_to(v.jsonrpc);
    v.method = j.at("method").get<std::string>();
    if (auto it = j.find("params"); it != j.end()) v.params = *it;
}

// ── JsonRpcResponse ──
struct JsonRpcResponse {
    std::string jsonrpc = "2.0";
    RequestId id;
    nlohmann::json result;
};
inline void to_json(nlohmann::json& j, const JsonRpcResponse& v) {
    j = nlohmann::json::object();
    j["jsonrpc"] = v.jsonrpc;
    std::visit([&j](const auto& val) { j["id"] = val; }, v.id);
    j["result"] = v.result;
}
inline void from_json(const nlohmann::json& j, JsonRpcResponse& v) {
    j.at("jsonrpc").get_to(v.jsonrpc);
    v.id = request_id_from_json(j.at("id"));
    v.result = j.at("result");
}

// ── JsonRpcErrorResponse ──
struct JsonRpcErrorResponse {
    std::string jsonrpc = "2.0";
    RequestId id;
    ErrorData error;
};
inline void to_json(nlohmann::json& j, const JsonRpcErrorResponse& v) {
    j = nlohmann::json::object();
    j["jsonrpc"] = v.jsonrpc;
    std::visit([&j](const auto& val) { j["id"] = val; }, v.id);
    j["error"] = v.error;
}
inline void from_json(const nlohmann::json& j, JsonRpcErrorResponse& v) {
    j.at("jsonrpc").get_to(v.jsonrpc);
    v.id = request_id_from_json(j.at("id"));
    v.error = j.at("error").get<ErrorData>();
}

// ── JsonRpcMessage variant ──
using JsonRpcMessage = std::variant<
    JsonRpcRequest,
    JsonRpcNotification,
    JsonRpcResponse,
    JsonRpcErrorResponse>;

inline void to_json(nlohmann::json& j, const JsonRpcMessage& msg) {
    std::visit([&j](const auto& m) { j = nlohmann::json(m); }, msg);
}
inline void from_json(const nlohmann::json& j, JsonRpcMessage& msg) {
    auto it_method = j.find("method");
    auto it_id = j.find("id");
    auto it_result = j.find("result");
    auto it_error = j.find("error");

    if (it_method != j.end() && it_id != j.end()) {
        msg = j.get<JsonRpcRequest>();
    } else if (it_method != j.end()) {
        msg = j.get<JsonRpcNotification>();
    } else if (it_result != j.end()) {
        msg = j.get<JsonRpcResponse>();
    } else if (it_error != j.end()) {
        msg = j.get<JsonRpcErrorResponse>();
    } else {
        throw std::runtime_error("unknown JSON-RPC message type");
    }
}

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

} // namespace mcp
