// McpTypesTasks.cpp — Task params and result serialization

#include <mcp/McpTypes.hpp>
#include <detail/JsonFields.hpp>
#include <detail/JsonSerializer.hpp>

namespace mcp {

// ── GetTaskResult ──

JsonValue SerializeGetTaskResult(const GetTaskResult& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kTaskId] = JsonValue(v.task_id);
    obj["status"] = JsonValue(v.status);
    obj[detail::kResultType] = JsonValue(v.task_result_type);
    detail::SerializeOptional(obj, detail::kResult, v.result);
    detail::SerializeOptional(obj, "errorMessage", v.error_message);
    detail::SerializeOptional(obj, "inputRequired", v.input_required);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

GetTaskResult DeserializeGetTaskResult(const JsonValue& j) {
    GetTaskResult v;
    v.task_id = j[detail::kTaskId].GetString();
    v.status = j["status"].GetString();
    auto* rt = j.Find(detail::kResultType);
    if (rt && rt->IsString()) v.task_result_type = rt->GetString();
    detail::DeserializeOptional(j, detail::kResult, v.result);
    detail::DeserializeOptional(j, "errorMessage", v.error_message);
    detail::DeserializeOptional(j, "inputRequired", v.input_required);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    return v;
}

// ── GetTaskRequestParams ──

JsonValue SerializeGetTaskRequestParams(const GetTaskRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kTaskId] = JsonValue(v.task_id);
    return obj;
}

GetTaskRequestParams DeserializeGetTaskRequestParams(const JsonValue& j) {
    GetTaskRequestParams v;
    v.task_id = j[detail::kTaskId].GetString();
    return v;
}

// ── UpdateTaskRequestParams ──

JsonValue SerializeUpdateTaskRequestParams(const UpdateTaskRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kTaskId] = JsonValue(v.task_id);
    detail::SerializeOptional(obj, detail::kResult, v.result);
    return obj;
}

UpdateTaskRequestParams DeserializeUpdateTaskRequestParams(const JsonValue& j) {
    UpdateTaskRequestParams v;
    v.task_id = j[detail::kTaskId].GetString();
    detail::DeserializeOptional(j, detail::kResult, v.result);
    return v;
}

// ── CancelTaskRequestParams ──

JsonValue SerializeCancelTaskRequestParams(const CancelTaskRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kTaskId] = JsonValue(v.task_id);
    detail::SerializeOptional(obj, detail::kReason, v.reason);
    return obj;
}

CancelTaskRequestParams DeserializeCancelTaskRequestParams(const JsonValue& j) {
    CancelTaskRequestParams v;
    v.task_id = j[detail::kTaskId].GetString();
    detail::DeserializeOptional(j, detail::kReason, v.reason);
    return v;
}

} // namespace mcp
