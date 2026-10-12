#include <mcp/ext/ServerCard.hpp>
#include <mcp/ErrorCodes.hpp>
#include <mcp/McpError.hpp>

namespace mcp::ext {

namespace {

bool HasHttpScheme(const std::string& url) {
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}

} // namespace

void ValidateCard(const ServerCard& card) {
    if (card.name.empty()) {
        throw McpError(McpErrorCode::InvalidParams, "server card name must not be empty");
    }
    if (card.version.empty()) {
        throw McpError(McpErrorCode::InvalidParams, "server card version must not be empty");
    }
    if (card.endpoints.empty()) {
        throw McpError(McpErrorCode::InvalidParams, "server card endpoints must not be empty");
    }
    for (const auto& endpoint : card.endpoints) {
        if (endpoint.url.empty() || !HasHttpScheme(endpoint.url)) {
            throw McpError(McpErrorCode::InvalidParams, "server card endpoint url must use http/https");
        }
    }
}

} // namespace mcp::ext
