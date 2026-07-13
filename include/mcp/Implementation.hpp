#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace mcp {

struct Implementation {
    std::string name;
    std::string version;
    std::optional<std::string> description;
    std::optional<std::string> website_url;
};

inline void to_json(nlohmann::json& j, const Implementation& v) {
    j = nlohmann::json::object();
    j["name"] = v.name;
    j["version"] = v.version;
    if (v.description) j["description"] = *v.description;
    if (v.website_url) j["website_url"] = *v.website_url;
}
inline void from_json(const nlohmann::json& j, Implementation& v) {
    j.at("name").get_to(v.name);
    j.at("version").get_to(v.version);
    if (auto it = j.find("description"); it != j.end()) v.description = it->get<std::string>();
    if (auto it = j.find("website_url"); it != j.end()) v.website_url = it->get<std::string>();
}

} // namespace mcp
