#pragma once

#include <mcp/Content.hpp>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace mcp {

struct Implementation {
    std::string name;
    std::string version;
    std::optional<std::string> title;
    std::vector<Icon> icons;
    std::optional<std::string> description;
    std::optional<std::string> website_url;
};

inline void to_json(nlohmann::json& j, const Implementation& v) {
    j = nlohmann::json::object();
    j["name"] = v.name;
    j["version"] = v.version;
    if (v.title)       j["title"] = *v.title;
    if (!v.icons.empty()) j["icons"] = v.icons;
    if (v.description) j["description"] = *v.description;
    if (v.website_url) j["websiteUrl"] = *v.website_url;
}
inline void from_json(const nlohmann::json& j, Implementation& v) {
    j.at("name").get_to(v.name);
    j.at("version").get_to(v.version);
    if (auto it = j.find("title"); it != j.end())       v.title = it->get<std::string>();
    if (auto it = j.find("icons"); it != j.end())       v.icons = it->get<std::vector<Icon>>();
    if (auto it = j.find("description"); it != j.end()) v.description = it->get<std::string>();
    if (auto it = j.find("websiteUrl"); it != j.end())  v.website_url = it->get<std::string>();
}

} // namespace mcp
