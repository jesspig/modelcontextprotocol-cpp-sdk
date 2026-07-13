#pragma once

#include <mcp/Tool.hpp>
#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace mcp {

/// URI template for resource pattern matching.
struct ResourceTemplate {
    std::string pattern;  // e.g. "greeting://{name}"
    std::optional<bool> list;  // undefined = can list

    nlohmann::json ToJson() const;
    static ResourceTemplate FromJson(const nlohmann::json& j);
    static ResourceTemplate FromString(std::string_view pattern);
};

/// Resource definition matching schema.ts Resource.
struct Resource {
    std::string uri;
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> mime_type;
    std::optional<ToolAnnotations> annotations;
    std::vector<Icon> icons;
    std::optional<nlohmann::json> meta;

    nlohmann::json ToJson() const;
    static Resource FromJson(const nlohmann::json& j);
};

/// Options for registering a resource.
struct ResourceOptions {
    std::optional<std::string> name;
    std::optional<std::string> description;
    std::optional<std::string> title;
    std::optional<std::string> mime_type;
    std::optional<ToolAnnotations> annotations;
    std::vector<Icon> icons;
    std::optional<nlohmann::json> meta;

    ResourceOptions& Description(std::string_view d) {
        description = std::string(d); return *this;
    }
    ResourceOptions& Title(std::string_view t) {
        title = std::string(t); return *this;
    }
    ResourceOptions& MimeType(std::string_view m) {
        mime_type = std::string(m); return *this;
    }
    ResourceOptions& Icons(std::vector<Icon> ic) {
        icons = std::move(ic); return *this;
    }
};

} // namespace mcp
