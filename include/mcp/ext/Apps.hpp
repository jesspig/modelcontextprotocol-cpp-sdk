#pragma once

#include <mcp/JsonValue.hpp>

#include <string>
#include <vector>

namespace mcp::ext {

struct UiMeta {
    std::string resource_uri;
    std::vector<std::string> csp;
    std::vector<std::string> permissions;
};

void ValidateUiMeta(const UiMeta& meta);

} // namespace mcp::ext
