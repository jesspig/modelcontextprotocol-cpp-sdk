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

} // namespace mcp