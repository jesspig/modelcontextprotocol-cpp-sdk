// McpTypesOptions.cpp — Registration options serialization

#include <mcp/McpTypes.hpp>
#include <detail/JsonFields.hpp>
#include <detail/JsonSerializer.hpp>

namespace mcp {

// ── RequestOptions ──

JsonValue SerializeRequestOptions(const RequestOptions& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    detail::SerializeOptional(obj, detail::kReadTimeoutMs, v.read_timeout_ms);
    detail::SerializeOptional(obj, detail::kInputResponses, v.input_responses);
    detail::SerializeOptional(obj, detail::kRequestState, v.request_state);
    return obj;
}

RequestOptions DeserializeRequestOptions(const JsonValue& j) {
    RequestOptions v;
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    detail::DeserializeOptional(j, detail::kReadTimeoutMs, v.read_timeout_ms);
    detail::DeserializeOptional(j, detail::kInputResponses, v.input_responses);
    detail::DeserializeOptional(j, detail::kRequestState, v.request_state);
    return v;
}

// ── CacheableRequestOptions ──

JsonValue SerializeCacheableRequestOptions(const CacheableRequestOptions& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    detail::SerializeOptional(obj, detail::kReadTimeoutMs, v.read_timeout_ms);
    detail::SerializeOptional(obj, detail::kInputResponses, v.input_responses);
    detail::SerializeOptional(obj, detail::kRequestState, v.request_state);
    detail::SerializeOptional(obj, "cacheMode", v.cache_mode);
    detail::SerializeOptional(obj, "maxAgeMs", v.max_age_ms);
    return obj;
}

CacheableRequestOptions DeserializeCacheableRequestOptions(const JsonValue& j) {
    CacheableRequestOptions v;
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    detail::DeserializeOptional(j, detail::kReadTimeoutMs, v.read_timeout_ms);
    detail::DeserializeOptional(j, detail::kInputResponses, v.input_responses);
    detail::DeserializeOptional(j, detail::kRequestState, v.request_state);
    detail::DeserializeOptional(j, "cacheMode", v.cache_mode);
    detail::DeserializeOptional(j, "maxAgeMs", v.max_age_ms);
    return v;
}

// ── ToolOptions ──

JsonValue SerializeToolOptions(const ToolOptions& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, detail::kName, v.name);
    detail::SerializeOptional(obj, detail::kDescription, v.description);
    detail::SerializeOptional(obj, detail::kTitle, v.title);
    detail::SerializeOptional(obj, detail::kDestructiveHint, v.destructive);
    detail::SerializeOptional(obj, detail::kIdempotentHint, v.idempotent);
    detail::SerializeOptional(obj, detail::kReadOnlyHint, v.read_only_hint);
    detail::SerializeOptional(obj, detail::kOpenWorldHint, v.open_world_hint);
    if (v.use_structured_content) obj[detail::kStructuredContent] = JsonValue(true);
    detail::SerializeOptional(obj, detail::kInputSchema, v.input_schema);
    detail::SerializeOptional(obj, detail::kOutputSchema, v.output_schema);
    detail::SerializeVector(obj, detail::kIcons, v.icons,
        [](const Icon& icon) { return SerializeIcon(icon); });
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

ToolOptions DeserializeToolOptions(const JsonValue& j) {
    ToolOptions v;
    detail::DeserializeOptional(j, detail::kName, v.name);
    detail::DeserializeOptional(j, detail::kDescription, v.description);
    detail::DeserializeOptional(j, detail::kTitle, v.title);
    detail::DeserializeOptional(j, detail::kDestructiveHint, v.destructive);
    detail::DeserializeOptional(j, detail::kIdempotentHint, v.idempotent);
    detail::DeserializeOptional(j, detail::kReadOnlyHint, v.read_only_hint);
    detail::DeserializeOptional(j, detail::kOpenWorldHint, v.open_world_hint);
    auto* sc = j.Find(detail::kStructuredContent);
    if (sc && sc->IsBool()) v.use_structured_content = sc->GetBool();
    detail::DeserializeOptional(j, detail::kInputSchema, v.input_schema);
    detail::DeserializeOptional(j, detail::kOutputSchema, v.output_schema);
    v.icons = detail::DeserializeVector<Icon>(j, detail::kIcons,
        [](const JsonValue& ic) { return DeserializeIcon(ic); });
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    return v;
}

// ── ResourceOptions ──

JsonValue SerializeResourceOptions(const ResourceOptions& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, detail::kName, v.name);
    detail::SerializeOptional(obj, detail::kDescription, v.description);
    detail::SerializeOptional(obj, detail::kTitle, v.title);
    detail::SerializeOptional(obj, detail::kMimeType, v.mime_type);
    detail::SerializeVector(obj, detail::kIcons, v.icons,
        [](const Icon& icon) { return SerializeIcon(icon); });
    return obj;
}

ResourceOptions DeserializeResourceOptions(const JsonValue& j) {
    ResourceOptions v;
    detail::DeserializeOptional(j, detail::kName, v.name);
    detail::DeserializeOptional(j, detail::kDescription, v.description);
    detail::DeserializeOptional(j, detail::kTitle, v.title);
    detail::DeserializeOptional(j, detail::kMimeType, v.mime_type);
    v.icons = detail::DeserializeVector<Icon>(j, detail::kIcons,
        [](const JsonValue& ic) { return DeserializeIcon(ic); });
    return v;
}

// ── PromptOptions ──

JsonValue SerializePromptOptions(const PromptOptions& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, detail::kName, v.name);
    detail::SerializeOptional(obj, detail::kDescription, v.description);
    detail::SerializeOptional(obj, detail::kTitle, v.title);
    detail::SerializeVector(obj, detail::kIcons, v.icons,
        [](const Icon& icon) { return SerializeIcon(icon); });
    return obj;
}

PromptOptions DeserializePromptOptions(const JsonValue& j) {
    PromptOptions v;
    detail::DeserializeOptional(j, detail::kName, v.name);
    detail::DeserializeOptional(j, detail::kDescription, v.description);
    detail::DeserializeOptional(j, detail::kTitle, v.title);
    v.icons = detail::DeserializeVector<Icon>(j, detail::kIcons,
        [](const JsonValue& ic) { return DeserializeIcon(ic); });
    return v;
}

} // namespace mcp
