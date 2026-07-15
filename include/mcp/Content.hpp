#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mcp {

struct Icon {
    std::string src;
    std::string mime_type;
    std::optional<std::vector<std::string>> sizes;
    std::optional<std::string> theme;
};
inline void to_json(nlohmann::json& j, const Icon& v) {
    j = nlohmann::json::object();
    j["src"] = v.src;
    j["mimeType"] = v.mime_type;
    if (v.sizes) j["sizes"] = *v.sizes;
    if (v.theme) j["theme"] = *v.theme;
}
inline void from_json(const nlohmann::json& j, Icon& v) {
    j.at("src").get_to(v.src);
    j.at("mimeType").get_to(v.mime_type);
    if (auto it = j.find("sizes"); it != j.end()) v.sizes = it->get<decltype(v.sizes)::value_type>();
    if (auto it = j.find("theme"); it != j.end()) v.theme = it->get<std::string>();
}

struct TextResourceContents {
    std::string uri;
    std::string text;
    std::optional<std::string> mime_type;
    std::optional<nlohmann::json> meta;
};
inline void to_json(nlohmann::json& j, const TextResourceContents& v) {
    j = nlohmann::json::object();
    j["uri"] = v.uri;
    j["text"] = v.text;
    if (v.mime_type) j["mimeType"] = *v.mime_type;
    if (v.meta)      j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, TextResourceContents& v) {
    j.at("uri").get_to(v.uri);
    j.at("text").get_to(v.text);
    if (auto it = j.find("mimeType"); it != j.end()) v.mime_type = it->get<std::string>();
    if (auto it = j.find("_meta"); it != j.end())    v.meta = *it;
}

struct BlobResourceContents {
    std::string uri;
    std::string blob;
    std::optional<std::string> mime_type;
    std::optional<nlohmann::json> meta;
};
inline void to_json(nlohmann::json& j, const BlobResourceContents& v) {
    j = nlohmann::json::object();
    j["uri"] = v.uri;
    j["blob"] = v.blob;
    if (v.mime_type) j["mimeType"] = *v.mime_type;
    if (v.meta)      j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, BlobResourceContents& v) {
    j.at("uri").get_to(v.uri);
    j.at("blob").get_to(v.blob);
    if (auto it = j.find("mimeType"); it != j.end()) v.mime_type = it->get<std::string>();
    if (auto it = j.find("_meta"); it != j.end())    v.meta = *it;
}

using ResourceContents = std::variant<TextResourceContents, BlobResourceContents>;

inline void to_json(nlohmann::json& j, const ResourceContents& rc) {
    std::visit([&j](const auto& v) { j = nlohmann::json(v); }, rc);
}
inline void from_json(const nlohmann::json& j, ResourceContents& rc) {
    if (j.contains("text"))       rc = j.get<TextResourceContents>();
    else if (j.contains("blob")) rc = j.get<BlobResourceContents>();
    else throw std::runtime_error("unknown ResourceContents type");
}

struct TextContent {
    std::string type = "text";
    std::string text;
    std::optional<std::string> mime_type;
    std::optional<nlohmann::json> annotations;
};
inline void to_json(nlohmann::json& j, const TextContent& v) {
    j = nlohmann::json{{"type", v.type}, {"text", v.text}};
    if (v.mime_type)   j["mimeType"] = *v.mime_type;
    if (v.annotations) j["annotations"] = *v.annotations;
}
inline void from_json(const nlohmann::json& j, TextContent& v) {
    j.at("type").get_to(v.type);
    j.at("text").get_to(v.text);
    if (auto it = j.find("mimeType"); it != j.end())    v.mime_type = it->get<std::string>();
    if (auto it = j.find("annotations"); it != j.end()) v.annotations = *it;
}

struct ImageContent {
    std::string type = "image";
    std::string data;
    std::string mime_type;
    std::optional<nlohmann::json> annotations;
};
inline void to_json(nlohmann::json& j, const ImageContent& v) {
    j = nlohmann::json{{"type", v.type}, {"data", v.data}, {"mimeType", v.mime_type}};
    if (v.annotations) j["annotations"] = *v.annotations;
}
inline void from_json(const nlohmann::json& j, ImageContent& v) {
    j.at("type").get_to(v.type);
    j.at("data").get_to(v.data);
    j.at("mimeType").get_to(v.mime_type);
    if (auto it = j.find("annotations"); it != j.end()) v.annotations = *it;
}

struct AudioContent {
    std::string type = "audio";
    std::string data;
    std::string mime_type;
    std::optional<nlohmann::json> annotations;
};
inline void to_json(nlohmann::json& j, const AudioContent& v) {
    j = nlohmann::json{{"type", v.type}, {"data", v.data}, {"mimeType", v.mime_type}};
    if (v.annotations) j["annotations"] = *v.annotations;
}
inline void from_json(const nlohmann::json& j, AudioContent& v) {
    j.at("type").get_to(v.type);
    j.at("data").get_to(v.data);
    j.at("mimeType").get_to(v.mime_type);
    if (auto it = j.find("annotations"); it != j.end()) v.annotations = *it;
}

struct EmbeddedResource {
    std::string type = "resource";
    ResourceContents resource;
    std::optional<nlohmann::json> annotations;
};
inline void to_json(nlohmann::json& j, const EmbeddedResource& v) {
    j = nlohmann::json{{"type", v.type}, {"resource", v.resource}};
    if (v.annotations) j["annotations"] = *v.annotations;
}
inline void from_json(const nlohmann::json& j, EmbeddedResource& v) {
    j.at("type").get_to(v.type);
    v.resource = j.at("resource").get<ResourceContents>();
    if (auto it = j.find("annotations"); it != j.end()) v.annotations = *it;
}

using ContentVariant = std::variant<TextContent, ImageContent, AudioContent, EmbeddedResource>;

inline void to_json(nlohmann::json& j, const ContentVariant& content) {
    std::visit([&j](const auto& v) { j = nlohmann::json(v); }, content);
}
inline void from_json(const nlohmann::json& j, ContentVariant& content) {
    auto type = j.at("type").get<std::string>();
    if (type == "text")         content = j.get<TextContent>();
    else if (type == "image")   content = j.get<ImageContent>();
    else if (type == "audio")   content = j.get<AudioContent>();
    else if (type == "resource") content = j.get<EmbeddedResource>();
    else throw std::runtime_error("unknown Content type: " + type);
}

} // namespace mcp