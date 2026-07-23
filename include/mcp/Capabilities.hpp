#pragma once

#include <mcp/JsonValue.hpp>

#include <map>
#include <optional>
#include <string>

namespace mcp {

struct ToolsCapability {
    std::optional<bool> list_changed;
};

struct ResourcesCapability {
    std::optional<bool> subscribe;
    std::optional<bool> list_changed;
};

struct PromptsCapability {
    std::optional<bool> list_changed;
};

struct SamplingCapability {};

struct RootsCapability {
    std::optional<bool> list_changed;
};

struct ElicitationCapability {
    std::optional<JsonValue> form;
    std::optional<JsonValue> url;
};

struct EmptyCapability {};

using LoggingCapability = EmptyCapability;
using TasksCapability = EmptyCapability;
using SubscriptionsCapability = EmptyCapability;
using CompletionsCapability = EmptyCapability;

struct ServerCapabilities {
    std::optional<ToolsCapability> tools;
    std::optional<ResourcesCapability> resources;
    std::optional<PromptsCapability> prompts;
    std::optional<LoggingCapability> logging;
    std::optional<SamplingCapability> sampling;
    std::optional<RootsCapability> roots;
    std::optional<ElicitationCapability> elicitation;
    std::optional<CompletionsCapability> completions;
    std::optional<SubscriptionsCapability> subscriptions;
    std::optional<std::map<std::string, JsonValue>> extensions;
    std::optional<JsonValue> experimental;
};

struct ClientCapabilities {
    std::optional<RootsCapability> roots;
    std::optional<SamplingCapability> sampling;
    std::optional<ElicitationCapability> elicitation;
    std::optional<std::map<std::string, JsonValue>> extensions;
    std::optional<JsonValue> experimental;
};

// ── Serialization ──
JsonValue SerializeToolsCapability(const ToolsCapability& v);
ToolsCapability DeserializeToolsCapability(const JsonValue& j);
JsonValue SerializeResourcesCapability(const ResourcesCapability& v);
ResourcesCapability DeserializeResourcesCapability(const JsonValue& j);
JsonValue SerializePromptsCapability(const PromptsCapability& v);
PromptsCapability DeserializePromptsCapability(const JsonValue& j);
JsonValue SerializeSamplingCapability(const SamplingCapability& v);
SamplingCapability DeserializeSamplingCapability(const JsonValue& j);
JsonValue SerializeRootsCapability(const RootsCapability& v);
RootsCapability DeserializeRootsCapability(const JsonValue& j);
JsonValue SerializeElicitationCapability(const ElicitationCapability& v);
ElicitationCapability DeserializeElicitationCapability(const JsonValue& j);
JsonValue SerializeEmptyCapability(const EmptyCapability& v);
EmptyCapability DeserializeEmptyCapability(const JsonValue& j);
JsonValue SerializeServerCapabilities(const ServerCapabilities& v);
ServerCapabilities DeserializeServerCapabilities(const JsonValue& j);
JsonValue SerializeClientCapabilities(const ClientCapabilities& v);
ClientCapabilities DeserializeClientCapabilities(const JsonValue& j);

} // namespace mcp
