#include <mcp/McpTypes.hpp>
#include <detail/JsonFields.hpp>
#include <detail/JsonSerializer.hpp>

namespace mcp {

JsonValue SerializeGetTaskResult(const GetTaskResult& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kTaskId] = JsonValue(v.task_id);
    obj["status"] = JsonValue(v.status);
    obj[detail::kResultType] = JsonValue(v.task_result_type);
    detail::SerializeOptional(obj, detail::kResult, v.result);
    detail::SerializeOptional(obj, "errorMessage", v.error_message);
    detail::SerializeOptional(obj, "inputRequired", v.input_required);
    detail::SerializeOptional(obj, detail::kStatusMessage, v.status_message);
    detail::SerializeOptional(obj, detail::kCreatedAt, v.created_at);
    detail::SerializeOptional(obj, detail::kLastUpdatedAt, v.last_updated_at);
    detail::SerializeOptional(obj, detail::kTTLMs, v.ttl_ms);
    detail::SerializeOptional(obj, detail::kPollIntervalMs, v.poll_interval_ms);
    if (v.input_requests) obj[detail::kInputRequests] = SerializeInputRequests(*v.input_requests);
    detail::SerializeOptional(obj, detail::kError, v.error);
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
    detail::DeserializeOptional(j, detail::kStatusMessage, v.status_message);
    detail::DeserializeOptional(j, detail::kCreatedAt, v.created_at);
    detail::DeserializeOptional(j, detail::kLastUpdatedAt, v.last_updated_at);
    detail::DeserializeOptional(j, detail::kTTLMs, v.ttl_ms);
    detail::DeserializeOptional(j, detail::kPollIntervalMs, v.poll_interval_ms);
    if (auto* ir = j.Find(detail::kInputRequests); ir && ir->IsObject())
        v.input_requests = DeserializeInputRequests(*ir);
    detail::DeserializeOptional(j, detail::kError, v.error);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    return v;
}

JsonValue SerializeCreateTaskResult(const CreateTaskResult& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kResultType] = JsonValue(detail::kTaskValue);
    obj[detail::kTaskId] = JsonValue(v.task_id);
    obj[detail::kStatus] = JsonValue(v.status);
    detail::SerializeOptional(obj, detail::kStatusMessage, v.status_message);
    if (!v.created_at.empty()) obj[detail::kCreatedAt] = JsonValue(v.created_at);
    detail::SerializeOptional(obj, detail::kLastUpdatedAt, v.last_updated_at);
    detail::SerializeOptional(obj, detail::kTTLMs, v.ttl_ms);
    detail::SerializeOptional(obj, detail::kPollIntervalMs, v.poll_interval_ms);
    if (v.input_requests) obj[detail::kInputRequests] = SerializeInputRequests(*v.input_requests);
    JsonValue task(JsonValue::object_tag);
    task[detail::kTaskId] = JsonValue(v.task_id);
    task[detail::kStatus] = JsonValue(v.status);
    task[detail::kCreatedAt] = JsonValue(v.created_at);
    obj[detail::kTask] = std::move(task);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

CreateTaskResult DeserializeCreateTaskResult(const JsonValue& j) {
    CreateTaskResult v;
    if (auto* id = j.Find(detail::kTaskId); id && id->IsString())
        v.task_id = id->GetString();
    if (auto* st = j.Find(detail::kStatus); st && st->IsString())
        v.status = st->GetString();
    if (auto* ca = j.Find(detail::kCreatedAt); ca && ca->IsString())
        v.created_at = ca->GetString();
    detail::DeserializeOptional(j, detail::kStatusMessage, v.status_message);
    detail::DeserializeOptional(j, detail::kLastUpdatedAt, v.last_updated_at);
    detail::DeserializeOptional(j, detail::kTTLMs, v.ttl_ms);
    detail::DeserializeOptional(j, detail::kPollIntervalMs, v.poll_interval_ms);
    if (auto* ir = j.Find(detail::kInputRequests); ir && ir->IsObject())
        v.input_requests = DeserializeInputRequests(*ir);
    if (auto* task = j.Find(detail::kTask); task && task->IsObject()) {
        if (v.task_id.empty()) {
            if (auto* id = task->Find(detail::kTaskId); id && id->IsString())
                v.task_id = id->GetString();
        }
        if (v.status.empty()) {
            if (auto* st = task->Find(detail::kStatus); st && st->IsString())
                v.status = st->GetString();
        }
        if (v.created_at.empty()) {
            if (auto* ca = task->Find(detail::kCreatedAt); ca && ca->IsString())
                v.created_at = ca->GetString();
        }
    }
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    auto* rt = j.Find(detail::kResultType);
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

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

JsonValue SerializeUpdateTaskRequestParams(const UpdateTaskRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kTaskId] = JsonValue(v.task_id);
    detail::SerializeOptional(obj, detail::kResult, v.result);
    detail::SerializeOptional(obj, detail::kInputResponses, v.input_responses);
    return obj;
}

UpdateTaskRequestParams DeserializeUpdateTaskRequestParams(const JsonValue& j) {
    UpdateTaskRequestParams v;
    v.task_id = j[detail::kTaskId].GetString();
    detail::DeserializeOptional(j, detail::kResult, v.result);
    detail::DeserializeOptional(j, detail::kInputResponses, v.input_responses);
    return v;
}

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
