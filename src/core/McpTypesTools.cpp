// McpTypesTools.cpp — Tool/resource/prompt type serialization

#include <mcp/McpTypes.hpp>
#include <mcp/McpError.hpp>
#include <detail/JsonFields.hpp>
#include <detail/JsonSerializer.hpp>

namespace mcp {

// ── ToolAnnotations ──

JsonValue SerializeToolAnnotations(const ToolAnnotations& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, detail::kTitle, v.title);
    detail::SerializeOptional(obj, detail::kReadOnlyHint, v.read_only_hint);
    detail::SerializeOptional(obj, detail::kIdempotentHint, v.idempotent_hint);
    detail::SerializeOptional(obj, detail::kOpenWorldHint, v.open_world_hint);
    detail::SerializeOptional(obj, detail::kDestructiveHint, v.destructive_hint);
    return obj;
}

ToolAnnotations DeserializeToolAnnotations(const JsonValue& j) {
    ToolAnnotations v;
    detail::DeserializeOptional(j, detail::kTitle, v.title);
    detail::DeserializeOptional(j, detail::kReadOnlyHint, v.read_only_hint);
    detail::DeserializeOptional(j, detail::kIdempotentHint, v.idempotent_hint);
    detail::DeserializeOptional(j, detail::kOpenWorldHint, v.open_world_hint);
    detail::DeserializeOptional(j, detail::kDestructiveHint, v.destructive_hint);
    return v;
}

// ── ToolExecution ──

JsonValue SerializeToolExecution(const ToolExecution& v) {
    JsonValue obj(JsonValue::object_tag);
    switch (v.mode) {
        case ToolExecutionMode::Auto: obj[detail::kMode] = JsonValue("auto"); break;
        case ToolExecutionMode::Manual: obj[detail::kMode] = JsonValue("manual"); break;
        case ToolExecutionMode::Task: obj[detail::kMode] = JsonValue("task"); break;
        default:
            throw McpError(McpErrorCode::InvalidParams,
                "SerializeToolExecution: unknown mode: " + std::to_string(static_cast<int>(v.mode)));
    }
    detail::SerializeOptional(obj, "humanUse", v.human_use);
    return obj;
}

ToolExecution DeserializeToolExecution(const JsonValue& j) {
    ToolExecution v;
    if (auto* m = j.Find(detail::kMode)) {
        auto s = m->GetString();
        if (s == "auto") v.mode = ToolExecutionMode::Auto;
        else if (s == "manual") v.mode = ToolExecutionMode::Manual;
        else if (s == "task") v.mode = ToolExecutionMode::Task;
        else
            throw McpError(McpErrorCode::InvalidParams,
                std::string("DeserializeToolExecution: unknown mode string: '") + s + "'");
    }
    detail::DeserializeOptional(j, "humanUse", v.human_use);
    return v;
}

// ── ResourceAnnotations ──

JsonValue SerializeResourceAnnotations(const ResourceAnnotations& v) {
    JsonValue obj(JsonValue::object_tag);
    if (v.audience) {
        detail::SerializeVector(obj, detail::kAudience, *v.audience,
            [](const std::string& s) { return JsonValue(s); });
    }
    detail::SerializeOptional(obj, detail::kPriority, v.priority);
    detail::SerializeOptional(obj, detail::kLastModified, v.last_modified);
    return obj;
}

ResourceAnnotations DeserializeResourceAnnotations(const JsonValue& j) {
    ResourceAnnotations v;
    auto* aud = j.Find(detail::kAudience);
    if (aud && aud->IsArray()) {
        v.audience = detail::DeserializeVector<std::string>(*aud,
            [](const JsonValue& s) { return s.GetString(); });
    }
    detail::DeserializeOptional(j, detail::kPriority, v.priority);
    detail::DeserializeOptional(j, detail::kLastModified, v.last_modified);
    return v;
}

// ── Tool ──

JsonValue SerializeTool(const Tool& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kName] = JsonValue(v.name);
    detail::SerializeOptional(obj, detail::kTitle, v.title);
    detail::SerializeOptional(obj, detail::kDescription, v.description);
    obj[detail::kInputSchema] = v.input_schema;
    detail::SerializeOptional(obj, detail::kOutputSchema, v.output_schema);
    if (v.annotations) obj[detail::kAnnotations] = SerializeToolAnnotations(*v.annotations);
    if (v.execution) obj["execution"] = SerializeToolExecution(*v.execution);
    detail::SerializeVector(obj, detail::kIcons, v.icons,
        [](const Icon& icon) { return SerializeIcon(icon); });
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

Tool DeserializeTool(const JsonValue& j) {
    Tool v;
    v.name = j[detail::kName].GetString();
    detail::DeserializeOptional(j, detail::kTitle, v.title);
    detail::DeserializeOptional(j, detail::kDescription, v.description);
    v.input_schema = j[detail::kInputSchema];
    detail::DeserializeOptional(j, detail::kOutputSchema, v.output_schema);
    auto* ann = j.Find(detail::kAnnotations);
    if (ann) v.annotations = DeserializeToolAnnotations(*ann);
    auto* e = j.Find("execution");
    if (e) v.execution = DeserializeToolExecution(*e);
    v.icons = detail::DeserializeVector<Icon>(j, detail::kIcons,
        [](const JsonValue& ic) { return DeserializeIcon(ic); });
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    return v;
}

// ── Resource ──

