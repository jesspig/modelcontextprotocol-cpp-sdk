#pragma once

#include <string>
#include <variant>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace mcp {

/// Text content block.
struct TextContent {
    std::string text;

    nlohmann::json ToJson() const;
    static TextContent FromJson(const nlohmann::json& j);
};

/// Image content block (base64-encoded data).
struct ImageContent {
    std::string data;  // base64-encoded
    std::string mime_type;

    nlohmann::json ToJson() const;
    static ImageContent FromJson(const nlohmann::json& j);
};

/// Audio content block (base64-encoded data).
struct AudioContent {
    std::string data;  // base64-encoded
    std::string mime_type;

    nlohmann::json ToJson() const;
    static AudioContent FromJson(const nlohmann::json& j);
};

/// Embedded resource content block.
struct EmbeddedResource {
    nlohmann::json resource;  // ResourceContents variant

    nlohmann::json ToJson() const;
    static EmbeddedResource FromJson(const nlohmann::json& j);
};

/// Content block variant (matches schema.ts Content union).
using Content = std::variant<
    TextContent,
    ImageContent,
    AudioContent,
    EmbeddedResource>;

nlohmann::json ContentToJson(const Content& content);
Content ContentFromJson(const nlohmann::json& j);

/// Resource contents (for ReadResourceResult).
struct TextResourceContents {
    std::string uri;
    std::string text;
    std::optional<std::string> mime_type;

    nlohmann::json ToJson() const;
    static TextResourceContents FromJson(const nlohmann::json& j);
};

struct BlobResourceContents {
    std::string uri;
    std::string blob;  // base64-encoded
    std::optional<std::string> mime_type;

    nlohmann::json ToJson() const;
    static BlobResourceContents FromJson(const nlohmann::json& j);
};

using ResourceContents = std::variant<TextResourceContents, BlobResourceContents>;

nlohmann::json ResourceContentsToJson(const ResourceContents& rc);
ResourceContents ResourceContentsFromJson(const nlohmann::json& j);

} // namespace mcp
