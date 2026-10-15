#include <mcp/McpTypes.hpp>
#include <mcp/McpError.hpp>
#include <detail/JsonFields.hpp>
#include <detail/JsonSerializer.hpp>

namespace mcp {

namespace {

constexpr int64_t kSkillsDefaultTtlMs = 0;
constexpr char kSkillsDefaultCacheScope[] = "private";

} // anonymous namespace

JsonValue SerializeSkillResource(const SkillResource& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kUri] = JsonValue(v.uri);
    obj[detail::kDigest] = JsonValue(v.digest);
    obj[detail::kSize] = JsonValue(v.size);
    return obj;
}

SkillResource DeserializeSkillResource(const JsonValue& j) {
    SkillResource v;
    v.uri = j[detail::kUri].GetString();
    v.digest = j[detail::kDigest].GetString();
    auto* size = j.Find(detail::kSize);
    if (size && size->IsInt()) v.size = size->GetInt();
    return v;
}

JsonValue SerializeSkillResources(const SkillResources& v) {
    if (auto* s = std::get_if<std::string>(&v)) return JsonValue(*s);
    const auto& vec = std::get<std::vector<SkillResource>>(v);
    JsonValue::Array arr;
    for (const auto& r : vec) arr.push_back(SerializeSkillResource(r));
    return JsonValue(std::move(arr));
}

SkillResources DeserializeSkillResources(const JsonValue& j) {
    if (j.IsString()) return SkillResources{j.GetString()};
    if (j.IsArray()) {
        std::vector<SkillResource> vec;
        for (const auto& elem : j.GetArray()) vec.push_back(DeserializeSkillResource(elem));
        return SkillResources{std::move(vec)};
    }
    throw McpError(McpErrorCode::DeserializeFailed,
        std::string("DeserializeSkillResources: expected array or string, got ") +
        detail::JsonValueTypeName(j));
}

JsonValue SerializeSkill(const Skill& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kUri] = JsonValue(v.uri);
    obj[detail::kFrontmatter] = v.frontmatter;
    obj[detail::kResources] = SerializeSkillResources(v.resources);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

Skill DeserializeSkill(const JsonValue& j) {
    Skill v;
    v.uri = j[detail::kUri].GetString();
    if (auto* fm = j.Find(detail::kFrontmatter); fm) v.frontmatter = *fm;
    if (auto* rs = j.Find(detail::kResources); rs) v.resources = DeserializeSkillResources(*rs);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    return v;
}

JsonValue SerializeSkillsListResult(const SkillsListResult& v) {
    JsonValue obj(JsonValue::object_tag);
    JsonValue::Array arr;
    for (const auto& s : v.skills) arr.push_back(SerializeSkill(s));
    obj[detail::kSkills] = JsonValue(std::move(arr));
    detail::SerializeOptional(obj, detail::kNextCursor, v.next_cursor);
    if (v.ttl_ms) {
        obj[detail::kTTLMs] = JsonValue(*v.ttl_ms);
    } else {
        obj[detail::kTTLMs] = JsonValue(kSkillsDefaultTtlMs);
    }
    if (v.cache_scope) {
        obj[detail::kCacheScope] = JsonValue(*v.cache_scope);
    } else {
        obj[detail::kCacheScope] = JsonValue(kSkillsDefaultCacheScope);
    }
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    obj[detail::kResultType] = SerializeResultType(v.result_type);
    return obj;
}

SkillsListResult DeserializeSkillsListResult(const JsonValue& j) {
    SkillsListResult v;
    auto* skills = j.Find(detail::kSkills);
    if (skills && skills->IsArray()) {
        for (const auto& elem : skills->GetArray()) v.skills.push_back(DeserializeSkill(elem));
    }
    detail::DeserializeOptional(j, detail::kNextCursor, v.next_cursor);
    detail::DeserializeOptional(j, detail::kTTLMs, v.ttl_ms);
    detail::DeserializeOptional(j, detail::kCacheScope, v.cache_scope);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    auto* rt = j.Find(detail::kResultType);
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

JsonValue SerializeSkillsGetResult(const SkillsGetResult& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kSkill] = SerializeSkill(v.skill);
    if (v.ttl_ms) {
        obj[detail::kTTLMs] = JsonValue(*v.ttl_ms);
    } else {
        obj[detail::kTTLMs] = JsonValue(kSkillsDefaultTtlMs);
    }
    if (v.cache_scope) {
        obj[detail::kCacheScope] = JsonValue(*v.cache_scope);
    } else {
        obj[detail::kCacheScope] = JsonValue(kSkillsDefaultCacheScope);
    }
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    obj[detail::kResultType] = SerializeResultType(v.result_type);
    return obj;
}

