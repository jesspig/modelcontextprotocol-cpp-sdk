#include <mcp/Capabilities.hpp>
#include <detail/JsonSerializer.hpp>

namespace mcp {

// ── ToolsCapability ──

JsonValue SerializeToolsCapability(const ToolsCapability& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "listChanged", v.list_changed);
    return obj;
}

ToolsCapability DeserializeToolsCapability(const JsonValue& j) {
    ToolsCapability v;
    detail::DeserializeOptional(j, "listChanged", v.list_changed);
    return v;
}

// ── ResourcesCapability ──

JsonValue SerializeResourcesCapability(const ResourcesCapability& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "subscribe", v.subscribe);
    detail::SerializeOptional(obj, "listChanged", v.list_changed);
    return obj;
}

ResourcesCapability DeserializeResourcesCapability(const JsonValue& j) {
    ResourcesCapability v;
    detail::DeserializeOptional(j, "subscribe", v.subscribe);
    detail::DeserializeOptional(j, "listChanged", v.list_changed);
    return v;
}

// ── PromptsCapability ──

JsonValue SerializePromptsCapability(const PromptsCapability& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "listChanged", v.list_changed);
    return obj;
}

PromptsCapability DeserializePromptsCapability(const JsonValue& j) {
    PromptsCapability v;
    detail::DeserializeOptional(j, "listChanged", v.list_changed);
    return v;
}

// ── SamplingCapability ──

JsonValue SerializeSamplingCapability(const SamplingCapability&) {
    return JsonValue(JsonValue::object_tag);
}

SamplingCapability DeserializeSamplingCapability(const JsonValue&) {
    return SamplingCapability{};
}

// ── RootsCapability ──

JsonValue SerializeRootsCapability(const RootsCapability& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "listChanged", v.list_changed);
    return obj;
}

RootsCapability DeserializeRootsCapability(const JsonValue& j) {
    RootsCapability v;
    detail::DeserializeOptional(j, "listChanged", v.list_changed);
    return v;
}

// ── ElicitationCapability ──

JsonValue SerializeElicitationCapability(const ElicitationCapability& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "form", v.form);
    detail::SerializeOptional(obj, "url", v.url);
    return obj;
}

ElicitationCapability DeserializeElicitationCapability(const JsonValue& j) {
    ElicitationCapability v;
    detail::DeserializeOptional(j, "form", v.form);
    detail::DeserializeOptional(j, "url", v.url);
    return v;
}

// ── EmptyCapability ──

JsonValue SerializeEmptyCapability(const EmptyCapability&) {
    return JsonValue(JsonValue::object_tag);
}

EmptyCapability DeserializeEmptyCapability(const JsonValue&) {
    return EmptyCapability{};
}

// ── ServerCapabilities ──

JsonValue SerializeServerCapabilities(const ServerCapabilities& v) {
    JsonValue obj(JsonValue::object_tag);
    if (v.tools) obj["tools"] = SerializeToolsCapability(*v.tools);
    if (v.resources) obj["resources"] = SerializeResourcesCapability(*v.resources);
    if (v.prompts) obj["prompts"] = SerializePromptsCapability(*v.prompts);
    if (v.logging) obj["logging"] = SerializeEmptyCapability(*v.logging);
    if (v.sampling) obj["sampling"] = SerializeSamplingCapability(*v.sampling);
    if (v.roots) obj["roots"] = SerializeRootsCapability(*v.roots);
    if (v.elicitation) obj["elicitation"] = SerializeElicitationCapability(*v.elicitation);
    if (v.completions) obj["completions"] = SerializeEmptyCapability(*v.completions);
    if (v.subscriptions) obj["subscriptions"] = SerializeEmptyCapability(*v.subscriptions);
    if (v.extensions) {
        JsonValue ext(JsonValue::object_tag);
        for (const auto& [k, val] : *v.extensions) ext[k] = val;
        obj["extensions"] = std::move(ext);
    }
    detail::SerializeOptional(obj, "experimental", v.experimental);
    return obj;
}

ServerCapabilities DeserializeServerCapabilities(const JsonValue& j) {
    ServerCapabilities v;
    auto* t = j.Find("tools");
    if (t && t->IsObject()) v.tools = DeserializeToolsCapability(*t);
    auto* r = j.Find("resources");
    if (r && r->IsObject()) v.resources = DeserializeResourcesCapability(*r);
    auto* p = j.Find("prompts");
    if (p && p->IsObject()) v.prompts = DeserializePromptsCapability(*p);
    auto* l = j.Find("logging");
    if (l && l->IsObject()) v.logging = DeserializeEmptyCapability(*l);
    auto* s = j.Find("sampling");
    if (s && s->IsObject()) v.sampling = DeserializeSamplingCapability(*s);
    auto* rt = j.Find("roots");
    if (rt && rt->IsObject()) v.roots = DeserializeRootsCapability(*rt);
    auto* e = j.Find("elicitation");
    if (e && e->IsObject()) v.elicitation = DeserializeElicitationCapability(*e);
    auto* c = j.Find("completions");
    if (c && c->IsObject()) v.completions = DeserializeEmptyCapability(*c);
    auto* sub = j.Find("subscriptions");
    if (sub && sub->IsObject()) v.subscriptions = DeserializeEmptyCapability(*sub);
    auto* ext = j.Find("extensions");
    if (ext && ext->IsObject()) {
        std::map<std::string, JsonValue> exts;
        for (const auto& [k, val] : ext->GetObject()) exts[k] = val;
        v.extensions = std::move(exts);
    }
    detail::DeserializeOptional(j, "experimental", v.experimental);
    return v;
}

// ── ClientCapabilities ──

JsonValue SerializeClientCapabilities(const ClientCapabilities& v) {
    JsonValue obj(JsonValue::object_tag);
    if (v.roots) obj["roots"] = SerializeRootsCapability(*v.roots);
    if (v.sampling) obj["sampling"] = SerializeSamplingCapability(*v.sampling);
    if (v.elicitation) obj["elicitation"] = SerializeElicitationCapability(*v.elicitation);
    if (v.extensions) {
        JsonValue ext(JsonValue::object_tag);
        for (const auto& [k, val] : *v.extensions) ext[k] = val;
        obj["extensions"] = std::move(ext);
    }
    detail::SerializeOptional(obj, "experimental", v.experimental);
    return obj;
}

ClientCapabilities DeserializeClientCapabilities(const JsonValue& j) {
    ClientCapabilities v;
    auto* rt = j.Find("roots");
    if (rt && rt->IsObject()) v.roots = DeserializeRootsCapability(*rt);
    auto* s = j.Find("sampling");
    if (s && s->IsObject()) v.sampling = DeserializeSamplingCapability(*s);
    auto* e = j.Find("elicitation");
    if (e && e->IsObject()) v.elicitation = DeserializeElicitationCapability(*e);
    auto* ext = j.Find("extensions");
    if (ext && ext->IsObject()) {
        std::map<std::string, JsonValue> exts;
        for (const auto& [k, val] : ext->GetObject()) exts[k] = val;
        v.extensions = std::move(exts);
    }
    detail::DeserializeOptional(j, "experimental", v.experimental);
    return v;
}

} // namespace mcp
