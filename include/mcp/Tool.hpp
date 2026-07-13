#pragma once

#include <mcp/Content.hpp>
#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace mcp {

/// Icon for a tool, resource, or prompt.
struct Icon {
    std::string src;
    std::string mime_type;
    std::vector<std::string> sizes;

    nlohmann::json ToJson() const;
    static Icon FromJson(const nlohmann::json& j);
};

/// Tool annotations matching schema.ts ToolAnnotations.
struct ToolAnnotations {
    std::optional<std::string> title;
    std::optional<bool> read_only_hint;
    std::optional<bool> idempotent_hint;
    std::optional<bool> open_world_hint;
    std::optional<bool> destructive_hint;

    nlohmann::json ToJson() const;
    static ToolAnnotations FromJson(const nlohmann::json& j);
};

/// Tool definition matching schema.ts Tool.
struct Tool {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    nlohmann::json input_schema;    // JSON Schema 2020-12
    std::optional<nlohmann::json> output_schema;  // JSON Schema 2020-12
    std::optional<ToolAnnotations> annotations;
    std::vector<Icon> icons;
    std::optional<nlohmann::json> meta;

    nlohmann::json ToJson() const;
    static Tool FromJson(const nlohmann::json& j);
};

/// Options for registering a tool (matches C# McpServerToolCreateOptions).
struct ToolOptions {
    std::optional<std::string> name;
    std::optional<std::string> description;
    std::optional<std::string> title;
    std::optional<bool> destructive;
    std::optional<bool> idempotent;
    std::optional<bool> read_only;
    std::optional<bool> open_world;
    bool use_structured_content{false};
    std::optional<nlohmann::json> output_schema;
    std::vector<Icon> icons;
    std::optional<nlohmann::json> meta;

    // Builder pattern
    ToolOptions& Description(std::string_view d) {
        description = std::string(d); return *this;
    }
    ToolOptions& Title(std::string_view t) {
        title = std::string(t); return *this;
    }
    ToolOptions& ReadOnlyHint(bool v) {
        read_only = v; return *this;
    }
    ToolOptions& IdempotentHint(bool v) {
        idempotent = v; return *this;
    }
    ToolOptions& DestructiveHint(bool v) {
        destructive = v; return *this;
    }
    ToolOptions& OpenWorldHint(bool v) {
        open_world = v; return *this;
    }
    ToolOptions& OutputSchema(const nlohmann::json& schema) {
        output_schema = schema; return *this;
    }
    ToolOptions& AddIcon(Icon icon) {
        icons.push_back(std::move(icon)); return *this;
    }
    ToolOptions& Meta(const nlohmann::json& m) {
        meta = m; return *this;
    }
    ToolOptions& StructuredContent(bool v) {
        use_structured_content = v; return *this;
    }
    ToolOptions& Name(std::string_view n) {
        name = std::string(n); return *this;
    }
};

} // namespace mcp
