#include <mcp/ext/Apps.hpp>
#include <mcp/ErrorCodes.hpp>
#include <mcp/McpError.hpp>

namespace mcp::ext {

void ValidateUiMeta(const UiMeta& meta) {
    if (meta.resource_uri.empty()) {
        throw McpError(McpErrorCode::InvalidParams, "ui resource_uri must not be empty");
    }
    constexpr char kPrefix[] = "ui://";
    if (meta.resource_uri.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) {
        throw McpError(McpErrorCode::InvalidParams, "ui resource_uri must start with ui://");
    }
}

} // namespace mcp::ext
