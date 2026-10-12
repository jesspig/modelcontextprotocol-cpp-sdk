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

JsonValue SerializeCard(const ServerCard& card) {
    ValidateCard(card);
    JsonValue::Object root;
    root["name"] = JsonValue(card.name);
    root["version"] = JsonValue(card.version);
    root["description"] = JsonValue(card.description.value_or(""));
    JsonValue::Array endpoints;
    for (const auto& endpoint : card.endpoints) {
        JsonValue::Object entry;
        entry["url"] = JsonValue(endpoint.url);
        if (!endpoint.headers.empty()) {
            JsonValue::Object headers;
            for (const auto& [key, value] : endpoint.headers) {
                headers[key] = JsonValue(value);
            }
            entry["headers"] = JsonValue(std::move(headers));
        }
        if (!endpoint.variables.empty()) {
            JsonValue::Object variables;
            for (const auto& [key, value] : endpoint.variables) {
                variables[key] = JsonValue(value);
            }
            entry["variables"] = JsonValue(std::move(variables));
        }
        endpoints.push_back(JsonValue(std::move(entry)));
    }
    root["endpoints"] = JsonValue(std::move(endpoints));
    JsonValue::Array versions;
    for (const auto& version : card.supported_protocol_versions) {
        versions.push_back(JsonValue(version));
    }
    root["supportedProtocolVersions"] = JsonValue(std::move(versions));
    return JsonValue(std::move(root));
}

} // namespace mcp::ext
