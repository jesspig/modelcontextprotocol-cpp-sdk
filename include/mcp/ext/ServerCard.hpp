#pragma once

#include <mcp/Content.hpp>
#include <mcp/JsonValue.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mcp::ext {

struct ServerCardEndpoint {
    std::string url;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> variables;
};

struct ServerCardRepository {
    std::optional<std::string> url;
};

struct ServerCard {
    std::string name;
    std::string version;
    std::optional<std::string> description;
    std::optional<std::string> title;
    std::vector<Icon> icons;
    std::optional<std::string> repository;
    std::optional<std::string> website_url;
    std::vector<ServerCardEndpoint> endpoints;
    std::vector<std::string> supported_protocol_versions;
    std::optional<JsonValue> meta;
};

void ValidateCard(const ServerCard& card);

} // namespace mcp::ext
