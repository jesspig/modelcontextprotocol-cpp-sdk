// McpTypesResults.cpp — Result type serialization

#include <mcp/McpTypes.hpp>
#include <mcp/McpError.hpp>
#include <detail/JsonFields.hpp>
#include <detail/JsonSerializer.hpp>

namespace mcp {

namespace {

// Cache hint deserialization compatible with both wire shapes: the 2026 era
// flattens ttlMs/cacheScope onto the result top level; the 2025 era nests
// them under cacheHint.
std::optional<CacheHint> DeserializeCacheHintCompat(const JsonValue& j) {
    auto* ttl = j.Find(detail::kTTLMs);
    auto* scope = j.Find(detail::kCacheScope);
    if (ttl || scope) {
        CacheHint hint;
        if (ttl && ttl->IsInt()) hint.ttl_ms = ttl->GetInt();
        if (scope && scope->IsString()) hint.cache_scope = scope->GetString();
        return hint;
    }
    auto* ch = j.Find(detail::kCacheHint);
    if (ch) return DeserializeCacheHint(*ch);
    return std::nullopt;
}

} // anonymous namespace

// ── ResultType enum ──

JsonValue SerializeResultType(ResultType v) {
    switch (v) {
        case ResultType::Complete:     return JsonValue(detail::kCompleteValue);
        case ResultType::InputRequired: return JsonValue(detail::kInputRequiredValue);
        default:
            throw McpError(McpErrorCode::InvalidParams,
                "SerializeResultType: unknown value: " + std::to_string(static_cast<int>(v)));
    }
}

ResultType DeserializeResultType(const JsonValue& j) {
    auto s = j.GetString();
    if (s == detail::kInputRequiredValue) return ResultType::InputRequired;
    if (s == detail::kCompleteValue) return ResultType::Complete;
    throw McpError(McpErrorCode::InvalidParams,
        std::string("DeserializeResultType: unknown string: '") + s + "'");
}

namespace {

// ── List-style result serialization helpers ──
// Shared by ListToolsResult / ListResourcesResult / ListResourceTemplatesResult /
// ListPromptsResult and ReadResourceResult (which has no next_cursor).

template <typename T, typename SerializeFn>
void SerializeListItems(JsonValue& obj, const char* items_key,
                        const std::vector<T>& items, SerializeFn&& ser) {
    JsonValue::Array arr;
    for (const auto& item : items) arr.push_back(ser(item));
    obj[items_key] = JsonValue(std::move(arr));
}

// Common trailing fields of list-style results: nextCursor (omit when absent,
// pass std::nullopt for results without one), cacheHint, meta, resultType.
void WriteListResultCommon(JsonValue& obj,
                           const std::optional<std::string>& next_cursor,
                           const std::optional<CacheHint>& cache_hint,
                           const std::optional<JsonValue>& meta,
                           ResultType result_type) {
    detail::SerializeOptional(obj, detail::kNextCursor, next_cursor);
    if (cache_hint) obj[detail::kCacheHint] = SerializeCacheHint(*cache_hint);
    detail::SerializeOptional(obj, detail::kMeta, meta);
    obj[detail::kResultType] = SerializeResultType(result_type);
}

template <typename T, typename DeserializeFn>
std::vector<T> DeserializeListItems(const JsonValue& j, const char* items_key,
                                    DeserializeFn&& deser) {
    std::vector<T> result;
    auto* v = j.Find(items_key);
    if (v && v->IsArray()) {
        for (const auto& elem : v->GetArray()) result.push_back(deser(elem));
    }
    return result;
}

// Common trailing fields of list-style results; next_cursor may be nullptr
// for results without a cursor (e.g. ReadResourceResult).
void ReadListResultCommon(const JsonValue& j,
                          std::optional<std::string>* next_cursor,
                          std::optional<CacheHint>& cache_hint,
                          std::optional<JsonValue>& meta,
                          ResultType& result_type) {
    if (next_cursor) detail::DeserializeOptional(j, detail::kNextCursor, *next_cursor);
    cache_hint = DeserializeCacheHintCompat(j);
    detail::DeserializeOptional(j, detail::kMeta, meta);
    auto* rt = j.Find(detail::kResultType);
    if (rt) result_type = DeserializeResultType(*rt);
}

} // anonymous namespace

// ── EmptyResult ──

JsonValue SerializeEmptyResult(const EmptyResult& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    obj[detail::kResultType] = SerializeResultType(v.result_type);
    return obj;
}

EmptyResult DeserializeEmptyResult(const JsonValue& j) {
    EmptyResult v;
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    auto* rt = j.Find(detail::kResultType);
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

// ── CallToolResult ──

JsonValue SerializeCallToolResult(const CallToolResult& v) {
    JsonValue obj(JsonValue::object_tag);
    {
        JsonValue::Array arr;
        for (const auto& c : v.content) arr.push_back(SerializeContentVariant(c));
        obj[detail::kContent] = JsonValue(std::move(arr));
    }
    obj["isError"] = JsonValue(v.is_error);
    detail::SerializeOptional(obj, detail::kStructuredContent, v.structured_content);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    obj[detail::kResultType] = SerializeResultType(v.result_type);
    return obj;
}

CallToolResult DeserializeCallToolResult(const JsonValue& j) {
    CallToolResult v;
    auto* content = j.Find(detail::kContent);
    if (content && content->IsArray()) {
        std::vector<ContentVariant> vec;
        for (const auto& cv : content->GetArray()) vec.push_back(DeserializeContentVariant(cv));
        v.content = std::move(vec);
    }
    auto* isErr = j.Find("isError");
    if (isErr && isErr->IsBool()) v.is_error = isErr->GetBool();
    detail::DeserializeOptional(j, detail::kStructuredContent, v.structured_content);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    auto* rt = j.Find(detail::kResultType);
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

// ── ListToolsResult ──

JsonValue SerializeListToolsResult(const ListToolsResult& v) {
    JsonValue obj(JsonValue::object_tag);
    SerializeListItems(obj, detail::kTools, v.tools, SerializeTool);
    WriteListResultCommon(obj, v.next_cursor, v.cache_hint, v.meta, v.result_type);
    return obj;
}

ListToolsResult DeserializeListToolsResult(const JsonValue& j) {
    ListToolsResult v;
    v.tools = DeserializeListItems<Tool>(j, detail::kTools, DeserializeTool);
    ReadListResultCommon(j, &v.next_cursor, v.cache_hint, v.meta, v.result_type);
    return v;
}

// ── ListResourcesResult ──

JsonValue SerializeListResourcesResult(const ListResourcesResult& v) {
    JsonValue obj(JsonValue::object_tag);
    SerializeListItems(obj, detail::kResources, v.resources, SerializeResource);
    WriteListResultCommon(obj, v.next_cursor, v.cache_hint, v.meta, v.result_type);
    return obj;
}

ListResourcesResult DeserializeListResourcesResult(const JsonValue& j) {
    ListResourcesResult v;
    v.resources = DeserializeListItems<Resource>(j, detail::kResources, DeserializeResource);
    ReadListResultCommon(j, &v.next_cursor, v.cache_hint, v.meta, v.result_type);
    return v;
}

// ── ListResourceTemplatesResult ──

JsonValue SerializeListResourceTemplatesResult(const ListResourceTemplatesResult& v) {
    JsonValue obj(JsonValue::object_tag);
    SerializeListItems(obj, detail::kResourceTemplates, v.resource_templates, SerializeResourceTemplate);
    WriteListResultCommon(obj, v.next_cursor, v.cache_hint, v.meta, v.result_type);
    return obj;
}

ListResourceTemplatesResult DeserializeListResourceTemplatesResult(const JsonValue& j) {
    ListResourceTemplatesResult v;
    v.resource_templates = DeserializeListItems<ResourceTemplate>(j, detail::kResourceTemplates, DeserializeResourceTemplate);
    ReadListResultCommon(j, &v.next_cursor, v.cache_hint, v.meta, v.result_type);
    return v;
}

// ── ReadResourceResult ──

JsonValue SerializeReadResourceResult(const ReadResourceResult& v) {
    JsonValue obj(JsonValue::object_tag);
    SerializeListItems(obj, detail::kContents, v.contents, SerializeResourceContents);
    WriteListResultCommon(obj, std::nullopt, v.cache_hint, v.meta, v.result_type);
    return obj;
}

ReadResourceResult DeserializeReadResourceResult(const JsonValue& j) {
    ReadResourceResult v;
    v.contents = DeserializeListItems<ResourceContents>(j, detail::kContents, DeserializeResourceContents);
    ReadListResultCommon(j, nullptr, v.cache_hint, v.meta, v.result_type);
    return v;
}

// ── ListPromptsResult ──

JsonValue SerializeListPromptsResult(const ListPromptsResult& v) {
    JsonValue obj(JsonValue::object_tag);
    SerializeListItems(obj, detail::kPrompts, v.prompts, SerializePrompt);
    WriteListResultCommon(obj, v.next_cursor, v.cache_hint, v.meta, v.result_type);
    return obj;
}

ListPromptsResult DeserializeListPromptsResult(const JsonValue& j) {
    ListPromptsResult v;
    v.prompts = DeserializeListItems<Prompt>(j, detail::kPrompts, DeserializePrompt);
    ReadListResultCommon(j, &v.next_cursor, v.cache_hint, v.meta, v.result_type);
    return v;
}

// ── GetPromptResult ──

JsonValue SerializeGetPromptResult(const GetPromptResult& v) {
    JsonValue obj(JsonValue::object_tag);
    {
        JsonValue::Array arr;
        for (const auto& msg : v.messages) arr.push_back(SerializePromptMessage(msg));
        obj[detail::kMessages] = JsonValue(std::move(arr));
    }
    detail::SerializeOptional(obj, detail::kDescription, v.description);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    obj[detail::kResultType] = SerializeResultType(v.result_type);
    return obj;
}

GetPromptResult DeserializeGetPromptResult(const JsonValue& j) {
    GetPromptResult v;
    auto* msgs = j.Find(detail::kMessages);
    if (msgs && msgs->IsArray()) {
        std::vector<PromptMessage> vec;
        for (const auto& mv : msgs->GetArray()) vec.push_back(DeserializePromptMessage(mv));
        v.messages = std::move(vec);
    }
    detail::DeserializeOptional(j, detail::kDescription, v.description);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    auto* rt = j.Find(detail::kResultType);
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

// ── CompleteResult ──

JsonValue SerializeCompleteResult(const CompleteResult& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["completion"] = v.completion;
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    obj[detail::kResultType] = SerializeResultType(v.result_type);
    return obj;
}

CompleteResult DeserializeCompleteResult(const JsonValue& j) {
    CompleteResult v;
    v.completion = j["completion"];
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    auto* rt = j.Find(detail::kResultType);
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

// ── InitializeResult ──

JsonValue SerializeInitializeResult(const InitializeResult& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kProtocolVersion] = JsonValue(v.protocol_version);
    obj[detail::kCapabilities] = SerializeServerCapabilities(v.capabilities);
    obj[detail::kServerInfo] = SerializeImplementation(v.server_info);
    detail::SerializeOptional(obj, detail::kInstructions, v.instructions);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    obj[detail::kResultType] = SerializeResultType(v.result_type);
    return obj;
}

InitializeResult DeserializeInitializeResult(const JsonValue& j) {
    InitializeResult v;
    v.protocol_version = j[detail::kProtocolVersion].GetString();
    v.capabilities = DeserializeServerCapabilities(j[detail::kCapabilities]);
    v.server_info = DeserializeImplementation(j[detail::kServerInfo]);
    detail::DeserializeOptional(j, detail::kInstructions, v.instructions);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    auto* rt = j.Find(detail::kResultType);
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
    obj[detail::kCapabilities] = SerializeServerCapabilities(v.capabilities);
    obj[detail::kServerInfo] = SerializeImplementation(v.server_info);
    detail::SerializeOptional(obj, detail::kInstructions, v.instructions);
    if (v.cache_hint) obj[detail::kCacheHint] = SerializeCacheHint(*v.cache_hint);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    obj[detail::kResultType] = SerializeResultType(v.result_type);
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
    v.capabilities = DeserializeServerCapabilities(j[detail::kCapabilities]);
    v.server_info = DeserializeImplementation(j[detail::kServerInfo]);
    detail::DeserializeOptional(j, detail::kInstructions, v.instructions);
    v.cache_hint = DeserializeCacheHintCompat(j);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    auto* rt = j.Find(detail::kResultType);
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

// ── InputRequestElicit ──

JsonValue SerializeInputRequestElicit(const InputRequestElicit& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kMessage] = JsonValue(v.message);
    detail::SerializeOptional(obj, detail::kRequestedSchema, v.requested_schema);
    return obj;
}

InputRequestElicit DeserializeInputRequestElicit(const JsonValue& j) {
    InputRequestElicit v;
    v.message = j[detail::kMessage].GetString();
    detail::DeserializeOptional(j, detail::kRequestedSchema, v.requested_schema);
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
    obj[detail::kInputRequests] = SerializeInputRequests(v.input_requests);
    obj[detail::kResultType] = JsonValue(detail::kInputRequiredValue);
    detail::SerializeOptional(obj, detail::kRequestState, v.request_state);
    return obj;
}

InputRequiredResult DeserializeInputRequiredResult(const JsonValue& j) {
    InputRequiredResult v;
    v.input_requests = DeserializeInputRequests(j[detail::kInputRequests]);
    detail::DeserializeOptional(j, detail::kRequestState, v.request_state);
    return v;
}

// ── ElicitResult ──

JsonValue SerializeElicitResult(const ElicitResult& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "values", v.values);
    obj[detail::kResultType] = SerializeResultType(v.result_type);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

ElicitResult DeserializeElicitResult(const JsonValue& j) {
    ElicitResult v;
    detail::DeserializeOptional(j, "values", v.values);
    auto* rt = j.Find(detail::kResultType);
    if (rt) v.result_type = DeserializeResultType(*rt);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    return v;
}

// ── CreateMessageResult ──

JsonValue SerializeCreateMessageResult(const CreateMessageResult& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kRole] = JsonValue(v.role);
    obj[detail::kContent] = SerializeContentVariant(v.content);
    obj["model"] = JsonValue(v.model);
    obj[detail::kResultType] = SerializeResultType(v.result_type);
    detail::SerializeOptional(obj, detail::kStopReason, v.stop_reason);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

CreateMessageResult DeserializeCreateMessageResult(const JsonValue& j) {
    CreateMessageResult v;
    v.role = j[detail::kRole].GetString();
    v.content = DeserializeContentVariant(j[detail::kContent]);
    v.model = j["model"].GetString();
    auto* rt = j.Find(detail::kResultType);
    if (rt) v.result_type = DeserializeResultType(*rt);
    detail::DeserializeOptional(j, detail::kStopReason, v.stop_reason);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    return v;
}

// ── ListRootsResult ──

JsonValue SerializeListRootsResult(const ListRootsResult& v) {
    JsonValue obj(JsonValue::object_tag);
    {
        JsonValue::Array arr;
        for (const auto& r : v.roots) arr.push_back(SerializeRoot(r));
        obj[detail::kRoots] = JsonValue(std::move(arr));
    }
    obj[detail::kResultType] = SerializeResultType(v.result_type);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

ListRootsResult DeserializeListRootsResult(const JsonValue& j) {
    ListRootsResult v;
    auto* roots = j.Find(detail::kRoots);
    if (roots && roots->IsArray()) {
        std::vector<Root> vec;
        for (const auto& rv : roots->GetArray()) vec.push_back(DeserializeRoot(rv));
        v.roots = std::move(vec);
    }
    auto* rt = j.Find(detail::kResultType);
    if (rt) v.result_type = DeserializeResultType(*rt);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    return v;
}

} // namespace mcp
