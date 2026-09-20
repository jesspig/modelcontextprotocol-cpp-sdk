#include <mcp/JsonRpc.hpp>
#include <mcp/McpError.hpp>
#include <detail/JsonFields.hpp>
#include <detail/JsonSerializer.hpp>

#include <cstdint>
#include <string>
#include <type_traits>

namespace mcp {

namespace {

void ValidateVersion(std::string_view ver) {
    if (ver != kJsonRpcVersion)
        throw McpError(McpErrorCode::InvalidRequest,
            std::string("JSON-RPC version mismatch: got '") + std::string(ver) +
            "', expected '" + std::string(kJsonRpcVersion) + "'");
}

void MergeMetaIntoParams(JsonValue& params_slot, JsonValue meta) {
    if (!params_slot.IsObject()) params_slot = JsonValue(JsonValue::object_tag);
    JsonValue* params_meta = params_slot.Find(detail::kMeta);
    if (!params_meta || !params_meta->IsObject()) {
        params_slot[detail::kMeta] = std::move(meta);
        return;
    }
    for (auto& [key, value] : meta.GetObject())
        (*params_meta)[key] = std::move(value);
}

std::optional<JsonValue> ExtractMetaFromParams(std::optional<JsonValue>& params) {
    if (!params || !params->IsObject()) return std::nullopt;
    auto* params_meta = params->Find(detail::kMeta);
    if (!params_meta) return std::nullopt;
    std::optional<JsonValue> meta = *params_meta;
    params->GetObject().erase(std::string(detail::kMeta));
    return meta;
}

bool JsonRpcIsKnownErrorCode(int64_t code) {
    switch (code) {
    case static_cast<int64_t>(McpErrorCode::ParseError):
    case static_cast<int64_t>(McpErrorCode::InvalidRequest):
    case static_cast<int64_t>(McpErrorCode::MethodNotFound):
    case static_cast<int64_t>(McpErrorCode::InvalidParams):
    case static_cast<int64_t>(McpErrorCode::InternalError):
    case static_cast<int64_t>(McpErrorCode::HeaderMismatch):
    case static_cast<int64_t>(McpErrorCode::MissingRequiredClientCapability):
    case static_cast<int64_t>(McpErrorCode::UnsupportedProtocolVersion):
    case static_cast<int64_t>(McpErrorCode::UrlElicitationRequired):
    case static_cast<int64_t>(McpErrorCode::ResourceNotFound):
    case static_cast<int64_t>(McpErrorCode::ConnectionClosed):
    case static_cast<int64_t>(McpErrorCode::RequestTimeout):
    case static_cast<int64_t>(McpErrorCode::RequestCancelled):
    case static_cast<int64_t>(McpErrorCode::ConnectionRefused):
    case static_cast<int64_t>(McpErrorCode::TlsHandshakeFailed):
    case static_cast<int64_t>(McpErrorCode::ProtocolViolation):
    case static_cast<int64_t>(McpErrorCode::TaskNotFound):
    case static_cast<int64_t>(McpErrorCode::HandlerError):
    case static_cast<int64_t>(McpErrorCode::DeserializeFailed):
        return true;
    default:
        return false;
    }
}

} // namespace

JsonValue RequestIdToJson(const RequestId& id) {
    return std::visit([](const auto& id_val) -> JsonValue {
        return JsonValue(id_val);
    }, id);
}

RequestId RequestIdFromJson(const JsonValue& j) {
    if (j.IsInt()) return j.GetInt();
    return j.GetString();
}

JsonValue SerializeErrorData(const ErrorData& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kCode] = static_cast<int64_t>(v.code);
    obj[detail::kMessage] = JsonValue(v.message);
    detail::SerializeOptional(obj, detail::kData, v.data);
    return obj;
}

ErrorData DeserializeErrorData(const JsonValue& j) {
    ErrorData v;
    int64_t code = j[detail::kCode].GetInt();
    if (!JsonRpcIsKnownErrorCode(code))
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("JSON-RPC error code out of range: ") + std::to_string(code));
    v.code = static_cast<McpErrorCode>(code);
    v.message = j[detail::kMessage].GetString();
    detail::DeserializeOptional(j, detail::kData, v.data);
    return v;
}

JsonValue SerializeJsonRpcRequest(const JsonRpcRequest& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kJsonrpc] = JsonValue(std::string(kJsonRpcVersion));
    obj[detail::kId] = RequestIdToJson(v.id);
    obj[detail::kMethod] = JsonValue(v.method);
    detail::SerializeOptional(obj, detail::kParams, v.params);
    if (v.meta) MergeMetaIntoParams(obj[detail::kParams], *v.meta);
    return obj;
}

JsonValue SerializeJsonRpcRequest(JsonRpcRequest&& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kJsonrpc] = JsonValue(std::string(kJsonRpcVersion));
    obj[detail::kId] = RequestIdToJson(std::move(v.id));
    obj[detail::kMethod] = JsonValue(std::move(v.method));
    if (v.params) obj[detail::kParams] = std::move(*v.params);
    if (v.meta) MergeMetaIntoParams(obj[detail::kParams], std::move(*v.meta));
    return obj;
}

JsonRpcRequest DeserializeJsonRpcRequest(const JsonValue& j) {
    JsonRpcRequest v;
    v.jsonrpc = j[detail::kJsonrpc].GetString();
    ValidateVersion(v.jsonrpc);
    auto* id_ptr = j.Find(detail::kId);
    if (!id_ptr || id_ptr->IsNull())
        throw McpError(McpErrorCode::InvalidRequest,
            "JSON-RPC request id must not be null (JSON-RPC 2.0 section 5.1)");
    v.id = RequestIdFromJson(*id_ptr);
    v.method = j[detail::kMethod].GetString();
    detail::DeserializeOptional(j, detail::kParams, v.params);
    v.meta = ExtractMetaFromParams(v.params);
    return v;
}

