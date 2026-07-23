#include <mcp/McpTypes.hpp>
#include <detail/JsonSerializer.hpp>

namespace mcp {

// ── Forward declarations from other modules ──

JsonValue SerializeIcon(const Icon& v);
Icon DeserializeIcon(const JsonValue& j);
JsonValue SerializeContentVariant(const ContentVariant& v);
ContentVariant DeserializeContentVariant(const JsonValue& j);
JsonValue SerializeResourceContents(const ResourceContents& v);
ResourceContents DeserializeResourceContents(const JsonValue& j);

JsonValue SerializeServerCapabilities(const ServerCapabilities& v);
ServerCapabilities DeserializeServerCapabilities(const JsonValue& j);
JsonValue SerializeClientCapabilities(const ClientCapabilities& v);
ClientCapabilities DeserializeClientCapabilities(const JsonValue& j);

JsonValue SerializeImplementation(const Implementation& v);
Implementation DeserializeImplementation(const JsonValue& j);

JsonValue SerializeCacheHint(const CacheHint& v);
CacheHint DeserializeCacheHint(const JsonValue& j);
JsonValue SerializeRequestMeta(const RequestMeta& v);
RequestMeta DeserializeRequestMeta(const JsonValue& j);

// ── Internal helpers ──

namespace {

JsonValue SerializeLoggingLevelValue(LoggingLevel l) {
    static const char* names[] = {"debug", "info", "notice", "warning",
                                   "error", "critical", "alert", "emergency"};
    auto i = static_cast<int>(l);
    return JsonValue(i >= 0 && i < 8 ? names[i] : "debug");
}

LoggingLevel DeserializeLoggingLevelValue(const JsonValue& j) {
    auto s = j.GetString();
    struct Entry { const char* name; LoggingLevel level; };
    static const Entry map[] = {
        {"debug", LoggingLevel::Debug}, {"info", LoggingLevel::Info},
        {"notice", LoggingLevel::Notice}, {"warning", LoggingLevel::Warning},
        {"error", LoggingLevel::Error}, {"critical", LoggingLevel::Critical},
        {"alert", LoggingLevel::Alert}, {"emergency", LoggingLevel::Emergency},
    };
    for (const auto& e : map) {
        if (s == e.name) return e.level;
    }
    return LoggingLevel::Debug;
}

void SerializeProgressTokenValue(const ProgressToken& pt, const char* key, JsonValue& obj) {
    if (std::holds_alternative<std::string>(pt)) {
        obj[key] = JsonValue(std::get<std::string>(pt));
    } else {
        obj[key] = JsonValue(std::get<int64_t>(pt));
    }
}

ProgressToken DeserializeProgressTokenValue(const JsonValue& j) {
    if (j.IsString()) return std::string(j.GetString());
    return j.GetInt();
}

void SerializeRequestIdValue(const RequestId& id, const char* key, JsonValue& obj) {
    if (std::holds_alternative<std::string>(id)) {
        obj[key] = JsonValue(std::get<std::string>(id));
    } else {
        obj[key] = JsonValue(std::get<int64_t>(id));
    }
}

RequestId DeserializeRequestIdValue(const JsonValue& j) {
    if (j.IsString()) return std::string(j.GetString());
    return j.GetInt();
}

} // anonymous namespace

// ── ResultType enum ──

JsonValue SerializeResultType(ResultType v) {
    switch (v) {
        case ResultType::Complete:     return JsonValue("complete");
        case ResultType::InputRequired: return JsonValue("input_required");
    }
    return JsonValue("complete");
}

ResultType DeserializeResultType(const JsonValue& j) {
    auto s = j.GetString();
    if (s == "input_required") return ResultType::InputRequired;
    return ResultType::Complete;
}

// ── ToolAnnotations ──

JsonValue SerializeToolAnnotations(const ToolAnnotations& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "title", v.title);
    detail::SerializeOptional(obj, "readOnlyHint", v.read_only_hint);
    detail::SerializeOptional(obj, "idempotentHint", v.idempotent_hint);
    detail::SerializeOptional(obj, "openWorldHint", v.open_world_hint);
    detail::SerializeOptional(obj, "destructiveHint", v.destructive_hint);
    return obj;
}

ToolAnnotations DeserializeToolAnnotations(const JsonValue& j) {
    ToolAnnotations v;
    detail::DeserializeOptional(j, "title", v.title);
    detail::DeserializeOptional(j, "readOnlyHint", v.read_only_hint);
    detail::DeserializeOptional(j, "idempotentHint", v.idempotent_hint);
    detail::DeserializeOptional(j, "openWorldHint", v.open_world_hint);
    detail::DeserializeOptional(j, "destructiveHint", v.destructive_hint);
    return v;
}

// ── ResourceAnnotations ──

JsonValue SerializeResourceAnnotations(const ResourceAnnotations& v) {
    JsonValue obj(JsonValue::object_tag);
    if (v.audience && !v.audience->empty()) {
        JsonValue::Array arr;
        for (const auto& s : *v.audience) arr.push_back(JsonValue(s));
        obj["audience"] = JsonValue(std::move(arr));
    }
    detail::SerializeOptional(obj, "priority", v.priority);
    detail::SerializeOptional(obj, "lastModified", v.last_modified);
    return obj;
}

ResourceAnnotations DeserializeResourceAnnotations(const JsonValue& j) {
    ResourceAnnotations v;
    auto* aud = j.Find("audience");
    if (aud && aud->IsArray()) {
        std::vector<std::string> vec;
        for (const auto& av : aud->GetArray()) vec.push_back(av.GetString());
        v.audience = std::move(vec);
    }
    detail::DeserializeOptional(j, "priority", v.priority);
    detail::DeserializeOptional(j, "lastModified", v.last_modified);
    return v;
}

// ── Result (base) ──

JsonValue SerializeResult(const Result& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "_meta", v.meta);
    obj["resultType"] = SerializeResultType(v.result_type);
    return obj;
}

Result DeserializeResult(const JsonValue& j) {
    Result v;
    detail::DeserializeOptional(j, "_meta", v.meta);
    auto* rt = j.Find("resultType");
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

// ── EmptyResult ──

JsonValue SerializeEmptyResult(const EmptyResult& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "_meta", v.meta);
    obj["resultType"] = SerializeResultType(v.result_type);
    return obj;
}

