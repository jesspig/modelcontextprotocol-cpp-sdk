#include <mcp/JsonRpc.hpp>
#include <detail/JsonSerializer.hpp>

#include <stdexcept>
#include <string>
#include <type_traits>

namespace mcp {

namespace {

void ValidateVersion(std::string_view ver) {
    if (ver != "2.0")
        throw std::runtime_error(std::string("invalid JSON-RPC version: ") + std::string(ver));
}

} // namespace

JsonValue RequestIdToJson(const RequestId& id) {
    return std::visit([](const auto& id_val) -> JsonValue {
        using T = std::decay_t<decltype(id_val)>;
        if constexpr (std::is_same_v<T, int64_t>) {
            return JsonValue(id_val);
        } else {
            return JsonValue(id_val);
        }
    }, id);
}

RequestId RequestIdFromJson(const JsonValue& j) {
    if (j.IsInt()) return j.GetInt();
    return j.GetString();
}

JsonValue SerializeErrorData(const ErrorData& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["code"] = static_cast<int64_t>(v.code);
    obj["message"] = JsonValue(v.message);
    detail::SerializeOptional(obj, "data", v.data);
    return obj;
}

ErrorData DeserializeErrorData(const JsonValue& j) {
    ErrorData v;
    v.code = static_cast<McpErrorCode>(j["code"].GetInt());
    v.message = j["message"].GetString();
    detail::DeserializeOptional(j, "data", v.data);
    return v;
}

JsonValue SerializeJsonRpcRequest(const JsonRpcRequest& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["jsonrpc"] = "2.0";
    obj["id"] = RequestIdToJson(v.id);
    obj["method"] = JsonValue(v.method);
    detail::SerializeOptional(obj, "params", v.params);
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

JsonRpcRequest DeserializeJsonRpcRequest(const JsonValue& j) {
    JsonRpcRequest v;
    v.jsonrpc = j["jsonrpc"].GetString();
    ValidateVersion(v.jsonrpc);
    v.id = RequestIdFromJson(j["id"]);
    v.method = j["method"].GetString();
    detail::DeserializeOptional(j, "params", v.params);
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

JsonValue SerializeJsonRpcNotification(const JsonRpcNotification& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["jsonrpc"] = "2.0";
    obj["method"] = JsonValue(v.method);
    detail::SerializeOptional(obj, "params", v.params);
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

JsonRpcNotification DeserializeJsonRpcNotification(const JsonValue& j) {
    JsonRpcNotification v;
    v.jsonrpc = j["jsonrpc"].GetString();
    ValidateVersion(v.jsonrpc);
    v.method = j["method"].GetString();
    detail::DeserializeOptional(j, "params", v.params);
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

JsonValue SerializeJsonRpcResponse(const JsonRpcResponse& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["jsonrpc"] = "2.0";
    obj["id"] = RequestIdToJson(v.id);
    obj["result"] = v.result;
    return obj;
}

JsonRpcResponse DeserializeJsonRpcResponse(const JsonValue& j) {
    JsonRpcResponse v;
    v.jsonrpc = j["jsonrpc"].GetString();
    ValidateVersion(v.jsonrpc);
    v.id = RequestIdFromJson(j["id"]);
    v.result = j["result"];
    return v;
}

JsonValue SerializeJsonRpcErrorResponse(const JsonRpcErrorResponse& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["jsonrpc"] = "2.0";
    if (v.id) {
        obj["id"] = RequestIdToJson(*v.id);
    } else {
        obj["id"] = JsonValue(nullptr);
    }
    obj["error"] = SerializeErrorData(v.error);
    return obj;
}

JsonRpcErrorResponse DeserializeJsonRpcErrorResponse(const JsonValue& j) {
    JsonRpcErrorResponse v;
    v.jsonrpc = j["jsonrpc"].GetString();
    ValidateVersion(v.jsonrpc);
    auto* id_ptr = j.Find("id");
    if (id_ptr && !id_ptr->IsNull()) {
        v.id = RequestIdFromJson(*id_ptr);
    }
    v.error = DeserializeErrorData(j["error"]);
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

JsonRpcMessage DeserializeMessage(std::string_view json) {
    auto jv = JsonValue::Parse(json);

    bool has_method = jv.Contains("method");
    bool has_id = jv.Contains("id");
    bool has_result = jv.Contains("result");
    bool has_error = jv.Contains("error");

    if (has_method && has_id) {
        return DeserializeJsonRpcRequest(jv);
    } else if (has_method) {
        return DeserializeJsonRpcNotification(jv);
    } else if (has_result && !has_error) {
        return DeserializeJsonRpcResponse(jv);
    } else if (has_error && !has_result) {
        return DeserializeJsonRpcErrorResponse(jv);
    } else if (has_result && has_error) {
        throw std::runtime_error("JSON-RPC message has both result and error");
    } else {
        throw std::runtime_error("unknown JSON-RPC message type");
    }
}

} // namespace mcp
