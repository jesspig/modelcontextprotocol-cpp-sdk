#pragma once

#include <mcp/Content.hpp>
#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace mcp {

/// Prompt argument definition.
struct PromptArgument {
    std::string name;
    std::optional<std::string> description;
    bool required{false};

    nlohmann::json ToJson() const;
    static PromptArgument FromJson(const nlohmann::json& j);
};

/// Prompt definition matching schema.ts Prompt.
struct Prompt {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::vector<PromptArgument> arguments;
    std::vector<Icon> icons;
    std::optional<nlohmann::json> meta;

    nlohmann::json ToJson() const;
    static Prompt FromJson(const nlohmann::json& j);
};

/// Prompt message role.
enum class PromptMessageRole { User, Assistant };

/// A single message in a prompt.
struct PromptMessage {
    PromptMessageRole role;
    Content content;

    nlohmann::json ToJson() const;
    static PromptMessage FromJson(const nlohmann::json& j);
};

/// Options for registering a prompt.
struct PromptOptions {
    std::optional<std::string> name;
    std::optional<std::string> description;
    std::optional<std::string> title;
    std::vector<Icon> icons;

    PromptOptions& Description(std::string_view d) {
        description = std::string(d); return *this;
    }
    PromptOptions& Title(std::string_view t) {
        title = std::string(t); return *this;
    }
    PromptOptions& Name(std::string_view n) {
        name = std::string(n); return *this;
    }
};

} // namespace mcp
