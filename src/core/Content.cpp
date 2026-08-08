// Content.cpp — Content type serialization/deserialization implementations

#include <mcp/Content.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/Meta.hpp>
#include <mcp/McpError.hpp>
#include <detail/JsonFields.hpp>
#include <detail/JsonSerializer.hpp>

namespace mcp {

// Forward declarations from Capabilities.cpp
JsonValue SerializeClientCapabilities(const ClientCapabilities& v);
ClientCapabilities DeserializeClientCapabilities(const JsonValue& j);

namespace {

// Known RequestMeta field keys that are stored in dedicated members rather
// than in the extensions bag (mirrors SerializeRequestMeta's output).
bool RequestMetaFieldIsKnown(std::string_view key) {
    return key == detail::kProgressToken
        || key == "io.modelcontextprotocol/protocolVersion"
        || key == "io.modelcontextprotocol/clientInfo"
        || key == "io.modelcontextprotocol/clientCapabilities"
        || key == "io.modelcontextprotocol/logLevel"
        || key == "io.modelcontextprotocol/subscriptionId"
        || key == "traceparent"
        || key == "tracestate"
        || key == "baggage";
}

} // namespace

// ── Icon ──

JsonValue SerializeIcon(const Icon& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["src"] = JsonValue(v.src);
    detail::SerializeOptional(obj, detail::kMimeType, v.mime_type);
    if (v.sizes) {
        detail::SerializeVector(obj, "sizes", *v.sizes,
            [](const std::string& s) { return JsonValue(s); });
    }
    detail::SerializeOptional(obj, "theme", v.theme);
    return obj;
}

Icon DeserializeIcon(const JsonValue& j) {
    Icon v;
    v.src = j["src"].GetString();
    detail::DeserializeOptional(j, detail::kMimeType, v.mime_type);
    auto* sz = j.Find("sizes");
    if (sz && sz->IsArray()) {
        v.sizes = detail::DeserializeVector<std::string>(*sz,
            [](const JsonValue& s) { return s.GetString(); });
    }
    detail::DeserializeOptional(j, "theme", v.theme);
    return v;
}

// ── Annotations ──

JsonValue SerializeAnnotations(const Annotations& v) {
    JsonValue obj(JsonValue::object_tag);
    if (v.audience) {
        detail::SerializeVector(obj, detail::kAudience, *v.audience,
            [](const std::string& s) { return JsonValue(s); });
    }
    detail::SerializeOptional(obj, detail::kPriority, v.priority);
    detail::SerializeOptional(obj, detail::kLastModified, v.last_modified);
    return obj;
}

Annotations DeserializeAnnotations(const JsonValue& j) {
    Annotations v;
    auto* aud = j.Find(detail::kAudience);
    if (aud && aud->IsArray()) {
        v.audience = detail::DeserializeVector<std::string>(*aud,
            [](const JsonValue& s) { return s.GetString(); });
    }
    detail::DeserializeOptional(j, detail::kPriority, v.priority);
    detail::DeserializeOptional(j, detail::kLastModified, v.last_modified);
    return v;
}

// ── TextResourceContents ──

JsonValue SerializeTextResourceContents(const TextResourceContents& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kUri] = JsonValue(v.uri);
    obj[detail::kText] = JsonValue(v.text);
    detail::SerializeOptional(obj, detail::kMimeType, v.mime_type);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

TextResourceContents DeserializeTextResourceContents(const JsonValue& j) {
    TextResourceContents v;
    v.uri = j[detail::kUri].GetString();
    v.text = j[detail::kText].GetString();
    detail::DeserializeOptional(j, detail::kMimeType, v.mime_type);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    return v;
}

// ── BlobResourceContents ──

JsonValue SerializeBlobResourceContents(const BlobResourceContents& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kUri] = JsonValue(v.uri);
    obj[detail::kBlob] = JsonValue(v.blob);
    detail::SerializeOptional(obj, detail::kMimeType, v.mime_type);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

BlobResourceContents DeserializeBlobResourceContents(const JsonValue& j) {
    BlobResourceContents v;
    v.uri = j[detail::kUri].GetString();
    v.blob = j[detail::kBlob].GetString();
    detail::DeserializeOptional(j, detail::kMimeType, v.mime_type);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
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

// Heuristic dispatch: presence of "text" or "blob" determines resource content type.
ResourceContents DeserializeResourceContents(const JsonValue& j) {
    if (j.Find(detail::kText)) return DeserializeTextResourceContents(j);
    if (j.Find(detail::kBlob)) return DeserializeBlobResourceContents(j);
    throw McpError(McpErrorCode::DeserializeFailed,
        std::string("unknown ResourceContents type: neither 'text' nor 'blob' field present, got ") +
        detail::JsonValueTypeName(j));
}

// ── TextContent ──

JsonValue SerializeTextContent(const TextContent& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kType] = JsonValue(v.type);
    obj[detail::kText] = JsonValue(v.text);
    detail::SerializeOptional(obj, detail::kMimeType, v.mime_type);
    if (v.annotations) obj[detail::kAnnotations] = SerializeAnnotations(*v.annotations);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

TextContent DeserializeTextContent(const JsonValue& j) {
    TextContent v;
    v.type = j[detail::kType].GetString();
    v.text = j[detail::kText].GetString();
    detail::DeserializeOptional(j, detail::kMimeType, v.mime_type);
    auto* ann = j.Find(detail::kAnnotations);
    if (ann) v.annotations = DeserializeAnnotations(*ann);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    return v;
}

// ── ImageContent ──

JsonValue SerializeImageContent(const ImageContent& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kType] = JsonValue(v.type);
    obj[detail::kData] = JsonValue(v.data);
    obj[detail::kMimeType] = JsonValue(v.mime_type);
    if (v.annotations) obj[detail::kAnnotations] = SerializeAnnotations(*v.annotations);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

ImageContent DeserializeImageContent(const JsonValue& j) {
    ImageContent v;
    v.type = j[detail::kType].GetString();
    v.data = j[detail::kData].GetString();
    v.mime_type = j[detail::kMimeType].GetString();
    auto* ann = j.Find(detail::kAnnotations);
    if (ann) v.annotations = DeserializeAnnotations(*ann);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    return v;
}

// ── AudioContent ──

JsonValue SerializeAudioContent(const AudioContent& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kType] = JsonValue(v.type);
    obj[detail::kData] = JsonValue(v.data);
    obj[detail::kMimeType] = JsonValue(v.mime_type);
    if (v.annotations) obj[detail::kAnnotations] = SerializeAnnotations(*v.annotations);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

AudioContent DeserializeAudioContent(const JsonValue& j) {
    AudioContent v;
    v.type = j[detail::kType].GetString();
    v.data = j[detail::kData].GetString();
    v.mime_type = j[detail::kMimeType].GetString();
    auto* ann = j.Find(detail::kAnnotations);
    if (ann) v.annotations = DeserializeAnnotations(*ann);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    return v;
}

// ── EmbeddedResource ──

JsonValue SerializeEmbeddedResource(const EmbeddedResource& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kType] = JsonValue(v.type);
    obj[detail::kResource] = SerializeResourceContents(v.resource);
    if (v.annotations) obj[detail::kAnnotations] = SerializeAnnotations(*v.annotations);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

EmbeddedResource DeserializeEmbeddedResource(const JsonValue& j) {
    EmbeddedResource v;
    v.type = j[detail::kType].GetString();
    auto* res = j.Find(detail::kResource);
    if (res) v.resource = DeserializeResourceContents(*res);
    auto* ann = j.Find(detail::kAnnotations);
    if (ann) v.annotations = DeserializeAnnotations(*ann);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
    return v;
}

// ── ResourceLink ──

JsonValue SerializeResourceLink(const ResourceLink& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kType] = JsonValue(v.type);
    obj[detail::kUri] = JsonValue(v.uri);
    detail::SerializeOptional(obj, detail::kTitle, v.title);
    if (v.annotations) obj[detail::kAnnotations] = SerializeAnnotations(*v.annotations);
    detail::SerializeOptional(obj, detail::kMeta, v.meta);
    return obj;
}

ResourceLink DeserializeResourceLink(const JsonValue& j) {
    ResourceLink v;
    v.type = j[detail::kType].GetString();
    v.uri = j[detail::kUri].GetString();
    detail::DeserializeOptional(j, detail::kTitle, v.title);
    auto* ann = j.Find(detail::kAnnotations);
    if (ann) v.annotations = DeserializeAnnotations(*ann);
    detail::DeserializeOptional(j, detail::kMeta, v.meta);
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

// Dispatch to the correct content deserializer based on the "type" field.
ContentVariant DeserializeContentVariant(const JsonValue& j) {
    auto type = j[detail::kType].GetString();
    if (type == "text")          return DeserializeTextContent(j);
    if (type == "image")         return DeserializeImageContent(j);
    if (type == "audio")         return DeserializeAudioContent(j);
    if (type == "resource")      return DeserializeEmbeddedResource(j);
    if (type == "resource_link") return DeserializeResourceLink(j);
    throw McpError(McpErrorCode::DeserializeFailed,
        std::string("unknown Content type: ") + type);
}

// ── Implementation ──

JsonValue SerializeImplementation(const Implementation& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kName] = JsonValue(v.name);
    obj["version"] = JsonValue(v.version);
    detail::SerializeOptional(obj, detail::kTitle, v.title);
    detail::SerializeVector(obj, detail::kIcons, v.icons,
        [](const Icon& icon) { return SerializeIcon(icon); });
    detail::SerializeOptional(obj, detail::kDescription, v.description);
    detail::SerializeOptional(obj, "websiteUrl", v.website_url);
    return obj;
}

Implementation DeserializeImplementation(const JsonValue& j) {
    Implementation v;
    v.name = j[detail::kName].GetString();
    v.version = j["version"].GetString();
    detail::DeserializeOptional(j, detail::kTitle, v.title);
    v.icons = detail::DeserializeVector<Icon>(j, detail::kIcons,
        [](const JsonValue& iv) { return DeserializeIcon(iv); });
    detail::DeserializeOptional(j, detail::kDescription, v.description);
    detail::DeserializeOptional(j, "websiteUrl", v.website_url);
    return v;
}

// ── ProgressToken ──

JsonValue SerializeProgressToken(const ProgressToken& pt) {
    return std::visit([](const auto& v) -> JsonValue {
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
    if (i < 0 || i >= 8)
        throw McpError(McpErrorCode::InvalidParams,
            "SerializeLoggingLevel: out of range value: " + std::to_string(i));
    return JsonValue(kLoggingLevelNames[i]);
}

LoggingLevel DeserializeLoggingLevel(const JsonValue& j) {
    auto s = j.GetString();
    for (int i = 0; i < 8; ++i) {
        if (s == kLoggingLevelNames[i]) return static_cast<LoggingLevel>(i);
    }
    throw McpError(McpErrorCode::InvalidParams,
        std::string("DeserializeLoggingLevel: unknown level string: '") + s + "'");
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
    if (v.progress_token) obj[detail::kProgressToken] = SerializeProgressToken(*v.progress_token);
    obj["io.modelcontextprotocol/protocolVersion"] = JsonValue(v.protocol_version);
    if (v.client_info) obj["io.modelcontextprotocol/clientInfo"] = SerializeImplementation(*v.client_info);
    if (v.client_capabilities) obj["io.modelcontextprotocol/clientCapabilities"] = SerializeClientCapabilities(*v.client_capabilities);
    if (v.log_level) obj["io.modelcontextprotocol/logLevel"] = SerializeLoggingLevel(*v.log_level);
    if (v.extensions) {
        for (const auto& [k, val] : v.extensions->GetObject()) obj[k] = val;
    }
    if (v.traceparent) obj["traceparent"] = JsonValue(*v.traceparent);
    if (v.tracestate) obj["tracestate"] = JsonValue(*v.tracestate);
    if (v.baggage) obj["baggage"] = JsonValue(*v.baggage);
    return obj;
}

RequestMeta DeserializeRequestMeta(const JsonValue& j) {
    RequestMeta v;
    auto* pt = j.Find(detail::kProgressToken);
    if (pt) v.progress_token = DeserializeProgressToken(*pt);
    auto* pv = j.Find("io.modelcontextprotocol/protocolVersion");
    if (pv) v.protocol_version = pv->GetString();
    auto* ci = j.Find("io.modelcontextprotocol/clientInfo");
    if (ci) v.client_info = DeserializeImplementation(*ci);
    auto* cc = j.Find("io.modelcontextprotocol/clientCapabilities");
    if (cc) v.client_capabilities = DeserializeClientCapabilities(*cc);
    auto* ll = j.Find("io.modelcontextprotocol/logLevel");
    if (ll) v.log_level = DeserializeLoggingLevel(*ll);
    if (auto* tp = j.Find("traceparent")) v.traceparent = tp->GetString();
    if (auto* ts = j.Find("tracestate")) v.tracestate = ts->GetString();
    if (auto* bg = j.Find("baggage")) v.baggage = bg->GetString();

    // Round-trip symmetry with SerializeRequestMeta: keys that are not
    // dedicated members are preserved in the extensions bag.
    JsonValue ext(JsonValue::object_tag);
    for (const auto& [k, val] : j.GetObject()) {
        if (!RequestMetaFieldIsKnown(k)) ext[k] = val;
    }
    if (!ext.Empty()) v.extensions = std::move(ext);
    return v;
}

} // namespace mcp