EmptyResult DeserializeEmptyResult(const JsonValue& j) {
    EmptyResult v;
    detail::DeserializeOptional(j, "_meta", v.meta);
    auto* rt = j.Find("resultType");
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

// ── Tool ──

JsonValue SerializeTool(const Tool& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["name"] = JsonValue(v.name);
    detail::SerializeOptional(obj, "title", v.title);
    detail::SerializeOptional(obj, "description", v.description);
    obj["inputSchema"] = v.input_schema;
    detail::SerializeOptional(obj, "outputSchema", v.output_schema);
    if (v.annotations) obj["annotations"] = SerializeToolAnnotations(*v.annotations);
    if (!v.icons.empty()) {
        JsonValue::Array arr;
        for (const auto& icon : v.icons) arr.push_back(SerializeIcon(icon));
        obj["icons"] = JsonValue(std::move(arr));
    }
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

Tool DeserializeTool(const JsonValue& j) {
    Tool v;
    v.name = j["name"].GetString();
    detail::DeserializeOptional(j, "title", v.title);
    detail::DeserializeOptional(j, "description", v.description);
    v.input_schema = j["inputSchema"];
    detail::DeserializeOptional(j, "outputSchema", v.output_schema);
    auto* ann = j.Find("annotations");
    if (ann) v.annotations = DeserializeToolAnnotations(*ann);
    auto* icons = j.Find("icons");
    if (icons && icons->IsArray()) {
        std::vector<Icon> vec;
        for (const auto& ic : icons->GetArray()) vec.push_back(DeserializeIcon(ic));
        v.icons = std::move(vec);
    }
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

// ── Resource ──

JsonValue SerializeResource(const Resource& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["uri"] = JsonValue(v.uri);
    obj["name"] = JsonValue(v.name);
    detail::SerializeOptional(obj, "title", v.title);
    detail::SerializeOptional(obj, "description", v.description);
    detail::SerializeOptional(obj, "mimeType", v.mime_type);
    detail::SerializeOptional(obj, "size", v.size);
    if (v.icons && !v.icons->empty()) {
        JsonValue::Array arr;
        for (const auto& icon : *v.icons) arr.push_back(SerializeIcon(icon));
        obj["icons"] = JsonValue(std::move(arr));
    }
    if (v.annotations) obj["annotations"] = SerializeResourceAnnotations(*v.annotations);
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

Resource DeserializeResource(const JsonValue& j) {
    Resource v;
    v.uri = j["uri"].GetString();
    v.name = j["name"].GetString();
    detail::DeserializeOptional(j, "title", v.title);
    detail::DeserializeOptional(j, "description", v.description);
    detail::DeserializeOptional(j, "mimeType", v.mime_type);
    detail::DeserializeOptional(j, "size", v.size);
    auto* icons = j.Find("icons");
    if (icons && icons->IsArray()) {
        std::vector<Icon> vec;
        for (const auto& ic : icons->GetArray()) vec.push_back(DeserializeIcon(ic));
        v.icons = std::move(vec);
    }
    auto* ann = j.Find("annotations");
    if (ann) v.annotations = DeserializeResourceAnnotations(*ann);
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

// ── ResourceTemplate ──

JsonValue SerializeResourceTemplate(const ResourceTemplate& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["uriTemplate"] = JsonValue(v.uri_template);
    obj["name"] = JsonValue(v.name);
    detail::SerializeOptional(obj, "title", v.title);
    detail::SerializeOptional(obj, "description", v.description);
    detail::SerializeOptional(obj, "mimeType", v.mime_type);
    if (v.icons && !v.icons->empty()) {
        JsonValue::Array arr;
        for (const auto& icon : *v.icons) arr.push_back(SerializeIcon(icon));
        obj["icons"] = JsonValue(std::move(arr));
    }
    if (v.annotations) obj["annotations"] = SerializeResourceAnnotations(*v.annotations);
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

ResourceTemplate DeserializeResourceTemplate(const JsonValue& j) {
    ResourceTemplate v;
    v.uri_template = j["uriTemplate"].GetString();
    v.name = j["name"].GetString();
    detail::DeserializeOptional(j, "title", v.title);
    detail::DeserializeOptional(j, "description", v.description);
    detail::DeserializeOptional(j, "mimeType", v.mime_type);
    auto* icons = j.Find("icons");
    if (icons && icons->IsArray()) {
        std::vector<Icon> vec;
        for (const auto& ic : icons->GetArray()) vec.push_back(DeserializeIcon(ic));
        v.icons = std::move(vec);
    }
    auto* ann = j.Find("annotations");
    if (ann) v.annotations = DeserializeResourceAnnotations(*ann);
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

// ── PromptArgument ──

JsonValue SerializePromptArgument(const PromptArgument& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["name"] = JsonValue(v.name);
    detail::SerializeOptional(obj, "description", v.description);
    detail::SerializeOptional(obj, "required", v.required);
    return obj;
}

PromptArgument DeserializePromptArgument(const JsonValue& j) {
    PromptArgument v;
    v.name = j["name"].GetString();
    detail::DeserializeOptional(j, "description", v.description);
    detail::DeserializeOptional(j, "required", v.required);
    return v;
}

// ── Prompt ──

JsonValue SerializePrompt(const Prompt& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["name"] = JsonValue(v.name);
    detail::SerializeOptional(obj, "title", v.title);
    detail::SerializeOptional(obj, "description", v.description);
    if (v.arguments && !v.arguments->empty()) {
        JsonValue::Array arr;
        for (const auto& arg : *v.arguments) arr.push_back(SerializePromptArgument(arg));
        obj["arguments"] = JsonValue(std::move(arr));
    }
    if (v.icons && !v.icons->empty()) {
        JsonValue::Array arr;
        for (const auto& icon : *v.icons) arr.push_back(SerializeIcon(icon));
        obj["icons"] = JsonValue(std::move(arr));
    }
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

Prompt DeserializePrompt(const JsonValue& j) {
    Prompt v;
    v.name = j["name"].GetString();
    detail::DeserializeOptional(j, "title", v.title);
    detail::DeserializeOptional(j, "description", v.description);
    auto* args = j.Find("arguments");
    if (args && args->IsArray()) {
        std::vector<PromptArgument> vec;
        for (const auto& av : args->GetArray()) vec.push_back(DeserializePromptArgument(av));
        v.arguments = std::move(vec);
    }
    auto* icons = j.Find("icons");
    if (icons && icons->IsArray()) {
        std::vector<Icon> vec;
        for (const auto& ic : icons->GetArray()) vec.push_back(DeserializeIcon(ic));
        v.icons = std::move(vec);
    }
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

// ── PromptMessage ──

JsonValue SerializePromptMessage(const PromptMessage& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["role"] = JsonValue(v.role);
    obj["content"] = SerializeContentVariant(v.content);
    return obj;
}

PromptMessage DeserializePromptMessage(const JsonValue& j) {
    PromptMessage v;
    v.role = j["role"].GetString();
    v.content = DeserializeContentVariant(j["content"]);
    return v;
}

// ── Pagination ──

JsonValue SerializePagination(const Pagination& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "nextCursor", v.next_cursor);
    return obj;
}

Pagination DeserializePagination(const JsonValue& j) {
    Pagination v;
    detail::DeserializeOptional(j, "nextCursor", v.next_cursor);
    return v;
}

// ── PaginatedRequestParams ──

JsonValue SerializePaginatedRequestParams(const PaginatedRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "cursor", v.cursor);
    if (v.meta) obj["_meta"] = SerializeRequestMeta(*v.meta);
    return obj;
}

PaginatedRequestParams DeserializePaginatedRequestParams(const JsonValue& j) {
    PaginatedRequestParams v;
    detail::DeserializeOptional(j, "cursor", v.cursor);
    auto* m = j.Find("_meta");
    if (m) v.meta = DeserializeRequestMeta(*m);
    return v;
}

// ── ResourceRequestParams ──

JsonValue SerializeResourceRequestParams(const ResourceRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["uri"] = JsonValue(v.uri);
    if (v.meta) obj["_meta"] = SerializeRequestMeta(*v.meta);
    return obj;
}

ResourceRequestParams DeserializeResourceRequestParams(const JsonValue& j) {
    ResourceRequestParams v;
    v.uri = j["uri"].GetString();
    auto* m = j.Find("_meta");
    if (m) v.meta = DeserializeRequestMeta(*m);
    return v;
}

// ── CallToolRequestParams ──

JsonValue SerializeCallToolRequestParams(const CallToolRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["name"] = JsonValue(v.name);
    detail::SerializeOptional(obj, "arguments", v.arguments);
    if (v.meta) obj["_meta"] = SerializeRequestMeta(*v.meta);
    detail::SerializeOptional(obj, "inputResponses", v.input_responses);
    detail::SerializeOptional(obj, "requestState", v.request_state);
    return obj;
}

CallToolRequestParams DeserializeCallToolRequestParams(const JsonValue& j) {
    CallToolRequestParams v;
    v.name = j["name"].GetString();
    detail::DeserializeOptional(j, "arguments", v.arguments);
    auto* m = j.Find("_meta");
    if (m) v.meta = DeserializeRequestMeta(*m);
    detail::DeserializeOptional(j, "inputResponses", v.input_responses);
    detail::DeserializeOptional(j, "requestState", v.request_state);
    return v;
}

// ── GetPromptRequestParams ──

JsonValue SerializeGetPromptRequestParams(const GetPromptRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["name"] = JsonValue(v.name);
    detail::SerializeOptional(obj, "arguments", v.arguments);
    if (v.meta) obj["_meta"] = SerializeRequestMeta(*v.meta);
    return obj;
}

GetPromptRequestParams DeserializeGetPromptRequestParams(const JsonValue& j) {
    GetPromptRequestParams v;
    v.name = j["name"].GetString();
    detail::DeserializeOptional(j, "arguments", v.arguments);
    auto* m = j.Find("_meta");
    if (m) v.meta = DeserializeRequestMeta(*m);
    return v;
}

// ── CompleteRequestParams ──

JsonValue SerializeCompleteRequestParams(const CompleteRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["ref"] = v.ref;
    obj["argumentName"] = JsonValue(v.argument_name);
    obj["argumentValue"] = JsonValue(v.argument_value);
    if (v.meta) obj["_meta"] = SerializeRequestMeta(*v.meta);
    return obj;
}

CompleteRequestParams DeserializeCompleteRequestParams(const JsonValue& j) {
    CompleteRequestParams v;
    v.ref = j["ref"];
    v.argument_name = j["argumentName"].GetString();
    v.argument_value = j["argumentValue"].GetString();
    auto* m = j.Find("_meta");
    if (m) v.meta = DeserializeRequestMeta(*m);
    return v;
}

// ── DiscoverRequestParams ──

JsonValue SerializeDiscoverRequestParams(const DiscoverRequestParams&) {
    return JsonValue(JsonValue::object_tag);
}

DiscoverRequestParams DeserializeDiscoverRequestParams(const JsonValue&) {
    return DiscoverRequestParams{};
}

// ── InitializeRequestParams ──

JsonValue SerializeInitializeRequestParams(const InitializeRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["protocolVersion"] = JsonValue(v.protocol_version);
    obj["capabilities"] = SerializeClientCapabilities(v.capabilities);
    obj["clientInfo"] = SerializeImplementation(v.client_info);
    return obj;
}

InitializeRequestParams DeserializeInitializeRequestParams(const JsonValue& j) {
    InitializeRequestParams v;
    v.protocol_version = j["protocolVersion"].GetString();
    v.capabilities = DeserializeClientCapabilities(j["capabilities"]);
    v.client_info = DeserializeImplementation(j["clientInfo"]);
    return v;
}

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
    obj["notifications"] = SerializeSubscriptionFilter(v.notifications);
    if (v.meta) obj["_meta"] = SerializeRequestMeta(*v.meta);
    return obj;
}

SubscriptionsListenRequestParams DeserializeSubscriptionsListenRequestParams(const JsonValue& j) {
    SubscriptionsListenRequestParams v;
    v.notifications = DeserializeSubscriptionFilter(j["notifications"]);
    auto* m = j.Find("_meta");
    if (m) v.meta = DeserializeRequestMeta(*m);
    return v;
}

// ── SubscriptionsAcknowledgedNotificationParams ──

JsonValue SerializeSubscriptionsAcknowledgedNotificationParams(const SubscriptionsAcknowledgedNotificationParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["notifications"] = SerializeSubscriptionFilter(v.notifications);
    return obj;
}

SubscriptionsAcknowledgedNotificationParams DeserializeSubscriptionsAcknowledgedNotificationParams(const JsonValue& j) {
    SubscriptionsAcknowledgedNotificationParams v;
    v.notifications = DeserializeSubscriptionFilter(j["notifications"]);
    return v;
}

// ── CallToolResult ──

JsonValue SerializeCallToolResult(const CallToolResult& v) {
    JsonValue obj(JsonValue::object_tag);
    {
        JsonValue::Array arr;
        for (const auto& c : v.content) arr.push_back(SerializeContentVariant(c));
        obj["content"] = JsonValue(std::move(arr));
    }
    obj["isError"] = JsonValue(v.is_error);
    detail::SerializeOptional(obj, "structuredContent", v.structured_content);
    detail::SerializeOptional(obj, "_meta", v.meta);
    obj["resultType"] = SerializeResultType(v.result_type);
    return obj;
}

CallToolResult DeserializeCallToolResult(const JsonValue& j) {
    CallToolResult v;
    auto* content = j.Find("content");
    if (content && content->IsArray()) {
        std::vector<ContentVariant> vec;
        for (const auto& cv : content->GetArray()) vec.push_back(DeserializeContentVariant(cv));
        v.content = std::move(vec);
    }
    auto* isErr = j.Find("isError");
    if (isErr && isErr->IsBool()) v.is_error = isErr->GetBool();
    detail::DeserializeOptional(j, "structuredContent", v.structured_content);
    detail::DeserializeOptional(j, "_meta", v.meta);
    auto* rt = j.Find("resultType");
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

// ── ListToolsResult ──

JsonValue SerializeListToolsResult(const ListToolsResult& v) {
    JsonValue obj(JsonValue::object_tag);
    {
        JsonValue::Array arr;
        for (const auto& t : v.tools) arr.push_back(SerializeTool(t));
        obj["tools"] = JsonValue(std::move(arr));
    }
    detail::SerializeOptional(obj, "nextCursor", v.next_cursor);
    if (v.cache_hint) obj["cacheHint"] = SerializeCacheHint(*v.cache_hint);
    detail::SerializeOptional(obj, "_meta", v.meta);
    obj["resultType"] = SerializeResultType(v.result_type);
    return obj;
}

ListToolsResult DeserializeListToolsResult(const JsonValue& j) {
    ListToolsResult v;
    auto* tools = j.Find("tools");
    if (tools && tools->IsArray()) {
        std::vector<Tool> vec;
        for (const auto& tv : tools->GetArray()) vec.push_back(DeserializeTool(tv));
        v.tools = std::move(vec);
    }
    detail::DeserializeOptional(j, "nextCursor", v.next_cursor);
    auto* ch = j.Find("cacheHint");
    if (ch) v.cache_hint = DeserializeCacheHint(*ch);
    detail::DeserializeOptional(j, "_meta", v.meta);
    auto* rt = j.Find("resultType");
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

// ── ListResourcesResult ──

JsonValue SerializeListResourcesResult(const ListResourcesResult& v) {
    JsonValue obj(JsonValue::object_tag);
    {
        JsonValue::Array arr;
        for (const auto& r : v.resources) arr.push_back(SerializeResource(r));
        obj["resources"] = JsonValue(std::move(arr));
    }
    detail::SerializeOptional(obj, "nextCursor", v.next_cursor);
    if (v.cache_hint) obj["cacheHint"] = SerializeCacheHint(*v.cache_hint);
    detail::SerializeOptional(obj, "_meta", v.meta);
    obj["resultType"] = SerializeResultType(v.result_type);
    return obj;
}

ListResourcesResult DeserializeListResourcesResult(const JsonValue& j) {
    ListResourcesResult v;
    auto* resources = j.Find("resources");
    if (resources && resources->IsArray()) {
        std::vector<Resource> vec;
        for (const auto& rv : resources->GetArray()) vec.push_back(DeserializeResource(rv));
        v.resources = std::move(vec);
    }
    detail::DeserializeOptional(j, "nextCursor", v.next_cursor);
    auto* ch = j.Find("cacheHint");
    if (ch) v.cache_hint = DeserializeCacheHint(*ch);
    detail::DeserializeOptional(j, "_meta", v.meta);
    auto* rt = j.Find("resultType");
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

// ── ListResourceTemplatesResult ──

JsonValue SerializeListResourceTemplatesResult(const ListResourceTemplatesResult& v) {
    JsonValue obj(JsonValue::object_tag);
    {
        JsonValue::Array arr;
        for (const auto& rt : v.resource_templates) arr.push_back(SerializeResourceTemplate(rt));
        obj["resourceTemplates"] = JsonValue(std::move(arr));
    }
    detail::SerializeOptional(obj, "nextCursor", v.next_cursor);
    if (v.cache_hint) obj["cacheHint"] = SerializeCacheHint(*v.cache_hint);
    detail::SerializeOptional(obj, "_meta", v.meta);
    obj["resultType"] = SerializeResultType(v.result_type);
    return obj;
}

ListResourceTemplatesResult DeserializeListResourceTemplatesResult(const JsonValue& j) {
    ListResourceTemplatesResult v;
    auto* templates = j.Find("resourceTemplates");
    if (templates && templates->IsArray()) {
        std::vector<ResourceTemplate> vec;
        for (const auto& rtv : templates->GetArray()) vec.push_back(DeserializeResourceTemplate(rtv));
        v.resource_templates = std::move(vec);
    }
    detail::DeserializeOptional(j, "nextCursor", v.next_cursor);
    auto* ch = j.Find("cacheHint");
    if (ch) v.cache_hint = DeserializeCacheHint(*ch);
    detail::DeserializeOptional(j, "_meta", v.meta);
    auto* rt = j.Find("resultType");
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

// ── ReadResourceResult ──

JsonValue SerializeReadResourceResult(const ReadResourceResult& v) {
    JsonValue obj(JsonValue::object_tag);
    {
        JsonValue::Array arr;
        for (const auto& rc : v.contents) arr.push_back(SerializeResourceContents(rc));
        obj["contents"] = JsonValue(std::move(arr));
    }
    if (v.cache_hint) obj["cacheHint"] = SerializeCacheHint(*v.cache_hint);
    detail::SerializeOptional(obj, "_meta", v.meta);
    obj["resultType"] = SerializeResultType(v.result_type);
    return obj;
}

ReadResourceResult DeserializeReadResourceResult(const JsonValue& j) {
    ReadResourceResult v;
    auto* contents = j.Find("contents");
    if (contents && contents->IsArray()) {
        std::vector<ResourceContents> vec;
        for (const auto& rcv : contents->GetArray()) vec.push_back(DeserializeResourceContents(rcv));
        v.contents = std::move(vec);
    }
    auto* ch = j.Find("cacheHint");
    if (ch) v.cache_hint = DeserializeCacheHint(*ch);
    detail::DeserializeOptional(j, "_meta", v.meta);
    auto* rt = j.Find("resultType");
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

// ── ListPromptsResult ──

JsonValue SerializeListPromptsResult(const ListPromptsResult& v) {
    JsonValue obj(JsonValue::object_tag);
    {
        JsonValue::Array arr;
        for (const auto& p : v.prompts) arr.push_back(SerializePrompt(p));
        obj["prompts"] = JsonValue(std::move(arr));
    }
    detail::SerializeOptional(obj, "nextCursor", v.next_cursor);
    if (v.cache_hint) obj["cacheHint"] = SerializeCacheHint(*v.cache_hint);
    detail::SerializeOptional(obj, "_meta", v.meta);
    obj["resultType"] = SerializeResultType(v.result_type);
    return obj;
}

ListPromptsResult DeserializeListPromptsResult(const JsonValue& j) {
    ListPromptsResult v;
    auto* prompts = j.Find("prompts");
    if (prompts && prompts->IsArray()) {
        std::vector<Prompt> vec;
        for (const auto& pv : prompts->GetArray()) vec.push_back(DeserializePrompt(pv));
        v.prompts = std::move(vec);
    }
    detail::DeserializeOptional(j, "nextCursor", v.next_cursor);
    auto* ch = j.Find("cacheHint");
    if (ch) v.cache_hint = DeserializeCacheHint(*ch);
    detail::DeserializeOptional(j, "_meta", v.meta);
    auto* rt = j.Find("resultType");
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

// ── GetPromptResult ──

JsonValue SerializeGetPromptResult(const GetPromptResult& v) {
    JsonValue obj(JsonValue::object_tag);
    {
        JsonValue::Array arr;
        for (const auto& msg : v.messages) arr.push_back(SerializePromptMessage(msg));
        obj["messages"] = JsonValue(std::move(arr));
    }
    detail::SerializeOptional(obj, "description", v.description);
    detail::SerializeOptional(obj, "_meta", v.meta);
    obj["resultType"] = SerializeResultType(v.result_type);
    return obj;
}

GetPromptResult DeserializeGetPromptResult(const JsonValue& j) {
    GetPromptResult v;
    auto* msgs = j.Find("messages");
    if (msgs && msgs->IsArray()) {
        std::vector<PromptMessage> vec;
        for (const auto& mv : msgs->GetArray()) vec.push_back(DeserializePromptMessage(mv));
        v.messages = std::move(vec);
    }
    detail::DeserializeOptional(j, "description", v.description);
    detail::DeserializeOptional(j, "_meta", v.meta);
    auto* rt = j.Find("resultType");
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

// ── CompleteResult ──

JsonValue SerializeCompleteResult(const CompleteResult& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["completion"] = v.completion;
    detail::SerializeOptional(obj, "_meta", v.meta);
    obj["resultType"] = SerializeResultType(v.result_type);
    return obj;
}

CompleteResult DeserializeCompleteResult(const JsonValue& j) {
    CompleteResult v;
    v.completion = j["completion"];
    detail::DeserializeOptional(j, "_meta", v.meta);
    auto* rt = j.Find("resultType");
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

// ── InitializeResult ──

JsonValue SerializeInitializeResult(const InitializeResult& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["protocolVersion"] = JsonValue(v.protocol_version);
    obj["capabilities"] = SerializeServerCapabilities(v.capabilities);
    obj["serverInfo"] = SerializeImplementation(v.server_info);
    detail::SerializeOptional(obj, "instructions", v.instructions);
    detail::SerializeOptional(obj, "_meta", v.meta);
    obj["resultType"] = SerializeResultType(v.result_type);
    return obj;
}

InitializeResult DeserializeInitializeResult(const JsonValue& j) {
    InitializeResult v;
    v.protocol_version = j["protocolVersion"].GetString();
    v.capabilities = DeserializeServerCapabilities(j["capabilities"]);
    v.server_info = DeserializeImplementation(j["serverInfo"]);
    detail::DeserializeOptional(j, "instructions", v.instructions);
    detail::DeserializeOptional(j, "_meta", v.meta);
    auto* rt = j.Find("resultType");
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

// ── DiscoverResult ──

JsonValue SerializeDiscoverResult(const DiscoverResult& v) {
    JsonValue obj(JsonValue::object_tag);
    {
        JsonValue::Array arr;
        for (const auto& sv : v.supported_versions) arr.push_back(JsonValue(sv));
        obj["supportedVersions"] = JsonValue(std::move(arr));
    }
    obj["capabilities"] = SerializeServerCapabilities(v.capabilities);
    obj["serverInfo"] = SerializeImplementation(v.server_info);
    detail::SerializeOptional(obj, "instructions", v.instructions);
    if (v.cache_hint) obj["cacheHint"] = SerializeCacheHint(*v.cache_hint);
    detail::SerializeOptional(obj, "_meta", v.meta);
    obj["resultType"] = SerializeResultType(v.result_type);
    return obj;
}

DiscoverResult DeserializeDiscoverResult(const JsonValue& j) {
    DiscoverResult v;
    auto* sv = j.Find("supportedVersions");
    if (sv && sv->IsArray()) {
        std::vector<std::string> versions;
        for (const auto& ve : sv->GetArray()) versions.push_back(ve.GetString());
        v.supported_versions = std::move(versions);
    }
    v.capabilities = DeserializeServerCapabilities(j["capabilities"]);
    v.server_info = DeserializeImplementation(j["serverInfo"]);
    detail::DeserializeOptional(j, "instructions", v.instructions);
    auto* ch = j.Find("cacheHint");
    if (ch) v.cache_hint = DeserializeCacheHint(*ch);
    detail::DeserializeOptional(j, "_meta", v.meta);
    auto* rt = j.Find("resultType");
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

// ── InputRequestElicit ──

JsonValue SerializeInputRequestElicit(const InputRequestElicit& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["message"] = JsonValue(v.message);
    detail::SerializeOptional(obj, "requestedSchema", v.requested_schema);
    return obj;
}

InputRequestElicit DeserializeInputRequestElicit(const JsonValue& j) {
    InputRequestElicit v;
    v.message = j["message"].GetString();
    detail::DeserializeOptional(j, "requestedSchema", v.requested_schema);
    return v;
}

// ── InputRequests ──

JsonValue SerializeInputRequests(const InputRequests& v) {
    JsonValue obj(JsonValue::object_tag);
    if (v.confirm) obj["confirm"] = SerializeInputRequestElicit(*v.confirm);
    if (v.elicit) obj["elicit"] = SerializeInputRequestElicit(*v.elicit);
    return obj;
}

InputRequests DeserializeInputRequests(const JsonValue& j) {
    InputRequests v;
    auto* confirm = j.Find("confirm");
    if (confirm) v.confirm = DeserializeInputRequestElicit(*confirm);
    auto* elicit = j.Find("elicit");
    if (elicit) v.elicit = DeserializeInputRequestElicit(*elicit);
    return v;
}

// ── InputRequiredResult ──

JsonValue SerializeInputRequiredResult(const InputRequiredResult& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["inputRequests"] = SerializeInputRequests(v.input_requests);
    obj["resultType"] = JsonValue("input_required");
    detail::SerializeOptional(obj, "requestState", v.request_state);
    return obj;
}

InputRequiredResult DeserializeInputRequiredResult(const JsonValue& j) {
    InputRequiredResult v;
    v.input_requests = DeserializeInputRequests(j["inputRequests"]);
    detail::DeserializeOptional(j, "requestState", v.request_state);
    return v;
}

// ── ProgressNotificationParams ──

JsonValue SerializeProgressNotificationParams(const ProgressNotificationParams& v) {
    JsonValue obj(JsonValue::object_tag);
    SerializeProgressTokenValue(v.progress_token, "progressToken", obj);
    obj["progress"] = JsonValue(v.progress);
    detail::SerializeOptional(obj, "total", v.total);
    detail::SerializeOptional(obj, "message", v.message);
    return obj;
}

ProgressNotificationParams DeserializeProgressNotificationParams(const JsonValue& j) {
    ProgressNotificationParams v;
    v.progress_token = DeserializeProgressTokenValue(j["progressToken"]);
    v.progress = j["progress"].GetDouble();
    detail::DeserializeOptional(j, "total", v.total);
    detail::DeserializeOptional(j, "message", v.message);
    return v;
}

// ── CancelledNotificationParams ──

JsonValue SerializeCancelledNotificationParams(const CancelledNotificationParams& v) {
    JsonValue obj(JsonValue::object_tag);
    SerializeRequestIdValue(v.request_id, "requestId", obj);
    detail::SerializeOptional(obj, "reason", v.reason);
    return obj;
}

CancelledNotificationParams DeserializeCancelledNotificationParams(const JsonValue& j) {
    CancelledNotificationParams v;
    v.request_id = DeserializeRequestIdValue(j["requestId"]);
    detail::DeserializeOptional(j, "reason", v.reason);
    return v;
}

// ── LoggingMessageNotificationParams ──

JsonValue SerializeLoggingMessageNotificationParams(const LoggingMessageNotificationParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["level"] = SerializeLoggingLevelValue(v.level);
    obj["logger"] = JsonValue(v.logger);
    obj["data"] = v.data;
    return obj;
}

LoggingMessageNotificationParams DeserializeLoggingMessageNotificationParams(const JsonValue& j) {
    LoggingMessageNotificationParams v;
    v.level = DeserializeLoggingLevelValue(j["level"]);
    v.logger = j["logger"].GetString();
    v.data = j["data"];
    return v;
}

// ── ElicitRequestParams ──

JsonValue SerializeElicitRequestParams(const ElicitRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["message"] = JsonValue(v.message);
    detail::SerializeOptional(obj, "requestedSchema", v.requested_schema);
    return obj;
}

ElicitRequestParams DeserializeElicitRequestParams(const JsonValue& j) {
    ElicitRequestParams v;
    v.message = j["message"].GetString();
    detail::DeserializeOptional(j, "requestedSchema", v.requested_schema);
    return v;
}

// ── ElicitResult ──

JsonValue SerializeElicitResult(const ElicitResult& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "values", v.values);
    obj["resultType"] = SerializeResultType(v.result_type);
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

ElicitResult DeserializeElicitResult(const JsonValue& j) {
    ElicitResult v;
    detail::DeserializeOptional(j, "values", v.values);
    auto* rt = j.Find("resultType");
    if (rt) v.result_type = DeserializeResultType(*rt);
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

// ── SamplingMessage ──

JsonValue SerializeSamplingMessage(const SamplingMessage& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["role"] = JsonValue(v.role);
    obj["content"] = SerializeContentVariant(v.content);
    return obj;
}

SamplingMessage DeserializeSamplingMessage(const JsonValue& j) {
    SamplingMessage v;
    v.role = j["role"].GetString();
    v.content = DeserializeContentVariant(j["content"]);
    return v;
}

// ── CreateMessageRequestParams ──

JsonValue SerializeCreateMessageRequestParams(const CreateMessageRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    {
        JsonValue::Array arr;
        for (const auto& msg : v.messages) arr.push_back(SerializeSamplingMessage(msg));
        obj["messages"] = JsonValue(std::move(arr));
    }
    obj["maxTokens"] = JsonValue(v.max_tokens);
    detail::SerializeOptional(obj, "stopReason", v.stop_reason);
    detail::SerializeOptional(obj, "modelPreference", v.model_preference);
    return obj;
}

CreateMessageRequestParams DeserializeCreateMessageRequestParams(const JsonValue& j) {
    CreateMessageRequestParams v;
    auto* msgs = j.Find("messages");
    if (msgs && msgs->IsArray()) {
        std::vector<SamplingMessage> vec;
        for (const auto& mv : msgs->GetArray()) vec.push_back(DeserializeSamplingMessage(mv));
        v.messages = std::move(vec);
    }
    v.max_tokens = j["maxTokens"].GetInt();
    detail::DeserializeOptional(j, "stopReason", v.stop_reason);
    detail::DeserializeOptional(j, "modelPreference", v.model_preference);
    return v;
}

// ── CreateMessageResult ──

JsonValue SerializeCreateMessageResult(const CreateMessageResult& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["role"] = JsonValue(v.role);
    obj["content"] = SerializeContentVariant(v.content);
    obj["model"] = JsonValue(v.model);
    obj["resultType"] = SerializeResultType(v.result_type);
    detail::SerializeOptional(obj, "stopReason", v.stop_reason);
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

CreateMessageResult DeserializeCreateMessageResult(const JsonValue& j) {
    CreateMessageResult v;
    v.role = j["role"].GetString();
    v.content = DeserializeContentVariant(j["content"]);
    v.model = j["model"].GetString();
    auto* rt = j.Find("resultType");
    if (rt) v.result_type = DeserializeResultType(*rt);
    detail::DeserializeOptional(j, "stopReason", v.stop_reason);
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

// ── Root ──

JsonValue SerializeRoot(const Root& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["uri"] = JsonValue(v.uri);
    detail::SerializeOptional(obj, "name", v.name);
    return obj;
}

Root DeserializeRoot(const JsonValue& j) {
    Root v;
    v.uri = j["uri"].GetString();
    detail::DeserializeOptional(j, "name", v.name);
    return v;
}

// ── ListRootsRequestParams ──

JsonValue SerializeListRootsRequestParams(const ListRootsRequestParams&) {
    return JsonValue(JsonValue::object_tag);
}

ListRootsRequestParams DeserializeListRootsRequestParams(const JsonValue&) {
    return ListRootsRequestParams{};
}

// ── ListRootsResult ──

JsonValue SerializeListRootsResult(const ListRootsResult& v) {
    JsonValue obj(JsonValue::object_tag);
    {
        JsonValue::Array arr;
        for (const auto& r : v.roots) arr.push_back(SerializeRoot(r));
        obj["roots"] = JsonValue(std::move(arr));
    }
    obj["resultType"] = SerializeResultType(v.result_type);
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

ListRootsResult DeserializeListRootsResult(const JsonValue& j) {
    ListRootsResult v;
    auto* roots = j.Find("roots");
    if (roots && roots->IsArray()) {
        std::vector<Root> vec;
        for (const auto& rv : roots->GetArray()) vec.push_back(DeserializeRoot(rv));
        v.roots = std::move(vec);
    }
    auto* rt = j.Find("resultType");
    if (rt) v.result_type = DeserializeResultType(*rt);
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

// ── SetLevelRequestParams ──

JsonValue SerializeSetLevelRequestParams(const SetLevelRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["level"] = SerializeLoggingLevelValue(v.level);
    return obj;
}

SetLevelRequestParams DeserializeSetLevelRequestParams(const JsonValue& j) {
    SetLevelRequestParams v;
    v.level = DeserializeLoggingLevelValue(j["level"]);
    return v;
}

// ── GetTaskResult ──

JsonValue SerializeGetTaskResult(const GetTaskResult& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["taskId"] = JsonValue(v.task_id);
    obj["status"] = JsonValue(v.status);
    obj["resultType"] = JsonValue(v.task_result_type);
    detail::SerializeOptional(obj, "result", v.result);
    detail::SerializeOptional(obj, "errorMessage", v.error_message);
    detail::SerializeOptional(obj, "inputRequired", v.input_required);
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

GetTaskResult DeserializeGetTaskResult(const JsonValue& j) {
    GetTaskResult v;
    v.task_id = j["taskId"].GetString();
    v.status = j["status"].GetString();
    auto* rt = j.Find("resultType");
    if (rt && rt->IsString()) v.task_result_type = rt->GetString();
    detail::DeserializeOptional(j, "result", v.result);
    detail::DeserializeOptional(j, "errorMessage", v.error_message);
    detail::DeserializeOptional(j, "inputRequired", v.input_required);
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

// ── GetTaskRequestParams ──

JsonValue SerializeGetTaskRequestParams(const GetTaskRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["taskId"] = JsonValue(v.task_id);
    return obj;
}

GetTaskRequestParams DeserializeGetTaskRequestParams(const JsonValue& j) {
    GetTaskRequestParams v;
    v.task_id = j["taskId"].GetString();
    return v;
}

// ── UpdateTaskRequestParams ──

JsonValue SerializeUpdateTaskRequestParams(const UpdateTaskRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["taskId"] = JsonValue(v.task_id);
    detail::SerializeOptional(obj, "result", v.result);
    return obj;
}

UpdateTaskRequestParams DeserializeUpdateTaskRequestParams(const JsonValue& j) {
    UpdateTaskRequestParams v;
    v.task_id = j["taskId"].GetString();
    detail::DeserializeOptional(j, "result", v.result);
    return v;
}

// ── CancelTaskRequestParams ──

JsonValue SerializeCancelTaskRequestParams(const CancelTaskRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["taskId"] = JsonValue(v.task_id);
    detail::SerializeOptional(obj, "reason", v.reason);
    return obj;
}

CancelTaskRequestParams DeserializeCancelTaskRequestParams(const JsonValue& j) {
    CancelTaskRequestParams v;
    v.task_id = j["taskId"].GetString();
    detail::DeserializeOptional(j, "reason", v.reason);
    return v;
}

// ── RequestOptions ──

JsonValue SerializeRequestOptions(const RequestOptions& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "_meta", v.meta);
    detail::SerializeOptional(obj, "readTimeoutMs", v.read_timeout_ms);
    detail::SerializeOptional(obj, "inputResponses", v.input_responses);
    detail::SerializeOptional(obj, "requestState", v.request_state);
    return obj;
}

RequestOptions DeserializeRequestOptions(const JsonValue& j) {
    RequestOptions v;
    detail::DeserializeOptional(j, "_meta", v.meta);
    detail::DeserializeOptional(j, "readTimeoutMs", v.read_timeout_ms);
    detail::DeserializeOptional(j, "inputResponses", v.input_responses);
    detail::DeserializeOptional(j, "requestState", v.request_state);
    return v;
}

// ── CacheableRequestOptions ──

JsonValue SerializeCacheableRequestOptions(const CacheableRequestOptions& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "_meta", v.meta);
    detail::SerializeOptional(obj, "readTimeoutMs", v.read_timeout_ms);
    detail::SerializeOptional(obj, "inputResponses", v.input_responses);
    detail::SerializeOptional(obj, "requestState", v.request_state);
    detail::SerializeOptional(obj, "cacheMode", v.cache_mode);
    detail::SerializeOptional(obj, "maxAgeMs", v.max_age_ms);
    return obj;
}

CacheableRequestOptions DeserializeCacheableRequestOptions(const JsonValue& j) {
    CacheableRequestOptions v;
    detail::DeserializeOptional(j, "_meta", v.meta);
    detail::DeserializeOptional(j, "readTimeoutMs", v.read_timeout_ms);
    detail::DeserializeOptional(j, "inputResponses", v.input_responses);
    detail::DeserializeOptional(j, "requestState", v.request_state);
    detail::DeserializeOptional(j, "cacheMode", v.cache_mode);
    detail::DeserializeOptional(j, "maxAgeMs", v.max_age_ms);
    return v;
}

// ── ToolOptions ──

JsonValue SerializeToolOptions(const ToolOptions& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "name", v.name);
    detail::SerializeOptional(obj, "description", v.description);
    detail::SerializeOptional(obj, "title", v.title);
    detail::SerializeOptional(obj, "destructiveHint", v.destructive);
    detail::SerializeOptional(obj, "idempotentHint", v.idempotent);
    detail::SerializeOptional(obj, "readOnlyHint", v.read_only_hint);
    detail::SerializeOptional(obj, "openWorldHint", v.open_world_hint);
    if (v.use_structured_content) obj["structuredContent"] = JsonValue(true);
    detail::SerializeOptional(obj, "inputSchema", v.input_schema);
    detail::SerializeOptional(obj, "outputSchema", v.output_schema);
    if (!v.icons.empty()) {
        JsonValue::Array arr;
        for (const auto& icon : v.icons) arr.push_back(SerializeIcon(icon));
        obj["icons"] = JsonValue(std::move(arr));
    }
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

ToolOptions DeserializeToolOptions(const JsonValue& j) {
    ToolOptions v;
    detail::DeserializeOptional(j, "name", v.name);
    detail::DeserializeOptional(j, "description", v.description);
    detail::DeserializeOptional(j, "title", v.title);
    detail::DeserializeOptional(j, "destructiveHint", v.destructive);
    detail::DeserializeOptional(j, "idempotentHint", v.idempotent);
    detail::DeserializeOptional(j, "readOnlyHint", v.read_only_hint);
    detail::DeserializeOptional(j, "openWorldHint", v.open_world_hint);
    auto* sc = j.Find("structuredContent");
    if (sc && sc->IsBool()) v.use_structured_content = sc->GetBool();
    detail::DeserializeOptional(j, "inputSchema", v.input_schema);
    detail::DeserializeOptional(j, "outputSchema", v.output_schema);
    auto* icons = j.Find("icons");
    if (icons && icons->IsArray()) {
        std::vector<Icon> vec;
        for (const auto& ic : icons->GetArray()) vec.push_back(DeserializeIcon(ic));
        v.icons = std::move(vec);
    }
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

// ── ResourceOptions ──

JsonValue SerializeResourceOptions(const ResourceOptions& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "name", v.name);
    detail::SerializeOptional(obj, "description", v.description);
    detail::SerializeOptional(obj, "title", v.title);
    detail::SerializeOptional(obj, "mimeType", v.mime_type);
    if (!v.icons.empty()) {
        JsonValue::Array arr;
        for (const auto& icon : v.icons) arr.push_back(SerializeIcon(icon));
        obj["icons"] = JsonValue(std::move(arr));
    }
    return obj;
}

ResourceOptions DeserializeResourceOptions(const JsonValue& j) {
    ResourceOptions v;
    detail::DeserializeOptional(j, "name", v.name);
    detail::DeserializeOptional(j, "description", v.description);
    detail::DeserializeOptional(j, "title", v.title);
    detail::DeserializeOptional(j, "mimeType", v.mime_type);
    auto* icons = j.Find("icons");
    if (icons && icons->IsArray()) {
        std::vector<Icon> vec;
        for (const auto& ic : icons->GetArray()) vec.push_back(DeserializeIcon(ic));
        v.icons = std::move(vec);
    }
    return v;
}

// ── PromptOptions ──

JsonValue SerializePromptOptions(const PromptOptions& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "name", v.name);
    detail::SerializeOptional(obj, "description", v.description);
    detail::SerializeOptional(obj, "title", v.title);
    if (!v.icons.empty()) {
        JsonValue::Array arr;
        for (const auto& icon : v.icons) arr.push_back(SerializeIcon(icon));
        obj["icons"] = JsonValue(std::move(arr));
    }
    return obj;
}

PromptOptions DeserializePromptOptions(const JsonValue& j) {
    PromptOptions v;
    detail::DeserializeOptional(j, "name", v.name);
    detail::DeserializeOptional(j, "description", v.description);
    detail::DeserializeOptional(j, "title", v.title);
    auto* icons = j.Find("icons");
    if (icons && icons->IsArray()) {
        std::vector<Icon> vec;
        for (const auto& ic : icons->GetArray()) vec.push_back(DeserializeIcon(ic));
        v.icons = std::move(vec);
    }
    return v;
}

// ====================================================================
// Free functions for elicitation helpers
// ====================================================================

JsonValue MakeInputRequestForElicitation(const ElicitRequestParams& params) {
    JsonValue obj(JsonValue::object_tag);
    obj["method"] = JsonValue(std::string(methods::kElicit));
    obj["params"] = SerializeElicitRequestParams(params);
    return obj;
}

JsonValue MakeInputResponseFromElicitResult(const ElicitResult& result) {
    return SerializeElicitResult(result);
}

bool IsInputRequiredResult(const JsonValue& j) {
    if (!j.IsObject()) return false;
    auto* rt = j.Find("resultType");
    return rt && rt->IsString() && rt->GetString() == "input_required";
}

std::optional<InputRequests> ExtractInputRequests(const JsonValue& result) {
    if (!result.IsObject()) return std::nullopt;
    auto* ir = result.Find("inputRequests");
    if (!ir) return std::nullopt;
    return DeserializeInputRequests(*ir);
}

} // namespace mcp