JsonValue SerializeResource(const Resource& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kUri] = JsonValue(v.uri);
    obj[detail::kName] = JsonValue(v.name);
    detail::SerializeOptional(obj, detail::kTitle, v.title);
    detail::SerializeOptional(obj, detail::kDescription, v.description);
    detail::SerializeOptional(obj, detail::kMimeType, v.mime_type);
    detail::SerializeOptional(obj, "size", v.size);
    if (v.icons) {
        detail::SerializeVector(obj, detail::kIcons, *v.icons,
            [](const Icon& icon) { return SerializeIcon(icon); });
    }
    if (v.annotations) obj[detail::kAnnotations] = SerializeResourceAnnotations(*v.annotations);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

Resource DeserializeResource(const JsonValue& j) {
    Resource v;
    v.uri = j[detail::kUri].GetString();
    v.name = j[detail::kName].GetString();
    detail::DeserializeOptional(j, detail::kTitle, v.title);
    detail::DeserializeOptional(j, detail::kDescription, v.description);
    detail::DeserializeOptional(j, detail::kMimeType, v.mime_type);
    detail::DeserializeOptional(j, "size", v.size);
    auto* icons = j.Find(detail::kIcons);
    if (icons && icons->IsArray()) {
        v.icons = detail::DeserializeVector<Icon>(*icons,
            [](const JsonValue& ic) { return DeserializeIcon(ic); });
    }
    auto* ann = j.Find(detail::kAnnotations);
    if (ann) v.annotations = DeserializeResourceAnnotations(*ann);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    return v;
}

// ── ResourceTemplate ──

JsonValue SerializeResourceTemplate(const ResourceTemplate& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["uriTemplate"] = JsonValue(v.uri_template);
    obj[detail::kName] = JsonValue(v.name);
    detail::SerializeOptional(obj, detail::kTitle, v.title);
    detail::SerializeOptional(obj, detail::kDescription, v.description);
    detail::SerializeOptional(obj, detail::kMimeType, v.mime_type);
    if (v.icons) {
        detail::SerializeVector(obj, detail::kIcons, *v.icons,
            [](const Icon& icon) { return SerializeIcon(icon); });
    }
    if (v.annotations) obj[detail::kAnnotations] = SerializeResourceAnnotations(*v.annotations);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

ResourceTemplate DeserializeResourceTemplate(const JsonValue& j) {
    ResourceTemplate v;
    v.uri_template = j["uriTemplate"].GetString();
    v.name = j[detail::kName].GetString();
    detail::DeserializeOptional(j, detail::kTitle, v.title);
    detail::DeserializeOptional(j, detail::kDescription, v.description);
    detail::DeserializeOptional(j, detail::kMimeType, v.mime_type);
    auto* icons = j.Find(detail::kIcons);
    if (icons && icons->IsArray()) {
        v.icons = detail::DeserializeVector<Icon>(*icons,
            [](const JsonValue& ic) { return DeserializeIcon(ic); });
    }
    auto* ann = j.Find(detail::kAnnotations);
    if (ann) v.annotations = DeserializeResourceAnnotations(*ann);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    return v;
}

// ── PromptArgument ──

JsonValue SerializePromptArgument(const PromptArgument& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kName] = JsonValue(v.name);
    detail::SerializeOptional(obj, detail::kDescription, v.description);
    detail::SerializeOptional(obj, "required", v.required);
    return obj;
}

PromptArgument DeserializePromptArgument(const JsonValue& j) {
    PromptArgument v;
    v.name = j[detail::kName].GetString();
    detail::DeserializeOptional(j, detail::kDescription, v.description);
    detail::DeserializeOptional(j, "required", v.required);
    return v;
}

// ── Prompt ──

JsonValue SerializePrompt(const Prompt& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kName] = JsonValue(v.name);
    detail::SerializeOptional(obj, detail::kTitle, v.title);
    detail::SerializeOptional(obj, detail::kDescription, v.description);
    if (v.arguments) {
        detail::SerializeVector(obj, detail::kArguments, *v.arguments,
            [](const PromptArgument& arg) { return SerializePromptArgument(arg); });
    }
    if (v.icons) {
        detail::SerializeVector(obj, detail::kIcons, *v.icons,
            [](const Icon& icon) { return SerializeIcon(icon); });
    }
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

Prompt DeserializePrompt(const JsonValue& j) {
    Prompt v;
    v.name = j[detail::kName].GetString();
    detail::DeserializeOptional(j, detail::kTitle, v.title);
    detail::DeserializeOptional(j, detail::kDescription, v.description);
    auto* args = j.Find(detail::kArguments);
    if (args && args->IsArray()) {
        v.arguments = detail::DeserializeVector<PromptArgument>(*args,
            [](const JsonValue& av) { return DeserializePromptArgument(av); });
    }
    auto* icons = j.Find(detail::kIcons);
    if (icons && icons->IsArray()) {
        v.icons = detail::DeserializeVector<Icon>(*icons,
            [](const JsonValue& ic) { return DeserializeIcon(ic); });
    }
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    return v;
}

// ── PromptMessage ──

JsonValue SerializePromptMessage(const PromptMessage& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kRole] = JsonValue(v.role);
    obj[detail::kContent] = SerializeContentVariant(v.content);
    return obj;
}

PromptMessage DeserializePromptMessage(const JsonValue& j) {
    PromptMessage v;
    v.role = j[detail::kRole].GetString();
    v.content = DeserializeContentVariant(j[detail::kContent]);
    return v;
}

} // namespace mcp
