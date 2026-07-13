#pragma once

#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace mcp {

/// Tools capability.
struct ToolsCapability {
    std::optional<bool> list_changed;
    std::optional<bool> subscribe;

    nlohmann::json ToJson() const;
    static ToolsCapability FromJson(const nlohmann::json& j);
};

/// Resources capability.
struct ResourcesCapability {
    std::optional<bool> list_changed;
    std::optional<bool> subscribe;

    nlohmann::json ToJson() const;
    static ResourcesCapability FromJson(const nlohmann::json& j);
};

/// Prompts capability.
struct PromptsCapability {
    std::optional<bool> list_changed;

    nlohmann::json ToJson() const;
    static PromptsCapability FromJson(const nlohmann::json& j);
};

/// Logging capability [deprecated].
struct LoggingCapability {
    std::optional<nlohmann::json> additional;

    nlohmann::json ToJson() const;
    static LoggingCapability FromJson(const nlohmann::json& j);
};

/// Sampling capability [deprecated].
struct SamplingCapability {
    std::optional<nlohmann::json> additional;

    nlohmann::json ToJson() const;
    static SamplingCapability FromJson(const nlohmann::json& j);
};

/// Roots capability [deprecated].
struct RootsCapability {
    std::optional<bool> list_changed;

    nlohmann::json ToJson() const;
    static RootsCapability FromJson(const nlohmann::json& j);
};

/// Tasks capability.
struct TasksCapability {
    std::optional<nlohmann::json> additional;

    nlohmann::json ToJson() const;
    static TasksCapability FromJson(const nlohmann::json& j);
};

/// Elicitation capability.
struct ElicitationCapability {
    std::optional<nlohmann::json> form;
    std::optional<bool> url;

    nlohmann::json ToJson() const;
    static ElicitationCapability FromJson(const nlohmann::json& j);
};

/// Server-side capabilities.
struct ServerCapabilities {
    std::optional<ToolsCapability> tools;
    std::optional<ResourcesCapability> resources;
    std::optional<PromptsCapability> prompts;
    std::optional<LoggingCapability> logging;        // [deprecated]
    std::optional<SamplingCapability> sampling;       // [deprecated]
    std::optional<RootsCapability> roots;             // [deprecated]
    std::optional<TasksCapability> tasks;

    nlohmann::json ToJson() const;
    static ServerCapabilities FromJson(const nlohmann::json& j);

    static ServerCapabilities Default();
    ServerCapabilities& WithTools(bool list_changed = false, bool subscribe = false);
    ServerCapabilities& WithResources(bool list_changed = false, bool subscribe = false);
    ServerCapabilities& WithPrompts(bool list_changed = false);
    ServerCapabilities& WithTasks();
};

/// Client-side capabilities.
struct ClientCapabilities {
    std::optional<RootsCapability> roots;              // [deprecated]
    std::optional<SamplingCapability> sampling;        // [deprecated]
    std::optional<ElicitationCapability> elicitation;

    nlohmann::json ToJson() const;
    static ClientCapabilities FromJson(const nlohmann::json& j);

    static ClientCapabilities Default();
    ClientCapabilities& WithElicitation(bool form = true, bool url = false);
};

} // namespace mcp
