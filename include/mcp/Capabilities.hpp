#pragma once

#include <nlohmann/json.hpp>

#include <optional>

namespace mcp {

struct ToolsCapability {
    std::optional<bool> list_changed;
};
inline void to_json(nlohmann::json& j, const ToolsCapability& v) {
    j = nlohmann::json::object();
    if (v.list_changed) j["listChanged"] = *v.list_changed;
}
inline void from_json(const nlohmann::json& j, ToolsCapability& v) {
    if (auto it = j.find("listChanged"); it != j.end())
        v.list_changed = it->get<bool>();
}

struct ResourcesCapability {
    std::optional<bool> subscribe;
    std::optional<bool> list_changed;
};
inline void to_json(nlohmann::json& j, const ResourcesCapability& v) {
    j = nlohmann::json::object();
    if (v.subscribe)   j["subscribe"] = *v.subscribe;
    if (v.list_changed) j["listChanged"] = *v.list_changed;
}
inline void from_json(const nlohmann::json& j, ResourcesCapability& v) {
    if (auto it = j.find("subscribe"); it != j.end()) v.subscribe = it->get<bool>();
    if (auto it = j.find("listChanged"); it != j.end()) v.list_changed = it->get<bool>();
}

struct PromptsCapability {
    std::optional<bool> list_changed;
};
inline void to_json(nlohmann::json& j, const PromptsCapability& v) {
    j = nlohmann::json::object();
    if (v.list_changed) j["listChanged"] = *v.list_changed;
}
inline void from_json(const nlohmann::json& j, PromptsCapability& v) {
    if (auto it = j.find("listChanged"); it != j.end())
        v.list_changed = it->get<bool>();
}

struct LoggingCapability {};
inline void to_json(nlohmann::json& j, const LoggingCapability&) { j = nlohmann::json::object(); }
inline void from_json(const nlohmann::json& j, LoggingCapability&) {}

struct SamplingCapability {};
inline void to_json(nlohmann::json& j, const SamplingCapability&) { j = nlohmann::json::object(); }
inline void from_json(const nlohmann::json& j, SamplingCapability&) {}

struct RootsCapability {
    std::optional<bool> list_changed;
};
inline void to_json(nlohmann::json& j, const RootsCapability& v) {
    j = nlohmann::json::object();
    if (v.list_changed) j["listChanged"] = *v.list_changed;
}
inline void from_json(const nlohmann::json& j, RootsCapability& v) {
    if (auto it = j.find("listChanged"); it != j.end()) v.list_changed = it->get<bool>();
}

struct ElicitationCapability {
    std::optional<nlohmann::json> form;
    std::optional<nlohmann::json> url;
};
inline void to_json(nlohmann::json& j, const ElicitationCapability& v) {
    j = nlohmann::json::object();
    if (v.form) j["form"] = *v.form;
    if (v.url)  j["url"] = *v.url;
}
inline void from_json(const nlohmann::json& j, ElicitationCapability& v) {
    if (auto it = j.find("form"); it != j.end()) v.form = *it;
    if (auto it = j.find("url"); it != j.end())  v.url = *it;
}

struct TasksCapability {};
inline void to_json(nlohmann::json& j, const TasksCapability&) { j = nlohmann::json::object(); }
inline void from_json(const nlohmann::json& j, TasksCapability&) {}

struct SubscriptionsCapability {};
inline void to_json(nlohmann::json& j, const SubscriptionsCapability&) { j = nlohmann::json::object(); }
inline void from_json(const nlohmann::json& j, SubscriptionsCapability&) {}

struct ServerCapabilities {
    std::optional<ToolsCapability> tools;
    std::optional<ResourcesCapability> resources;
    std::optional<PromptsCapability> prompts;
    std::optional<LoggingCapability> logging;
    std::optional<SamplingCapability> sampling;
    std::optional<RootsCapability> roots;
    std::optional<ElicitationCapability> elicitation;
    std::optional<TasksCapability> tasks;
    std::optional<SubscriptionsCapability> subscriptions;
    std::optional<nlohmann::json> extensions;
    std::optional<nlohmann::json> experimental;
};
inline void to_json(nlohmann::json& j, const ServerCapabilities& v) {
    j = nlohmann::json::object();
    if (v.tools)         j["tools"] = *v.tools;
    if (v.resources)     j["resources"] = *v.resources;
    if (v.prompts)       j["prompts"] = *v.prompts;
    if (v.logging)       j["logging"] = *v.logging;
    if (v.sampling)      j["sampling"] = *v.sampling;
    if (v.roots)         j["roots"] = *v.roots;
    if (v.elicitation)   j["elicitation"] = *v.elicitation;
    if (v.tasks)         j["tasks"] = *v.tasks;
    if (v.subscriptions) j["subscriptions"] = *v.subscriptions;
    if (v.extensions)    j["extensions"] = *v.extensions;
    if (v.experimental)  j["experimental"] = *v.experimental;
}
inline void from_json(const nlohmann::json& j, ServerCapabilities& v) {
    if (auto it = j.find("tools"); it != j.end())         v.tools = it->get<ToolsCapability>();
    if (auto it = j.find("resources"); it != j.end())     v.resources = it->get<ResourcesCapability>();
    if (auto it = j.find("prompts"); it != j.end())       v.prompts = it->get<PromptsCapability>();
    if (auto it = j.find("logging"); it != j.end())       v.logging = it->get<LoggingCapability>();
    if (auto it = j.find("sampling"); it != j.end())      v.sampling = it->get<SamplingCapability>();
    if (auto it = j.find("roots"); it != j.end())         v.roots = it->get<RootsCapability>();
    if (auto it = j.find("elicitation"); it != j.end())   v.elicitation = it->get<ElicitationCapability>();
    if (auto it = j.find("tasks"); it != j.end())         v.tasks = it->get<TasksCapability>();
    if (auto it = j.find("subscriptions"); it != j.end()) v.subscriptions = it->get<SubscriptionsCapability>();
    if (auto it = j.find("extensions"); it != j.end())    v.extensions = *it;
    if (auto it = j.find("experimental"); it != j.end())  v.experimental = *it;
}

struct ClientCapabilities {
    std::optional<RootsCapability> roots;
    std::optional<SamplingCapability> sampling;
    std::optional<ElicitationCapability> elicitation;
    std::optional<nlohmann::json> extensions;
    std::optional<nlohmann::json> experimental;
};
inline void to_json(nlohmann::json& j, const ClientCapabilities& v) {
    j = nlohmann::json::object();
    if (v.roots)       j["roots"] = *v.roots;
    if (v.sampling)    j["sampling"] = *v.sampling;
    if (v.elicitation) j["elicitation"] = *v.elicitation;
    if (v.extensions)  j["extensions"] = *v.extensions;
    if (v.experimental) j["experimental"] = *v.experimental;
}
inline void from_json(const nlohmann::json& j, ClientCapabilities& v) {
    if (auto it = j.find("roots"); it != j.end())       v.roots = it->get<RootsCapability>();
    if (auto it = j.find("sampling"); it != j.end())    v.sampling = it->get<SamplingCapability>();
    if (auto it = j.find("elicitation"); it != j.end()) v.elicitation = it->get<ElicitationCapability>();
    if (auto it = j.find("extensions"); it != j.end())  v.extensions = *it;
    if (auto it = j.find("experimental"); it != j.end()) v.experimental = *it;
}

} // namespace mcp
