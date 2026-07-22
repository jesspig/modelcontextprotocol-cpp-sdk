#pragma once

#include <mcp/JsonValue.hpp>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mcp {

struct Icon {
    std::string src;
    std::optional<std::string> mime_type = std::nullopt;
    std::optional<std::vector<std::string>> sizes = std::nullopt;
    std::optional<std::string> theme = std::nullopt;
};

struct Annotations {
    std::optional<std::vector<std::string>> audience;
    std::optional<double> priority;
    std::optional<std::string> last_modified;
};

struct TextResourceContents {
    std::string uri;
    std::string text;
    std::optional<std::string> mime_type;
    std::optional<JsonValue> meta;
};

struct BlobResourceContents {
    std::string uri;
    std::string blob;
    std::optional<std::string> mime_type;
    std::optional<JsonValue> meta;
};

using ResourceContents = std::variant<TextResourceContents, BlobResourceContents>;

struct TextContent {
    std::string type = "text";
    std::string text;
    std::optional<std::string> mime_type = std::nullopt;
    std::optional<Annotations> annotations = std::nullopt;
    std::optional<JsonValue> meta = std::nullopt;
};

struct ImageContent {
    std::string type = "image";
    std::string data;
    std::string mime_type;
    std::optional<Annotations> annotations = std::nullopt;
    std::optional<JsonValue> meta = std::nullopt;
};

struct AudioContent {
    std::string type = "audio";
    std::string data;
    std::string mime_type;
    std::optional<Annotations> annotations = std::nullopt;
    std::optional<JsonValue> meta = std::nullopt;
};

struct EmbeddedResource {
    std::string type = "resource";
    ResourceContents resource;
    std::optional<Annotations> annotations = std::nullopt;
    std::optional<JsonValue> meta = std::nullopt;
};

struct ResourceLink {
    std::string type = "resource_link";
    std::string uri;
    std::optional<std::string> title = std::nullopt;
    std::optional<Annotations> annotations = std::nullopt;
    std::optional<JsonValue> meta = std::nullopt;
};

using ContentVariant = std::variant<TextContent, ImageContent, AudioContent, EmbeddedResource, ResourceLink>;

// ── Forward declarations for types defined in Meta.hpp / Implementation.hpp ──
struct Implementation;
enum class LoggingLevel;
struct CacheHint;
struct RequestMeta;

// ── Serialization ──
JsonValue SerializeIcon(const Icon& v);
Icon DeserializeIcon(const JsonValue& j);

JsonValue SerializeAnnotations(const Annotations& v);
Annotations DeserializeAnnotations(const JsonValue& j);

JsonValue SerializeTextResourceContents(const TextResourceContents& v);
TextResourceContents DeserializeTextResourceContents(const JsonValue& j);
JsonValue SerializeBlobResourceContents(const BlobResourceContents& v);
BlobResourceContents DeserializeBlobResourceContents(const JsonValue& j);
JsonValue SerializeResourceContents(const ResourceContents& rc);
ResourceContents DeserializeResourceContents(const JsonValue& j);

JsonValue SerializeTextContent(const TextContent& v);
TextContent DeserializeTextContent(const JsonValue& j);
JsonValue SerializeImageContent(const ImageContent& v);
ImageContent DeserializeImageContent(const JsonValue& j);
JsonValue SerializeAudioContent(const AudioContent& v);
AudioContent DeserializeAudioContent(const JsonValue& j);
JsonValue SerializeEmbeddedResource(const EmbeddedResource& v);
EmbeddedResource DeserializeEmbeddedResource(const JsonValue& j);
JsonValue SerializeResourceLink(const ResourceLink& v);
ResourceLink DeserializeResourceLink(const JsonValue& j);
JsonValue SerializeContentVariant(const ContentVariant& content);
ContentVariant DeserializeContentVariant(const JsonValue& j);

JsonValue SerializeImplementation(const Implementation& v);
Implementation DeserializeImplementation(const JsonValue& j);

JsonValue SerializeProgressToken(const std::variant<std::string, int64_t>& pt);
std::variant<std::string, int64_t> DeserializeProgressToken(const JsonValue& j);
JsonValue SerializeLoggingLevel(LoggingLevel l);
LoggingLevel DeserializeLoggingLevel(const JsonValue& j);
JsonValue SerializeCacheHint(const CacheHint& v);
CacheHint DeserializeCacheHint(const JsonValue& j);
JsonValue SerializeRequestMeta(const RequestMeta& v);
RequestMeta DeserializeRequestMeta(const JsonValue& j);

} // namespace mcp