SkillsGetResult DeserializeSkillsGetResult(const JsonValue& j) {
    SkillsGetResult v;
    if (auto* skill = j.Find(detail::kSkill); skill) v.skill = DeserializeSkill(*skill);
    detail::DeserializeOptional(j, detail::kTTLMs, v.ttl_ms);
    detail::DeserializeOptional(j, detail::kCacheScope, v.cache_scope);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    auto* rt = j.Find(detail::kResultType);
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

JsonValue SerializeSkillsGetRequestParams(const SkillsGetRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kUri] = JsonValue(v.uri);
    if (v.meta) obj[detail::kMeta] = SerializeRequestMeta(*v.meta);
    return obj;
}

SkillsGetRequestParams DeserializeSkillsGetRequestParams(const JsonValue& j) {
    SkillsGetRequestParams v;
    v.uri = j[detail::kUri].GetString();
    auto* m = j.Find(detail::kMeta);
    if (m) v.meta = DeserializeRequestMeta(*m);
    return v;
}

JsonValue SerializeDirectoryChild(const DirectoryChild& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kUri] = JsonValue(v.uri);
    obj[detail::kName] = JsonValue(v.name);
    detail::SerializeOptional(obj, detail::kMimeType, v.mime_type);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

DirectoryChild DeserializeDirectoryChild(const JsonValue& j) {
    DirectoryChild v;
    v.uri = j[detail::kUri].GetString();
    v.name = j[detail::kName].GetString();
    detail::DeserializeOptional(j, detail::kMimeType, v.mime_type);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    return v;
}

JsonValue SerializeReadDirectoryResult(const ReadDirectoryResult& v) {
    JsonValue obj(JsonValue::object_tag);
    JsonValue::Array arr;
    for (const auto& child : v.resources) arr.push_back(SerializeDirectoryChild(child));
    obj[detail::kResources] = JsonValue(std::move(arr));
    detail::SerializeOptional(obj, detail::kNextCursor, v.next_cursor);
    if (v.ttl_ms) {
        obj[detail::kTTLMs] = JsonValue(*v.ttl_ms);
    } else {
        obj[detail::kTTLMs] = JsonValue(kSkillsDefaultTtlMs);
    }
    if (v.cache_scope) {
        obj[detail::kCacheScope] = JsonValue(*v.cache_scope);
    } else {
        obj[detail::kCacheScope] = JsonValue(kSkillsDefaultCacheScope);
    }
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    obj[detail::kResultType] = SerializeResultType(v.result_type);
    return obj;
}

ReadDirectoryResult DeserializeReadDirectoryResult(const JsonValue& j) {
    ReadDirectoryResult v;
    auto* resources = j.Find(detail::kResources);
    if (resources && resources->IsArray()) {
        for (const auto& elem : resources->GetArray())
            v.resources.push_back(DeserializeDirectoryChild(elem));
    }
    detail::DeserializeOptional(j, detail::kNextCursor, v.next_cursor);
    detail::DeserializeOptional(j, detail::kTTLMs, v.ttl_ms);
    detail::DeserializeOptional(j, detail::kCacheScope, v.cache_scope);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    auto* rt = j.Find(detail::kResultType);
    if (rt) v.result_type = DeserializeResultType(*rt);
    return v;
}

JsonValue SerializeReadDirectoryRequestParams(const ReadDirectoryRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kUri] = JsonValue(v.uri);
    detail::SerializeOptional(obj, detail::kCursor, v.cursor);
    if (v.meta) obj[detail::kMeta] = SerializeRequestMeta(*v.meta);
    return obj;
}

ReadDirectoryRequestParams DeserializeReadDirectoryRequestParams(const JsonValue& j) {
    ReadDirectoryRequestParams v;
    v.uri = j[detail::kUri].GetString();
    detail::DeserializeOptional(j, detail::kCursor, v.cursor);
    auto* m = j.Find(detail::kMeta);
    if (m) v.meta = DeserializeRequestMeta(*m);
    return v;
}

} // namespace mcp
