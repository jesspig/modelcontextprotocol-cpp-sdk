#include <mcp/Content.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/Meta.hpp>
#include <detail/JsonSerializer.hpp>

namespace mcp {

// Forward declarations from Capabilities.cpp
JsonValue SerializeClientCapabilities(const ClientCapabilities& v);
ClientCapabilities DeserializeClientCapabilities(const JsonValue& j);

// ── Icon ──

JsonValue SerializeIcon(const Icon& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["src"] = JsonValue(v.src);
    detail::SerializeOptional(obj, "mimeType", v.mime_type);
    if (v.sizes) {
        JsonValue::Array arr;
        for (const auto& s : *v.sizes) arr.push_back(JsonValue(s));
        obj["sizes"] = JsonValue(std::move(arr));
    }
    detail::SerializeOptional(obj, "theme", v.theme);
    return obj;
}

Icon DeserializeIcon(const JsonValue& j) {
    Icon v;
    v.src = j["src"].GetString();
    detail::DeserializeOptional(j, "mimeType", v.mime_type);
    auto* sz = j.Find("sizes");
    if (sz && sz->IsArray()) {
        std::vector<std::string> sizes;
        for (const auto& s : sz->GetArray()) sizes.push_back(s.GetString());
        v.sizes = std::move(sizes);
    }
    detail::DeserializeOptional(j, "theme", v.theme);
    return v;
}

// ── Annotations ──

JsonValue SerializeAnnotations(const Annotations& v) {
    JsonValue obj(JsonValue::object_tag);
    if (v.audience) {
        JsonValue::Array arr;
        for (const auto& s : *v.audience) arr.push_back(JsonValue(s));
        obj["audience"] = JsonValue(std::move(arr));
    }
    detail::SerializeOptional(obj, "priority", v.priority);
    detail::SerializeOptional(obj, "lastModified", v.last_modified);
    return obj;
}

Annotations DeserializeAnnotations(const JsonValue& j) {
    Annotations v;
    auto* aud = j.Find("audience");
    if (aud && aud->IsArray()) {
        std::vector<std::string> audience;
        for (const auto& s : aud->GetArray()) audience.push_back(s.GetString());
        v.audience = std::move(audience);
    }
    detail::DeserializeOptional(j, "priority", v.priority);
    detail::DeserializeOptional(j, "lastModified", v.last_modified);
    return v;
}

// ── TextResourceContents ──

JsonValue SerializeTextResourceContents(const TextResourceContents& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["uri"] = JsonValue(v.uri);
    obj["text"] = JsonValue(v.text);
    detail::SerializeOptional(obj, "mimeType", v.mime_type);
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

TextResourceContents DeserializeTextResourceContents(const JsonValue& j) {
    TextResourceContents v;
    v.uri = j["uri"].GetString();
    v.text = j["text"].GetString();
    detail::DeserializeOptional(j, "mimeType", v.mime_type);
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

// ── BlobResourceContents ──

JsonValue SerializeBlobResourceContents(const BlobResourceContents& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["uri"] = JsonValue(v.uri);
    obj["blob"] = JsonValue(v.blob);
    detail::SerializeOptional(obj, "mimeType", v.mime_type);
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

BlobResourceContents DeserializeBlobResourceContents(const JsonValue& j) {
    BlobResourceContents v;
    v.uri = j["uri"].GetString();
    v.blob = j["blob"].GetString();
    detail::DeserializeOptional(j, "mimeType", v.mime_type);
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

// ── ResourceContents ──

JsonValue SerializeResourceContents(const ResourceContents& rc) {
    return std::visit([](const auto& v) -> JsonValue {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, TextResourceContents>)
            return SerializeTextResourceContents(v);
        else
            return SerializeBlobResourceContents(v);
    }, rc);
}

ResourceContents DeserializeResourceContents(const JsonValue& j) {
    if (j.Find("text")) return DeserializeTextResourceContents(j);
    if (j.Find("blob")) return DeserializeBlobResourceContents(j);
    throw std::runtime_error("unknown ResourceContents type");
}

// ── TextContent ──

JsonValue SerializeTextContent(const TextContent& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["type"] = JsonValue(v.type);
    obj["text"] = JsonValue(v.text);
    detail::SerializeOptional(obj, "mimeType", v.mime_type);
    if (v.annotations) obj["annotations"] = SerializeAnnotations(*v.annotations);
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

TextContent DeserializeTextContent(const JsonValue& j) {
    TextContent v;
    v.type = j["type"].GetString();
    v.text = j["text"].GetString();
    detail::DeserializeOptional(j, "mimeType", v.mime_type);
    auto* ann = j.Find("annotations");
    if (ann) v.annotations = DeserializeAnnotations(*ann);
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

// ── ImageContent ──

JsonValue SerializeImageContent(const ImageContent& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["type"] = JsonValue(v.type);
    obj["data"] = JsonValue(v.data);
    obj["mimeType"] = JsonValue(v.mime_type);
    if (v.annotations) obj["annotations"] = SerializeAnnotations(*v.annotations);
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

ImageContent DeserializeImageContent(const JsonValue& j) {
    ImageContent v;
    v.type = j["type"].GetString();
    v.data = j["data"].GetString();
    v.mime_type = j["mimeType"].GetString();
    auto* ann = j.Find("annotations");
    if (ann) v.annotations = DeserializeAnnotations(*ann);
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

// ── AudioContent ──

JsonValue SerializeAudioContent(const AudioContent& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["type"] = JsonValue(v.type);
    obj["data"] = JsonValue(v.data);
    obj["mimeType"] = JsonValue(v.mime_type);
    if (v.annotations) obj["annotations"] = SerializeAnnotations(*v.annotations);
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

AudioContent DeserializeAudioContent(const JsonValue& j) {
    AudioContent v;
    v.type = j["type"].GetString();
    v.data = j["data"].GetString();
    v.mime_type = j["mimeType"].GetString();
    auto* ann = j.Find("annotations");
    if (ann) v.annotations = DeserializeAnnotations(*ann);
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

// ── EmbeddedResource ──

JsonValue SerializeEmbeddedResource(const EmbeddedResource& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["type"] = JsonValue(v.type);
    obj["resource"] = SerializeResourceContents(v.resource);
    if (v.annotations) obj["annotations"] = SerializeAnnotations(*v.annotations);
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

EmbeddedResource DeserializeEmbeddedResource(const JsonValue& j) {
    EmbeddedResource v;
    v.type = j["type"].GetString();
    auto* res = j.Find("resource");
    if (res) v.resource = DeserializeResourceContents(*res);
    auto* ann = j.Find("annotations");
    if (ann) v.annotations = DeserializeAnnotations(*ann);
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

// ── ResourceLink ──

JsonValue SerializeResourceLink(const ResourceLink& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["type"] = JsonValue(v.type);
    obj["uri"] = JsonValue(v.uri);
    detail::SerializeOptional(obj, "title", v.title);
    if (v.annotations) obj["annotations"] = SerializeAnnotations(*v.annotations);
    detail::SerializeOptional(obj, "_meta", v.meta);
    return obj;
}

ResourceLink DeserializeResourceLink(const JsonValue& j) {
    ResourceLink v;
    v.type = j["type"].GetString();
    v.uri = j["uri"].GetString();
    detail::DeserializeOptional(j, "title", v.title);
    auto* ann = j.Find("annotations");
    if (ann) v.annotations = DeserializeAnnotations(*ann);
    detail::DeserializeOptional(j, "_meta", v.meta);
    return v;
}

// ── ContentVariant ──

JsonValue SerializeContentVariant(const ContentVariant& content) {
    return std::visit([](const auto& v) -> JsonValue {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, TextContent>)
            return SerializeTextContent(v);
        else if constexpr (std::is_same_v<T, ImageContent>)
            return SerializeImageContent(v);
        else if constexpr (std::is_same_v<T, AudioContent>)
            return SerializeAudioContent(v);
        else if constexpr (std::is_same_v<T, EmbeddedResource>)
            return SerializeEmbeddedResource(v);
        else
            return SerializeResourceLink(v);
    }, content);
}

ContentVariant DeserializeContentVariant(const JsonValue& j) {
    auto type = j["type"].GetString();
    if (type == "text")          return DeserializeTextContent(j);
    if (type == "image")         return DeserializeImageContent(j);
    if (type == "audio")         return DeserializeAudioContent(j);
    if (type == "resource")      return DeserializeEmbeddedResource(j);
    if (type == "resource_link") return DeserializeResourceLink(j);
    throw std::runtime_error(std::string("unknown Content type: ") + type);
}

// ── Implementation ──

JsonValue SerializeImplementation(const Implementation& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["name"] = JsonValue(v.name);
    obj["version"] = JsonValue(v.version);
    detail::SerializeOptional(obj, "title", v.title);
    if (!v.icons.empty()) {
        JsonValue::Array arr;
        for (const auto& icon : v.icons) arr.push_back(SerializeIcon(icon));
        obj["icons"] = JsonValue(std::move(arr));
    }
    detail::SerializeOptional(obj, "description", v.description);
    detail::SerializeOptional(obj, "websiteUrl", v.website_url);
    return obj;
}

Implementation DeserializeImplementation(const JsonValue& j) {
    Implementation v;
    v.name = j["name"].GetString();
    v.version = j["version"].GetString();
    detail::DeserializeOptional(j, "title", v.title);
    auto* icons = j.Find("icons");
    if (icons && icons->IsArray()) {
        std::vector<Icon> ivec;
        for (const auto& iv : icons->GetArray()) ivec.push_back(DeserializeIcon(iv));
        v.icons = std::move(ivec);
    }
    detail::DeserializeOptional(j, "description", v.description);
    detail::DeserializeOptional(j, "websiteUrl", v.website_url);
    return v;
}

// ── ProgressToken ──

JsonValue SerializeProgressToken(const ProgressToken& pt) {
    return std::visit([](const auto& v) -> JsonValue {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>)
            return JsonValue(v);
        else
            return JsonValue(v);
    }, pt);
}

ProgressToken DeserializeProgressToken(const JsonValue& j) {
    if (j.IsString()) return j.GetString();
    return j.GetInt();
}

// ── LoggingLevel ──

static const char* kLoggingLevelNames[] = {
    "debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"
};

JsonValue SerializeLoggingLevel(LoggingLevel l) {
    auto i = static_cast<int>(l);
    return JsonValue((i >= 0 && i < 8) ? kLoggingLevelNames[i] : "debug");
}

LoggingLevel DeserializeLoggingLevel(const JsonValue& j) {
    auto s = j.GetString();
    for (int i = 0; i < 8; ++i) {
        if (s == kLoggingLevelNames[i]) return static_cast<LoggingLevel>(i);
    }
    throw std::runtime_error(std::string("unknown LoggingLevel: ") + s);
}

// ── CacheHint ──

JsonValue SerializeCacheHint(const CacheHint& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "ttlMs", v.ttl_ms);
    detail::SerializeOptional(obj, "cacheScope", v.cache_scope);
    return obj;
}

CacheHint DeserializeCacheHint(const JsonValue& j) {
    CacheHint v;
    detail::DeserializeOptional(j, "ttlMs", v.ttl_ms);
    detail::DeserializeOptional(j, "cacheScope", v.cache_scope);
    return v;
}

// ── RequestMeta ──

JsonValue SerializeRequestMeta(const RequestMeta& v) {
    JsonValue obj(JsonValue::object_tag);
    if (v.progress_token) obj["progressToken"] = SerializeProgressToken(*v.progress_token);
    obj["io.modelcontextprotocol/protocolVersion"] = JsonValue(v.protocol_version);
    if (v.client_info) obj["io.modelcontextprotocol/clientInfo"] = SerializeImplementation(*v.client_info);
    if (v.client_capabilities) obj["io.modelcontextprotocol/clientCapabilities"] = SerializeClientCapabilities(*v.client_capabilities);
    if (v.log_level) obj["io.modelcontextprotocol/logLevel"] = SerializeLoggingLevel(*v.log_level);
    if (v.extensions) {
        for (const auto& [k, val] : v.extensions->GetObject()) obj[k] = val;
    }
    return obj;
}

RequestMeta DeserializeRequestMeta(const JsonValue& j) {
    RequestMeta v;
    auto* pt = j.Find("progressToken");
    if (pt) v.progress_token = DeserializeProgressToken(*pt);
    auto* pv = j.Find("io.modelcontextprotocol/protocolVersion");
    if (pv) v.protocol_version = pv->GetString();
    auto* ci = j.Find("io.modelcontextprotocol/clientInfo");
    if (ci) v.client_info = DeserializeImplementation(*ci);
    auto* cc = j.Find("io.modelcontextprotocol/clientCapabilities");
    if (cc) v.client_capabilities = DeserializeClientCapabilities(*cc);
    auto* ll = j.Find("io.modelcontextprotocol/logLevel");
    if (ll) v.log_level = DeserializeLoggingLevel(*ll);
    // ponytail: extensions skipped in deserialize (old code didn't read them back either)
    return v;
}

} // namespace mcp
