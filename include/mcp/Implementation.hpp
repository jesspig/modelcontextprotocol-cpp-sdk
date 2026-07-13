#pragma once

#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace mcp {

/// Describes an MCP implementation (client or server).
/// Matches C# ModelContextProtocol.Protocol.Implementation.
struct Implementation {
    std::string name;
    std::string version;
    std::optional<std::string> description;
    std::optional<std::string> website_url;

    nlohmann::json ToJson() const;
    static Implementation FromJson(const nlohmann::json& j);
    static Implementation Default(std::string_view name, std::string_view version = "0.1.0");
};

} // namespace mcp