JsonValue SerializeJsonRpcNotification(const JsonRpcNotification& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kJsonrpc] = JsonValue(std::string(kJsonRpcVersion));
    obj[detail::kMethod] = JsonValue(v.method);
    detail::SerializeOptional(obj, detail::kParams, v.params);
    if (v.meta) MergeMetaIntoParams(obj[detail::kParams], *v.meta);
    return obj;
}

JsonRpcNotification DeserializeJsonRpcNotification(const JsonValue& j) {
    JsonRpcNotification v;
    v.jsonrpc = j[detail::kJsonrpc].GetString();
    ValidateVersion(v.jsonrpc);
    v.method = j[detail::kMethod].GetString();
    detail::DeserializeOptional(j, detail::kParams, v.params);
    v.meta = ExtractMetaFromParams(v.params);
    return v;
}

JsonValue SerializeJsonRpcResponse(const JsonRpcResponse& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kJsonrpc] = JsonValue(std::string(kJsonRpcVersion));
    obj[detail::kId] = RequestIdToJson(v.id);
    obj[detail::kResult] = v.result;
    return obj;
}

JsonValue SerializeJsonRpcResponse(JsonRpcResponse&& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kJsonrpc] = JsonValue(std::string(kJsonRpcVersion));
    obj[detail::kId] = RequestIdToJson(std::move(v.id));
    obj[detail::kResult] = std::move(v.result);
    return obj;
}

JsonRpcResponse DeserializeJsonRpcResponse(const JsonValue& j) {
    JsonRpcResponse v;
    v.jsonrpc = j[detail::kJsonrpc].GetString();
    ValidateVersion(v.jsonrpc);
    v.id = RequestIdFromJson(j[detail::kId]);
    v.result = j[detail::kResult];
    return v;
}

JsonValue SerializeJsonRpcErrorResponse(const JsonRpcErrorResponse& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kJsonrpc] = JsonValue(std::string(kJsonRpcVersion));
    if (v.id) {
        obj[detail::kId] = RequestIdToJson(*v.id);
    } else {
        obj[detail::kId] = JsonValue(nullptr);
    }
    obj[detail::kError] = SerializeErrorData(v.error);
    return obj;
}

JsonRpcErrorResponse DeserializeJsonRpcErrorResponse(const JsonValue& j) {
    JsonRpcErrorResponse v;
    v.jsonrpc = j[detail::kJsonrpc].GetString();
    ValidateVersion(v.jsonrpc);
    auto* id_ptr = j.Find(detail::kId);
    if (id_ptr && !id_ptr->IsNull()) {
        v.id = RequestIdFromJson(*id_ptr);
    }
    v.error = DeserializeErrorData(j[detail::kError]);
    return v;
}

std::string SerializeMessage(const JsonRpcMessage& msg) {
    JsonValue jv = std::visit([](const auto& m) -> JsonValue {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, JsonRpcRequest>)
            return SerializeJsonRpcRequest(m);
        else if constexpr (std::is_same_v<T, JsonRpcNotification>)
            return SerializeJsonRpcNotification(m);
        else if constexpr (std::is_same_v<T, JsonRpcResponse>)
            return SerializeJsonRpcResponse(m);
        else
            return SerializeJsonRpcErrorResponse(m);
    }, msg);
    return jv.Dump();
}

std::string SerializeMessage(JsonRpcMessage&& msg) {
    JsonValue jv = std::visit([](auto&& m) -> JsonValue {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, JsonRpcRequest>)
            return SerializeJsonRpcRequest(std::move(m));
        else if constexpr (std::is_same_v<T, JsonRpcNotification>)
            return SerializeJsonRpcNotification(std::move(m));
        else if constexpr (std::is_same_v<T, JsonRpcResponse>)
            return SerializeJsonRpcResponse(std::move(m));
        else
            return SerializeJsonRpcErrorResponse(std::move(m));
    }, std::move(msg));
    return jv.Dump();
}

JsonRpcMessage DeserializeMessage(std::string_view json) {
    auto jv = JsonValue::Parse(json);

    bool has_method = jv.Contains(detail::kMethod);
    bool has_result = jv.Contains(detail::kResult);
    bool has_error = jv.Contains(detail::kError);
    auto* id_ptr = jv.Find(detail::kId);
    bool has_id = id_ptr && !id_ptr->IsNull();

    if (has_method && has_id) {
        return DeserializeJsonRpcRequest(jv);
    } else if (has_method) {
        return DeserializeJsonRpcNotification(jv);
    } else if (has_result && !has_error) {
        return DeserializeJsonRpcResponse(jv);
    } else if (has_error && !has_result) {
        return DeserializeJsonRpcErrorResponse(jv);
    } else if (has_result && has_error) {
        throw McpError(McpErrorCode::InvalidRequest,
            "JSON-RPC message has both 'result' and 'error' fields");
    } else {
        throw McpError(McpErrorCode::InvalidRequest,
            std::string("unknown JSON-RPC message type: method=") +
            (has_method ? "present" : "absent") +
            ", result=" + (has_result ? "present" : "absent") +
            ", error=" + (has_error ? "present" : "absent"));
    }
}

} // namespace mcp
