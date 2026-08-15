// McpTypesNotifications.cpp — Notification params serialization

#include <mcp/McpTypes.hpp>
#include <detail/JsonFields.hpp>
#include <detail/JsonSerializer.hpp>

namespace mcp {

// ── SubscriptionFilter ──

JsonValue SerializeSubscriptionFilter(const SubscriptionFilter& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "toolsListChanged", v.tools_list_changed);
    detail::SerializeOptional(obj, "promptsListChanged", v.prompts_list_changed);
    detail::SerializeOptional(obj, "resourcesListChanged", v.resources_list_changed);
    if (!v.resource_subscriptions.empty()) {
        JsonValue::Array arr;
        for (const auto& s : v.resource_subscriptions) arr.push_back(JsonValue(s));
        obj["resourceSubscriptions"] = JsonValue(std::move(arr));
    }
    return obj;
}

SubscriptionFilter DeserializeSubscriptionFilter(const JsonValue& j) {
    SubscriptionFilter v;
    detail::DeserializeOptional(j, "toolsListChanged", v.tools_list_changed);
    detail::DeserializeOptional(j, "promptsListChanged", v.prompts_list_changed);
    detail::DeserializeOptional(j, "resourcesListChanged", v.resources_list_changed);
    auto* subs = j.Find("resourceSubscriptions");
    if (subs && subs->IsArray()) {
        std::vector<std::string> vec;
        for (const auto& sv : subs->GetArray()) vec.push_back(sv.GetString());
        v.resource_subscriptions = std::move(vec);
    }
    return v;
}

// ── SubscriptionsListenRequestParams ──

JsonValue SerializeSubscriptionsListenRequestParams(const SubscriptionsListenRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kNotifications] = SerializeSubscriptionFilter(v.notifications);
    if (v.meta) obj[detail::kMeta] = SerializeRequestMeta(*v.meta);
    return obj;
}

SubscriptionsListenRequestParams DeserializeSubscriptionsListenRequestParams(const JsonValue& j) {
    SubscriptionsListenRequestParams v;
    v.notifications = DeserializeSubscriptionFilter(j[detail::kNotifications]);
    auto* m = j.Find(detail::kMeta);
    if (m) v.meta = DeserializeRequestMeta(*m);
    return v;
}

// ── SubscriptionsAcknowledgedNotificationParams ──

JsonValue SerializeSubscriptionsAcknowledgedNotificationParams(const SubscriptionsAcknowledgedNotificationParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kNotifications] = SerializeSubscriptionFilter(v.notifications);
    return obj;
}

SubscriptionsAcknowledgedNotificationParams DeserializeSubscriptionsAcknowledgedNotificationParams(const JsonValue& j) {
    SubscriptionsAcknowledgedNotificationParams v;
    v.notifications = DeserializeSubscriptionFilter(j[detail::kNotifications]);
    return v;
}

// ── ProgressNotificationParams ──

JsonValue SerializeProgressNotificationParams(const ProgressNotificationParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kProgressToken] = SerializeProgressToken(v.progress_token);
    obj["progress"] = JsonValue(v.progress);
    detail::SerializeOptional(obj, "total", v.total);
    detail::SerializeOptional(obj, detail::kMessage, v.message);
    return obj;
}

ProgressNotificationParams DeserializeProgressNotificationParams(const JsonValue& j) {
    ProgressNotificationParams v;
    v.progress_token = DeserializeProgressToken(j[detail::kProgressToken]);
    const JsonValue& progress_val = j["progress"];
    if (!progress_val.IsNumber())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("ProgressNotificationParams: field 'progress' expected number, got ") +
            detail::JsonValueTypeName(progress_val));
    v.progress = progress_val.IsDouble() ? progress_val.GetDouble() : static_cast<double>(progress_val.GetInt());
    detail::DeserializeOptional(j, "total", v.total);
    detail::DeserializeOptional(j, detail::kMessage, v.message);
    return v;
}

// ── CancelledNotificationParams ──

JsonValue SerializeCancelledNotificationParams(const CancelledNotificationParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["requestId"] = RequestIdToJson(v.request_id);
    detail::SerializeOptional(obj, detail::kReason, v.reason);
    return obj;
}

CancelledNotificationParams DeserializeCancelledNotificationParams(const JsonValue& j) {
    CancelledNotificationParams v;
    v.request_id = RequestIdFromJson(j["requestId"]);
    detail::DeserializeOptional(j, detail::kReason, v.reason);
    return v;
}

// ── LoggingMessageNotificationParams ──

JsonValue SerializeLoggingMessageNotificationParams(const LoggingMessageNotificationParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kLevel] = SerializeLoggingLevel(v.level);
    detail::SerializeOptional(obj, "logger", v.logger);
    obj[detail::kData] = v.data;
    return obj;
}

LoggingMessageNotificationParams DeserializeLoggingMessageNotificationParams(const JsonValue& j) {
    LoggingMessageNotificationParams v;
    v.level = DeserializeLoggingLevel(j[detail::kLevel]);
    detail::DeserializeOptional(j, "logger", v.logger);
    v.data = j[detail::kData];
    return v;
}

// ── TaskStatusNotificationParams ──

JsonValue SerializeTaskStatusNotificationParams(const TaskStatusNotificationParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kTaskId] = JsonValue(v.task_id);
    obj[detail::kStatus] = JsonValue(v.status);
    return obj;
}

} // namespace mcp
