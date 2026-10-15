#pragma once

#include <mcp/JsonValue.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace mcp::ext {

inline constexpr std::string_view kAppsExtensionId = "io.modelcontextprotocol/ui";
inline constexpr std::string_view kAppsMimeType = "text/html;profile=mcp-app";
inline constexpr std::string_view kAppsUriScheme = "ui://";

struct UiMeta {
    std::string resource_uri;
    std::vector<std::string> csp;
    std::vector<std::string> permissions;
};

void ValidateUiMeta(const UiMeta& meta);

} // namespace mcp::ext
