#pragma once

#include <nlohmann/json.hpp>

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
inline void to_json(nlohmann::json& j, const Icon& v) {
    j = nlohmann::json::object();
    j["src"] = v.src;
    if (v.mime_type) j["mimeType"] = *v.mime_type;
    if (v.sizes) j["sizes"] = *v.sizes;
    if (v.theme) j["theme"] = *v.theme;
}
inline void from_json(const nlohmann::json& j, Icon& v) {
    j.at("src").get_to(v.src);
    if (auto it = j.find("mimeType"); it != j.end()) v.mime_type = it->get<std::string>();
    if (auto it = j.find("sizes"); it != j.end()) v.sizes = it->get<decltype(v.sizes)::value_type>();
    if (auto it = j.find("theme"); it != j.end()) v.theme = it->get<std::string>();
}

struct Annotations {
    std::optional<std::vector<std::string>> audience;
    std::optional<double> priority;
    std::optional<std::string> last_modified;
};
inline void to_json(nlohmann::json& j, const Annotations& v) {
    j = nlohmann::json::object();
    if (v.audience)      j["audience"] = *v.audience;
    if (v.priority)      j["priority"] = *v.priority;
    if (v.last_modified) j["lastModified"] = *v.last_modified;
}
inline void from_json(const nlohmann::json& j, Annotations& v) {
    if (auto it = j.find("audience"); it != j.end())      v.audience = it->get<std::vector<std::string>>();
    if (auto it = j.find("priority"); it != j.end())      v.priority = it->get<double>();
    if (auto it = j.find("lastModified"); it != j.end())  v.last_modified = it->get<std::string>();
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
    std::optional<std::string> mime_type = std::nullopt;
    std::optional<Annotations> annotations = std::nullopt;
    std::optional<nlohmann::json> meta = std::nullopt;
};
inline void to_json(nlohmann::json& j, const TextContent& v) {
    j = nlohmann::json{{"type", v.type}, {"text", v.text}};
    if (v.mime_type)   j["mimeType"] = *v.mime_type;
    if (v.annotations) j["annotations"] = *v.annotations;
    if (v.meta)        j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, TextContent& v) {
    j.at("type").get_to(v.type);
    j.at("text").get_to(v.text);
    if (auto it = j.find("mimeType"); it != j.end())    v.mime_type = it->get<std::string>();
    if (auto it = j.find("annotations"); it != j.end()) v.annotations = *it;
    if (auto it = j.find("_meta"); it != j.end())       v.meta = *it;
}

struct ImageContent {
    std::string type = "image";
    std::string data;
    std::string mime_type;
    std::optional<Annotations> annotations = std::nullopt;
    std::optional<nlohmann::json> meta = std::nullopt;
};
inline void to_json(nlohmann::json& j, const ImageContent& v) {
    j = nlohmann::json{{"type", v.type}, {"data", v.data}, {"mimeType", v.mime_type}};
    if (v.annotations) j["annotations"] = *v.annotations;
    if (v.meta)        j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, ImageContent& v) {
    j.at("type").get_to(v.type);
    j.at("data").get_to(v.data);
    j.at("mimeType").get_to(v.mime_type);
    if (auto it = j.find("annotations"); it != j.end()) v.annotations = *it;
    if (auto it = j.find("_meta"); it != j.end())       v.meta = *it;
}

struct AudioContent {
    std::string type = "audio";
    std::string data;
    std::string mime_type;
    std::optional<Annotations> annotations = std::nullopt;
    std::optional<nlohmann::json> meta = std::nullopt;
};
inline void to_json(nlohmann::json& j, const AudioContent& v) {
    j = nlohmann::json{{"type", v.type}, {"data", v.data}, {"mimeType", v.mime_type}};
    if (v.annotations) j["annotations"] = *v.annotations;
    if (v.meta)        j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, AudioContent& v) {
    j.at("type").get_to(v.type);
    j.at("data").get_to(v.data);
    j.at("mimeType").get_to(v.mime_type);
    if (auto it = j.find("annotations"); it != j.end()) v.annotations = *it;
    if (auto it = j.find("_meta"); it != j.end())       v.meta = *it;
}

struct EmbeddedResource {
    std::string type = "resource";
    ResourceContents resource;
    std::optional<Annotations> annotations = std::nullopt;
    std::optional<nlohmann::json> meta = std::nullopt;
};
inline void to_json(nlohmann::json& j, const EmbeddedResource& v) {
    j = nlohmann::json{{"type", v.type}, {"resource", v.resource}};
    if (v.annotations) j["annotations"] = *v.annotations;
    if (v.meta)        j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, EmbeddedResource& v) {
    j.at("type").get_to(v.type);
    v.resource = j.at("resource").get<ResourceContents>();
    if (auto it = j.find("annotations"); it != j.end()) v.annotations = *it;
    if (auto it = j.find("_meta"); it != j.end())       v.meta = *it;
}

struct ResourceLink {
    std::string type = "resource_link";
    std::string uri;
    std::optional<std::string> title = std::nullopt;
    std::optional<Annotations> annotations = std::nullopt;
    std::optional<nlohmann::json> meta = std::nullopt;
};
inline void to_json(nlohmann::json& j, const ResourceLink& v) {
    j = nlohmann::json{{"type", v.type}, {"uri", v.uri}};
    if (v.title)       j["title"] = *v.title;
    if (v.annotations) j["annotations"] = *v.annotations;
    if (v.meta)        j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, ResourceLink& v) {
    j.at("type").get_to(v.type);
    j.at("uri").get_to(v.uri);
    if (auto it = j.find("title"); it != j.end())       v.title = it->get<std::string>();
    if (auto it = j.find("annotations"); it != j.end()) v.annotations = *it;
    if (auto it = j.find("_meta"); it != j.end())       v.meta = *it;
}

using ContentVariant = std::variant<TextContent, ImageContent, AudioContent, EmbeddedResource, ResourceLink>;

inline void to_json(nlohmann::json& j, const ContentVariant& content) {
    std::visit([&j](const auto& v) { j = nlohmann::json(v); }, content);
}
inline void from_json(const nlohmann::json& j, ContentVariant& content) {
    auto type = j.at("type").get<std::string>();
    if (type == "text")          content = j.get<TextContent>();
    else if (type == "image")    content = j.get<ImageContent>();
    else if (type == "audio")    content = j.get<AudioContent>();
    else if (type == "resource") content = j.get<EmbeddedResource>();
    else if (type == "resource_link") content = j.get<ResourceLink>();
    else throw std::runtime_error("unknown Content type: " + type);
}

} // namespace mcp