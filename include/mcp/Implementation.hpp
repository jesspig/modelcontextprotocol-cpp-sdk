#pragma once

#include <mcp/Content.hpp>

#include <optional>
#include <string>
#include <vector>

namespace mcp {

struct Implementation {
    std::string name;
    std::string version;
    std::optional<std::string> title = std::nullopt;
    std::vector<Icon> icons = {};
    std::optional<std::string> description = std::nullopt;
    std::optional<std::string> website_url = std::nullopt;
};

} // namespace mcp
