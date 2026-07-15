#pragma once

#include <mcp/Content.hpp>
#include <mcp/Capabilities.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/Meta.hpp>
#include <mcp/Methods.hpp>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace mcp {



// JSON Value �?represents any JSON value (2026-07-28)
// Used for structuredContent, arbitrary metadata, etc.
using JsonValue = nlohmann::json;

// ====================================================================
// ToolAnnotations
// ====================================================================
struct ToolAnnotations {
    std::optional<std::string> title;
    std::optional<bool> read_only_hint;
    std::optional<bool> idempotent_hint;
    std::optional<bool> open_world_hint;
    std::optional<bool> destructive_hint;
};
inline void to_json(nlohmann::json& j, const ToolAnnotations& v) {
    j = nlohmann::json::object();
    if (v.title)            j["title"] = *v.title;
    if (v.read_only_hint)   j["readOnlyHint"] = *v.read_only_hint;
    if (v.idempotent_hint)  j["idempotentHint"] = *v.idempotent_hint;
    if (v.open_world_hint)  j["openWorldHint"] = *v.open_world_hint;
    if (v.destructive_hint) j["destructiveHint"] = *v.destructive_hint;
}
inline void from_json(const nlohmann::json& j, ToolAnnotations& v) {
    if (auto it = j.find("title"); it != j.end())            v.title = it->get<std::string>();
    if (auto it = j.find("readOnlyHint"); it != j.end())     v.read_only_hint = it->get<bool>();
    if (auto it = j.find("idempotentHint"); it != j.end())   v.idempotent_hint = it->get<bool>();
    if (auto it = j.find("openWorldHint"); it != j.end())    v.open_world_hint = it->get<bool>();
    if (auto it = j.find("destructiveHint"); it != j.end())  v.destructive_hint = it->get<bool>();
}

// ====================================================================
// ResourceAnnotations
// ====================================================================
struct ResourceAnnotations {
    std::optional<std::vector<std::string>> audience;
    std::optional<double> priority;
};
inline void to_json(nlohmann::json& j, const ResourceAnnotations& v) {
    j = nlohmann::json::object();
    if (v.audience) j["audience"] = *v.audience;
    if (v.priority) j["priority"] = *v.priority;
}
inline void from_json(const nlohmann::json& j, ResourceAnnotations& v) {
    if (auto it = j.find("audience"); it != j.end()) v.audience = it->get<std::vector<std::string>>();
    if (auto it = j.find("priority"); it != j.end()) v.priority = it->get<double>();
}

// ====================================================================
// Tool
// ====================================================================
struct Tool {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    nlohmann::json input_schema;                         // JSON Schema 2020-12（支�?$ref、oneOf、if/then/else 等）
    std::optional<nlohmann::json> output_schema;         // JSON Schema 2020-12（同上）
    std::optional<ToolAnnotations> annotations;
    std::vector<Icon> icons;
    std::optional<nlohmann::json> meta;
};
inline void to_json(nlohmann::json& j, const Tool& v) {
    j = nlohmann::json::object();
    j["name"] = v.name;
    if (v.title)        j["title"] = *v.title;
    if (v.description)  j["description"] = *v.description;
    j["inputSchema"] = v.input_schema;
    if (v.output_schema) j["outputSchema"] = *v.output_schema;
    if (v.annotations)  j["annotations"] = *v.annotations;
    if (!v.icons.empty()) j["icons"] = v.icons;
    if (v.meta)         j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, Tool& v) {
    j.at("name").get_to(v.name);
    if (auto it = j.find("title"); it != j.end())        v.title = it->get<std::string>();
    if (auto it = j.find("description"); it != j.end())  v.description = it->get<std::string>();
    v.input_schema = j.at("inputSchema");
    if (auto it = j.find("outputSchema"); it != j.end()) v.output_schema = *it;
    if (auto it = j.find("annotations"); it != j.end())  v.annotations = it->get<ToolAnnotations>();
    if (auto it = j.find("icons"); it != j.end())        v.icons = it->get<std::vector<Icon>>();
    if (auto it = j.find("_meta"); it != j.end())        v.meta = *it;
}

// ====================================================================
// Resource
// ====================================================================
struct Resource {
    std::string uri;
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> mime_type;
    std::optional<int64_t> size;
    std::optional<std::vector<Icon>> icons;
    std::optional<ResourceAnnotations> annotations;
    std::optional<nlohmann::json> meta;
};
inline void to_json(nlohmann::json& j, const Resource& v) {
    j = nlohmann::json::object();
    j["uri"] = v.uri; j["name"] = v.name;
    if (v.title)       j["title"] = *v.title;
    if (v.description) j["description"] = *v.description;
    if (v.mime_type)   j["mimeType"] = *v.mime_type;
    if (v.size)        j["size"] = *v.size;
    if (v.icons)       j["icons"] = *v.icons;
    if (v.annotations) j["annotations"] = *v.annotations;
    if (v.meta)        j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, Resource& v) {
    j.at("uri").get_to(v.uri); j.at("name").get_to(v.name);
    if (auto it = j.find("title"); it != j.end())       v.title = it->get<std::string>();
    if (auto it = j.find("description"); it != j.end()) v.description = it->get<std::string>();
    if (auto it = j.find("mimeType"); it != j.end())    v.mime_type = it->get<std::string>();
    if (auto it = j.find("size"); it != j.end())        v.size = it->get<int64_t>();
    if (auto it = j.find("icons"); it != j.end())       v.icons = it->get<std::vector<Icon>>();
    if (auto it = j.find("annotations"); it != j.end()) v.annotations = it->get<ResourceAnnotations>();
    if (auto it = j.find("_meta"); it != j.end())       v.meta = *it;
}

// ====================================================================
// ResourceTemplate
// ====================================================================
struct ResourceTemplate {
    std::string uri_template;
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> mime_type;
    std::optional<std::vector<Icon>> icons;
    std::optional<ResourceAnnotations> annotations;
    std::optional<nlohmann::json> meta;
};
inline void to_json(nlohmann::json& j, const ResourceTemplate& v) {
    j = nlohmann::json::object();
    j["uriTemplate"] = v.uri_template; j["name"] = v.name;
    if (v.title)       j["title"] = *v.title;
    if (v.description) j["description"] = *v.description;
    if (v.mime_type)   j["mimeType"] = *v.mime_type;
    if (v.icons)       j["icons"] = *v.icons;
    if (v.annotations) j["annotations"] = *v.annotations;
    if (v.meta)        j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, ResourceTemplate& v) {
    j.at("uriTemplate").get_to(v.uri_template); j.at("name").get_to(v.name);
    if (auto it = j.find("title"); it != j.end())       v.title = it->get<std::string>();
    if (auto it = j.find("description"); it != j.end()) v.description = it->get<std::string>();
    if (auto it = j.find("mimeType"); it != j.end())    v.mime_type = it->get<std::string>();
    if (auto it = j.find("icons"); it != j.end())       v.icons = it->get<std::vector<Icon>>();
    if (auto it = j.find("annotations"); it != j.end()) v.annotations = it->get<ResourceAnnotations>();
    if (auto it = j.find("_meta"); it != j.end())       v.meta = *it;
}

// ====================================================================
// PromptArgument / Prompt / PromptMessage
// ====================================================================
struct PromptArgument {
    std::string name;
    std::optional<std::string> description;
    std::optional<bool> required;
};
inline void to_json(nlohmann::json& j, const PromptArgument& v) {
    j = nlohmann::json{{"name", v.name}};
    if (v.description) j["description"] = *v.description;
    if (v.required)    j["required"] = *v.required;
}
inline void from_json(const nlohmann::json& j, PromptArgument& v) {
    j.at("name").get_to(v.name);
    if (auto it = j.find("description"); it != j.end()) v.description = it->get<std::string>();
    if (auto it = j.find("required"); it != j.end())    v.required = it->get<bool>();
}

struct Prompt {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::vector<PromptArgument>> arguments;
    std::optional<std::vector<Icon>> icons;
    std::optional<ResourceAnnotations> annotations;
    std::optional<nlohmann::json> meta;
};
inline void to_json(nlohmann::json& j, const Prompt& v) {
    j = nlohmann::json{{"name", v.name}};
    if (v.title)       j["title"] = *v.title;
    if (v.description) j["description"] = *v.description;
    if (v.arguments)   j["arguments"] = *v.arguments;
    if (v.icons)       j["icons"] = *v.icons;
    if (v.annotations) j["annotations"] = *v.annotations;
    if (v.meta)        j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, Prompt& v) {
    j.at("name").get_to(v.name);
    if (auto it = j.find("title"); it != j.end())       v.title = it->get<std::string>();
    if (auto it = j.find("description"); it != j.end()) v.description = it->get<std::string>();
    if (auto it = j.find("arguments"); it != j.end())   v.arguments = it->get<std::vector<PromptArgument>>();
    if (auto it = j.find("icons"); it != j.end())       v.icons = it->get<std::vector<Icon>>();
    if (auto it = j.find("annotations"); it != j.end()) v.annotations = it->get<ResourceAnnotations>();
    if (auto it = j.find("_meta"); it != j.end())       v.meta = *it;
}

struct PromptMessage {
    std::string role;
    ContentVariant content;
};
inline void to_json(nlohmann::json& j, const PromptMessage& v) {
    j = nlohmann::json{{"role", v.role}, {"content", v.content}};
}
inline void from_json(const nlohmann::json& j, PromptMessage& v) {
    v.role = j.at("role").get<std::string>();
    from_json(j.at("content"), v.content);
}

// ====================================================================
// Pagination
// ====================================================================
struct Pagination {
    std::optional<std::string> next_cursor;
};
inline void to_json(nlohmann::json& j, const Pagination& v) {
    j = nlohmann::json::object();
    if (v.next_cursor) j["nextCursor"] = *v.next_cursor;
}
inline void from_json(const nlohmann::json& j, Pagination& v) {
    if (auto it = j.find("nextCursor"); it != j.end()) v.next_cursor = it->get<std::string>();
}

// ====================================================================
// Base Result
// ====================================================================
enum class ResultType { Complete, InputRequired };

NLOHMANN_JSON_SERIALIZE_ENUM(ResultType, {
    {ResultType::Complete, "complete"},
    {ResultType::InputRequired, "input_required"},
})

struct Result {
    std::optional<nlohmann::json> meta;
    ResultType result_type{ResultType::Complete};
};

// ====================================================================
// Request params
// ====================================================================
struct ListToolsRequestParams {
    std::optional<std::string> cursor;
    std::optional<RequestMeta> meta;
};
inline void to_json(nlohmann::json& j, const ListToolsRequestParams& v) {
    j = nlohmann::json::object();
    if (v.cursor) j["cursor"] = *v.cursor;
    if (v.meta)   j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, ListToolsRequestParams& v) {
    if (auto it = j.find("cursor"); it != j.end()) v.cursor = it->get<std::string>();
    if (auto it = j.find("_meta"); it != j.end())  v.meta = it->get<RequestMeta>();
}

struct CallToolRequestParams {
    std::string name;
    std::optional<nlohmann::json> arguments;
    std::optional<RequestMeta> meta;
    std::optional<nlohmann::json> input_responses;  // MRTR: responses to InputRequired requests
    std::optional<std::string> request_state;        // MRTR: opaque state from server
};
inline void to_json(nlohmann::json& j, const CallToolRequestParams& v) {
    j = nlohmann::json{{"name", v.name}};
    if (v.arguments) j["arguments"] = *v.arguments;
    if (v.meta)      j["_meta"] = *v.meta;
    if (v.input_responses) j["inputResponses"] = *v.input_responses;
    if (v.request_state)   j["requestState"] = *v.request_state;
}
inline void from_json(const nlohmann::json& j, CallToolRequestParams& v) {
    v.name = j.at("name").get<std::string>();
    if (auto it = j.find("_meta"); it != j.end()) v.meta = *it;
    if (auto it = j.find("arguments"); it != j.end()) v.arguments = *it;
    if (auto it = j.find("inputResponses"); it != j.end()) v.input_responses = *it;
    if (auto it = j.find("requestState"); it != j.end()) v.request_state = it->get<std::string>();
}

struct ReadResourceRequestParams {
    std::string uri;
    std::optional<RequestMeta> meta;
};
inline void to_json(nlohmann::json& j, const ReadResourceRequestParams& v) {
    j = nlohmann::json{{"uri", v.uri}};
    if (v.meta) j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, ReadResourceRequestParams& v) {
    v.uri = j.at("uri").get<std::string>();
    if (auto it = j.find("_meta"); it != j.end())  v.meta = it->get<RequestMeta>();
}

struct ListResourcesRequestParams {
    std::optional<std::string> cursor;
    std::optional<RequestMeta> meta;
};
inline void to_json(nlohmann::json& j, const ListResourcesRequestParams& v) {
    j = nlohmann::json::object();
    if (v.cursor) j["cursor"] = *v.cursor;
    if (v.meta)   j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, ListResourcesRequestParams& v) {
    if (auto it = j.find("cursor"); it != j.end()) v.cursor = it->get<std::string>();
    if (auto it = j.find("_meta"); it != j.end())  v.meta = it->get<RequestMeta>();
}

struct ListResourceTemplatesRequestParams {
    std::optional<std::string> cursor;
    std::optional<RequestMeta> meta;
};
inline void to_json(nlohmann::json& j, const ListResourceTemplatesRequestParams& v) {
    j = nlohmann::json::object();
    if (v.cursor) j["cursor"] = *v.cursor;
    if (v.meta)   j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, ListResourceTemplatesRequestParams& v) {
    if (auto it = j.find("cursor"); it != j.end()) v.cursor = it->get<std::string>();
    if (auto it = j.find("_meta"); it != j.end())  v.meta = it->get<RequestMeta>();
}

struct SubscribeRequestParams {
    std::string uri;
    std::optional<RequestMeta> meta;
};
inline void to_json(nlohmann::json& j, const SubscribeRequestParams& v) {
    j = nlohmann::json{{"uri", v.uri}};
    if (v.meta) j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, SubscribeRequestParams& v) {
    v.uri = j.at("uri").get<std::string>();
    if (auto it = j.find("_meta"); it != j.end())  v.meta = it->get<RequestMeta>();
}

struct UnsubscribeRequestParams {
    std::string uri;
    std::optional<RequestMeta> meta;
};
inline void to_json(nlohmann::json& j, const UnsubscribeRequestParams& v) {
    j = nlohmann::json{{"uri", v.uri}};
    if (v.meta) j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, UnsubscribeRequestParams& v) {
    v.uri = j.at("uri").get<std::string>();
    if (auto it = j.find("_meta"); it != j.end())  v.meta = it->get<RequestMeta>();
}

struct ListPromptsRequestParams {
    std::optional<std::string> cursor;
    std::optional<RequestMeta> meta;
};
inline void to_json(nlohmann::json& j, const ListPromptsRequestParams& v) {
    j = nlohmann::json::object();
    if (v.cursor) j["cursor"] = *v.cursor;
    if (v.meta)   j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, ListPromptsRequestParams& v) {
    if (auto it = j.find("cursor"); it != j.end()) v.cursor = it->get<std::string>();
    if (auto it = j.find("_meta"); it != j.end())  v.meta = it->get<RequestMeta>();
}

struct GetPromptRequestParams {
    std::string name;
    std::optional<nlohmann::json> arguments;
    std::optional<RequestMeta> meta;
};
inline void to_json(nlohmann::json& j, const GetPromptRequestParams& v) {
    j = nlohmann::json{{"name", v.name}};
    if (v.arguments) j["arguments"] = *v.arguments;
    if (v.meta)      j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, GetPromptRequestParams& v) {
    v.name = j.at("name").get<std::string>();
    if (auto it = j.find("arguments"); it != j.end()) v.arguments = *it;
    if (auto it = j.find("_meta"); it != j.end())     v.meta = it->get<RequestMeta>();
}

struct CompleteRequestParams {
    nlohmann::json ref;
    std::string argument_name;
    std::string argument_value;
    std::optional<RequestMeta> meta;
};
inline void to_json(nlohmann::json& j, const CompleteRequestParams& v) {
    j = nlohmann::json{{"ref", v.ref}, {"argumentName", v.argument_name}, {"argumentValue", v.argument_value}};
    if (v.meta) j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, CompleteRequestParams& v) {
    v.ref = j.at("ref"); v.argument_name = j.at("argumentName").get<std::string>(); v.argument_value = j.at("argumentValue").get<std::string>();
    if (auto it = j.find("_meta"); it != j.end())  v.meta = it->get<RequestMeta>();
}

struct DiscoverRequestParams {
    std::optional<RequestMeta> meta;
};
inline void to_json(nlohmann::json& j, const DiscoverRequestParams&) { j = nlohmann::json::object(); }
inline void from_json(const nlohmann::json&, DiscoverRequestParams&) {}

struct InitializeRequestParams {
    std::string protocol_version;
    ClientCapabilities capabilities;   // client capabilities, not server
    Implementation client_info;
};
inline void to_json(nlohmann::json& j, const InitializeRequestParams& v) {
    j = nlohmann::json{{"protocolVersion", v.protocol_version}, {"capabilities", v.capabilities}, {"clientInfo", v.client_info}};
}
inline void from_json(const nlohmann::json& j, InitializeRequestParams& v) {
    j.at("protocolVersion").get_to(v.protocol_version);
    // capabilities in initialize response are server capabilities
    v.capabilities = j.at("capabilities").get<ClientCapabilities>();
    v.client_info = j.at("clientInfo").get<Implementation>();
}

// ====================================================================
// Subscription
// ====================================================================
struct SubscriptionFilter {
    std::optional<bool> tools_list_changed;
    std::optional<bool> prompts_list_changed;
    std::optional<bool> resources_list_changed;
    std::vector<std::string> resource_subscriptions;
};
inline void to_json(nlohmann::json& j, const SubscriptionFilter& v) {
    j = nlohmann::json::object();
    if (v.tools_list_changed.has_value())     j["toolsListChanged"] = *v.tools_list_changed;
    if (v.prompts_list_changed.has_value())   j["promptsListChanged"] = *v.prompts_list_changed;
    if (v.resources_list_changed.has_value()) j["resourcesListChanged"] = *v.resources_list_changed;
    if (!v.resource_subscriptions.empty())    j["resourceSubscriptions"] = v.resource_subscriptions;
}
inline void from_json(const nlohmann::json& j, SubscriptionFilter& v) {
    if (auto it = j.find("toolsListChanged"); it != j.end())     v.tools_list_changed = it->get<bool>();
    if (auto it = j.find("promptsListChanged"); it != j.end())   v.prompts_list_changed = it->get<bool>();
    if (auto it = j.find("resourcesListChanged"); it != j.end()) v.resources_list_changed = it->get<bool>();
    if (auto it = j.find("resourceSubscriptions"); it != j.end()) v.resource_subscriptions = it->get<decltype(v.resource_subscriptions)>();
}

struct SubscriptionsListenRequestParams {
    SubscriptionFilter notifications;
    std::optional<RequestMeta> meta;
};
inline void to_json(nlohmann::json& j, const SubscriptionsListenRequestParams& v) {
    j = nlohmann::json{{"notifications", v.notifications}};
    if (v.meta) j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, SubscriptionsListenRequestParams& v) {
    j.at("notifications").get_to(v.notifications);
    if (auto it = j.find("_meta"); it != j.end())  v.meta = it->get<RequestMeta>();
}

struct SubscriptionsAcknowledgedNotificationParams {
    SubscriptionFilter notifications;
};
inline void to_json(nlohmann::json& j, const SubscriptionsAcknowledgedNotificationParams& v) {
    j = nlohmann::json{{"notifications", v.notifications}};
}
inline void from_json(const nlohmann::json& j, SubscriptionsAcknowledgedNotificationParams& v) {
    j.at("notifications").get_to(v.notifications);
}

// ====================================================================
// Result types
// ====================================================================
struct EmptyResult : Result {};
inline void to_json(nlohmann::json& j, const EmptyResult& v) {
    j = nlohmann::json::object();
    j["resultType"] = v.result_type;
}
inline void from_json(const nlohmann::json& j, EmptyResult& v) {
    if (auto it = j.find("resultType"); it != j.end()) v.result_type = it->get<ResultType>();
}

struct CallToolResult : Result {
    std::vector<ContentVariant> content;
    // structuredContent �?any JSON value (2026-07-28 protocol feature)
    std::optional<JsonValue> structured_content;
    bool is_error{false};
};
inline void to_json(nlohmann::json& j, const CallToolResult& v) {
    j = nlohmann::json{{"content", v.content}, {"isError", v.is_error}};
    if (v.structured_content) j["structuredContent"] = *v.structured_content;
    if (v.meta)               j["_meta"] = *v.meta;
    j["resultType"] = v.result_type;
}
inline void from_json(const nlohmann::json& j, CallToolResult& v) {
    v.content = j.at("content").get<decltype(v.content)>();
    v.is_error = j.value("isError", false);
    if (auto it = j.find("structuredContent"); it != j.end()) v.structured_content = *it;
    if (auto it = j.find("resultType"); it != j.end())        v.result_type = it->get<ResultType>();
    if (auto it = j.find("_meta"); it != j.end())             v.meta = *it;
}

struct ListToolsResult : Result {
    std::vector<Tool> tools;
    std::optional<std::string> next_cursor;
    std::optional<CacheHint> cache_hint;
};
inline void to_json(nlohmann::json& j, const ListToolsResult& v) {
    j = nlohmann::json{{"tools", v.tools}};
    if (v.next_cursor) j["nextCursor"] = *v.next_cursor;
    if (v.cache_hint)  j["cacheHint"] = *v.cache_hint;
    if (v.meta)        j["_meta"] = *v.meta;
    j["resultType"] = v.result_type;
}
inline void from_json(const nlohmann::json& j, ListToolsResult& v) {
    v.tools = j.at("tools").get<decltype(v.tools)>();
    if (auto it = j.find("nextCursor"); it != j.end()) v.next_cursor = it->get<std::string>();
    if (auto it = j.find("cacheHint"); it != j.end())  v.cache_hint = it->get<CacheHint>();
    if (auto it = j.find("resultType"); it != j.end()) v.result_type = it->get<ResultType>();
    if (auto it = j.find("_meta"); it != j.end())      v.meta = *it;
}

struct ListResourcesResult : Result {
    std::vector<Resource> resources;
    std::optional<std::string> next_cursor;
    std::optional<CacheHint> cache_hint;
};
inline void to_json(nlohmann::json& j, const ListResourcesResult& v) {
    j = nlohmann::json{{"resources", v.resources}};
    if (v.next_cursor) j["nextCursor"] = *v.next_cursor;
    if (v.cache_hint)  j["cacheHint"] = *v.cache_hint;
    if (v.meta)        j["_meta"] = *v.meta;
    j["resultType"] = v.result_type;
}
inline void from_json(const nlohmann::json& j, ListResourcesResult& v) {
    v.resources = j.at("resources").get<decltype(v.resources)>();
    if (auto it = j.find("nextCursor"); it != j.end()) v.next_cursor = it->get<std::string>();
    if (auto it = j.find("cacheHint"); it != j.end())  v.cache_hint = it->get<CacheHint>();
    if (auto it = j.find("resultType"); it != j.end()) v.result_type = it->get<ResultType>();
    if (auto it = j.find("_meta"); it != j.end())      v.meta = *it;
}

struct ListResourceTemplatesResult : Result {
    std::vector<ResourceTemplate> resource_templates;
    std::optional<std::string> next_cursor;
    std::optional<CacheHint> cache_hint;
};
inline void to_json(nlohmann::json& j, const ListResourceTemplatesResult& v) {
    j = nlohmann::json{{"resourceTemplates", v.resource_templates}};
    if (v.next_cursor) j["nextCursor"] = *v.next_cursor;
    if (v.cache_hint)  j["cacheHint"] = *v.cache_hint;
    if (v.meta)        j["_meta"] = *v.meta;
    j["resultType"] = v.result_type;
}
inline void from_json(const nlohmann::json& j, ListResourceTemplatesResult& v) {
    v.resource_templates = j.at("resourceTemplates").get<decltype(v.resource_templates)>();
    if (auto it = j.find("nextCursor"); it != j.end()) v.next_cursor = it->get<std::string>();
    if (auto it = j.find("cacheHint"); it != j.end())  v.cache_hint = it->get<CacheHint>();
    if (auto it = j.find("resultType"); it != j.end()) v.result_type = it->get<ResultType>();
    if (auto it = j.find("_meta"); it != j.end())      v.meta = *it;
}

struct ReadResourceResult : Result {
    std::vector<ResourceContents> contents;
    std::optional<CacheHint> cache_hint;
};
inline void to_json(nlohmann::json& j, const ReadResourceResult& v) {
    j = nlohmann::json{{"contents", v.contents}};
    if (v.cache_hint) j["cacheHint"] = *v.cache_hint;
    if (v.meta)       j["_meta"] = *v.meta;
    j["resultType"] = v.result_type;
}
inline void from_json(const nlohmann::json& j, ReadResourceResult& v) {
    v.contents = j.at("contents").get<decltype(v.contents)>();
    if (auto it = j.find("cacheHint"); it != j.end()) v.cache_hint = it->get<CacheHint>();
    if (auto it = j.find("resultType"); it != j.end()) v.result_type = it->get<ResultType>();
    if (auto it = j.find("_meta"); it != j.end())     v.meta = *it;
}

struct ListPromptsResult : Result {
    std::vector<Prompt> prompts;
    std::optional<std::string> next_cursor;
    std::optional<CacheHint> cache_hint;
};
inline void to_json(nlohmann::json& j, const ListPromptsResult& v) {
    j = nlohmann::json{{"prompts", v.prompts}};
    if (v.next_cursor) j["nextCursor"] = *v.next_cursor;
    if (v.cache_hint)  j["cacheHint"] = *v.cache_hint;
    if (v.meta)        j["_meta"] = *v.meta;
    j["resultType"] = v.result_type;
}
inline void from_json(const nlohmann::json& j, ListPromptsResult& v) {
    v.prompts = j.at("prompts").get<decltype(v.prompts)>();
    if (auto it = j.find("nextCursor"); it != j.end()) v.next_cursor = it->get<std::string>();
    if (auto it = j.find("cacheHint"); it != j.end())  v.cache_hint = it->get<CacheHint>();
    if (auto it = j.find("resultType"); it != j.end()) v.result_type = it->get<ResultType>();
    if (auto it = j.find("_meta"); it != j.end())      v.meta = *it;
}

struct GetPromptResult : Result {
    std::vector<PromptMessage> messages;
    std::optional<std::string> description;
};
inline void to_json(nlohmann::json& j, const GetPromptResult& v) {
    j = nlohmann::json{{"messages", v.messages}};
    if (v.description) j["description"] = *v.description;
    if (v.meta)        j["_meta"] = *v.meta;
    j["resultType"] = v.result_type;
}
inline void from_json(const nlohmann::json& j, GetPromptResult& v) {
    v.messages = j.at("messages").get<decltype(v.messages)>();
    if (auto it = j.find("description"); it != j.end()) v.description = it->get<std::string>();
    if (auto it = j.find("resultType"); it != j.end()) v.result_type = it->get<ResultType>();
    if (auto it = j.find("_meta"); it != j.end())      v.meta = *it;
}

struct CompleteResult : Result {
    nlohmann::json completion;
};
inline void to_json(nlohmann::json& j, const CompleteResult& v) {
    j = nlohmann::json{{"completion", v.completion}};
    if (v.meta) j["_meta"] = *v.meta;
    j["resultType"] = v.result_type;
}
inline void from_json(const nlohmann::json& j, CompleteResult& v) {
    v.completion = j.at("completion");
    if (auto it = j.find("resultType"); it != j.end()) v.result_type = it->get<ResultType>();
    if (auto it = j.find("_meta"); it != j.end())      v.meta = *it;
}

struct InitializeResult : Result {
    std::string protocol_version;
    ServerCapabilities capabilities;
    Implementation server_info;
    std::optional<std::string> instructions;
};
inline void to_json(nlohmann::json& j, const InitializeResult& v) {
    j = nlohmann::json{{"protocolVersion", v.protocol_version}, {"capabilities", v.capabilities}, {"serverInfo", v.server_info}};
    if (v.instructions) j["instructions"] = *v.instructions;
    if (v.meta)         j["_meta"] = *v.meta;
    j["resultType"] = v.result_type;
}
inline void from_json(const nlohmann::json& j, InitializeResult& v) {
    j.at("protocolVersion").get_to(v.protocol_version);
    v.capabilities = j.at("capabilities").get<ServerCapabilities>();
    v.server_info = j.at("serverInfo").get<Implementation>();
    if (auto it = j.find("instructions"); it != j.end()) v.instructions = it->get<std::string>();
    if (auto it = j.find("resultType"); it != j.end()) v.result_type = it->get<ResultType>();
    if (auto it = j.find("_meta"); it != j.end())      v.meta = *it;
}

struct DiscoverResult : Result {
    std::vector<std::string> supported_versions;
    ServerCapabilities capabilities;
    Implementation server_info;
    std::optional<std::string> instructions;
    std::optional<CacheHint> cache_hint;
};
inline void to_json(nlohmann::json& j, const DiscoverResult& v) {
    j = nlohmann::json{{"supportedVersions", v.supported_versions}, {"capabilities", v.capabilities}, {"serverInfo", v.server_info}};
    if (v.instructions) j["instructions"] = *v.instructions;
    if (v.cache_hint)   j["cacheHint"] = *v.cache_hint;
    if (v.meta)         j["_meta"] = *v.meta;
    j["resultType"] = v.result_type;
}
inline void from_json(const nlohmann::json& j, DiscoverResult& v) {
    v.supported_versions = j.at("supportedVersions").get<decltype(v.supported_versions)>();
    v.capabilities = j.at("capabilities").get<ServerCapabilities>();
    v.server_info = j.at("serverInfo").get<Implementation>();
    if (auto it = j.find("instructions"); it != j.end()) v.instructions = it->get<std::string>();
    if (auto it = j.find("cacheHint"); it != j.end())    v.cache_hint = it->get<CacheHint>();
    if (auto it = j.find("resultType"); it != j.end()) v.result_type = it->get<ResultType>();
    if (auto it = j.find("_meta"); it != j.end())      v.meta = *it;
}

struct PingResult : Result {};
inline void to_json(nlohmann::json& j, const PingResult& v) {
    j = nlohmann::json::object();
    j["resultType"] = v.result_type;
}
inline void from_json(const nlohmann::json& j, PingResult& v) {
    if (auto it = j.find("resultType"); it != j.end()) v.result_type = it->get<ResultType>();
}

// ====================================================================
// InputRequiredResult (MRTR)
// ====================================================================
struct InputRequestElicit {
    std::string message;
    std::optional<nlohmann::json> requested_schema;
};
inline void to_json(nlohmann::json& j, const InputRequestElicit& v) {
    j = nlohmann::json{{"message", v.message}};
    if (v.requested_schema) j["requestedSchema"] = *v.requested_schema;
}
inline void from_json(const nlohmann::json& j, InputRequestElicit& v) {
    v.message = j.at("message").get<std::string>();
    if (auto it = j.find("requestedSchema"); it != j.end()) v.requested_schema = *it;
}

struct InputRequests {
    std::optional<InputRequestElicit> confirm;
    std::optional<InputRequestElicit> elicit;
};
inline void to_json(nlohmann::json& j, const InputRequests& v) {
    j = nlohmann::json::object();
    if (v.confirm) j["confirm"] = *v.confirm;
    if (v.elicit)  j["elicit"] = *v.elicit;
}
inline void from_json(const nlohmann::json& j, InputRequests& v) {
    if (auto it = j.find("confirm"); it != j.end()) v.confirm = it->get<InputRequestElicit>();
    if (auto it = j.find("elicit"); it != j.end())  v.elicit  = it->get<InputRequestElicit>();
}

struct InputRequiredResult {
    InputRequests input_requests;
    std::optional<std::string> request_state;
};
inline void to_json(nlohmann::json& j, const InputRequiredResult& v) {
    j = nlohmann::json{{"inputRequests", v.input_requests}, {"resultType", "input_required"}};
    if (v.request_state) j["requestState"] = *v.request_state;
}
inline void from_json(const nlohmann::json& j, InputRequiredResult& v) {
    v.input_requests = j.at("inputRequests").get<InputRequests>();
    if (auto it = j.find("requestState"); it != j.end()) v.request_state = it->get<std::string>();
}

// ====================================================================
// Notification params
// ====================================================================
struct ProgressNotificationParams {
    ProgressToken progress_token;
    double progress;
    std::optional<double> total;
    std::optional<std::string> message;
};
inline void to_json(nlohmann::json& j, const ProgressNotificationParams& v) {
    j = nlohmann::json::object();
    std::visit([&j](const auto& val) { j["progressToken"] = val; }, v.progress_token);
    j["progress"] = v.progress;
    if (v.total)   j["total"] = *v.total;
    if (v.message) j["message"] = *v.message;
}
inline void from_json(const nlohmann::json& j, ProgressNotificationParams& v) {
    {
        const auto& val = j.at("progressToken");
        if (val.is_string()) v.progress_token = val.get<std::string>();
        else v.progress_token = val.get<int64_t>();
    }
    v.progress = j.at("progress").get<double>();
    if (auto it = j.find("total"); it != j.end())   v.total = it->get<double>();
    if (auto it = j.find("message"); it != j.end()) v.message = it->get<std::string>();
}

struct CancelledNotificationParams {
    RequestId request_id;
    std::optional<std::string> reason;
};
inline void to_json(nlohmann::json& j, const CancelledNotificationParams& v) {
    j = nlohmann::json::object();
    std::visit([&j](const auto& val) { j["requestId"] = val; }, v.request_id);
    if (v.reason) j["reason"] = *v.reason;
}
inline void from_json(const nlohmann::json& j, CancelledNotificationParams& v) {
    {
        const auto& val = j.at("requestId");
        if (val.is_number_integer()) v.request_id = val.get<int64_t>();
        else v.request_id = val.get<std::string>();
    }
    if (auto it = j.find("reason"); it != j.end()) v.reason = it->get<std::string>();
}

struct InitializedNotificationParams {};
inline void to_json(nlohmann::json&, const InitializedNotificationParams&) {}
inline void from_json(const nlohmann::json&, InitializedNotificationParams&) {}

// ====================================================================
// Logging [deprecated]
// ====================================================================
struct LoggingMessageNotificationParams {
    LoggingLevel level;
    std::string logger;
    nlohmann::json data;
};
inline void to_json(nlohmann::json& j, const LoggingMessageNotificationParams& v) {
    j = nlohmann::json{{"level", v.level}, {"logger", v.logger}, {"data", v.data}};
}
inline void from_json(const nlohmann::json& j, LoggingMessageNotificationParams& v) {
    v.level = j.at("level").get<LoggingLevel>();
    v.logger = j.at("logger").get<std::string>();
    v.data = j.at("data");
}

// ====================================================================
// Elicitation
// ====================================================================
struct ElicitRequestParams {
    std::string message;
    std::optional<nlohmann::json> requested_schema;
};
inline void to_json(nlohmann::json& j, const ElicitRequestParams& v) {
    j = nlohmann::json{{"message", v.message}};
    if (v.requested_schema) j["requestedSchema"] = *v.requested_schema;
}
inline void from_json(const nlohmann::json& j, ElicitRequestParams& v) {
    v.message = j.at("message").get<std::string>();
    if (auto it = j.find("requestedSchema"); it != j.end()) v.requested_schema = *it;
}

struct ElicitResult : Result {
    std::optional<nlohmann::json> values;
};
inline void to_json(nlohmann::json& j, const ElicitResult& v) {
    j = nlohmann::json::object();
    if (v.values) j["values"] = *v.values;
    j["resultType"] = v.result_type;
    if (v.meta)   j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, ElicitResult& v) {
    if (auto it = j.find("values"); it != j.end()) v.values = *it;
    if (auto it = j.find("resultType"); it != j.end()) v.result_type = it->get<ResultType>();
    if (auto it = j.find("_meta"); it != j.end())  v.meta = *it;
}

// ── Typed elicitation result / schema builder ──

namespace detail {

inline std::string infer_json_type_name(const nlohmann::json& val) {
    switch (val.type()) {
        case nlohmann::json::value_t::null:            return "null";
        case nlohmann::json::value_t::boolean:         return "boolean";
        case nlohmann::json::value_t::number_integer:
        case nlohmann::json::value_t::number_unsigned:
        case nlohmann::json::value_t::number_float:    return "number";
        case nlohmann::json::value_t::string:          return "string";
        case nlohmann::json::value_t::array:           return "array";
        case nlohmann::json::value_t::object:          return "object";
        default:                                       return "string";
    }
}

template <typename T>
nlohmann::json build_json_schema() {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();

    T instance{};
    nlohmann::json j = instance;

    if (j.is_object()) {
        auto& properties = schema["properties"];
        auto& required = schema["required"] = nlohmann::json::array();

        for (auto it = j.begin(); it != j.end(); ++it) {
            nlohmann::json prop;
            prop["type"] = infer_json_type_name(it.value());
            properties[it.key()] = std::move(prop);
            required.push_back(it.key());
        }
    }

    return schema;
}

} // namespace detail

template <typename T>
struct ElicitResultTyped {
    std::string action = "cancel";
    std::optional<T> content;

    bool is_accepted() const { return action == "accept"; }
};

template <typename T>
inline void to_json(nlohmann::json& j, const ElicitResultTyped<T>& v) {
    j = nlohmann::json{{"action", v.action}};
    if (v.content) j["content"] = *v.content;
}

template <typename T>
inline void from_json(const nlohmann::json& j, ElicitResultTyped<T>& v) {
    j.at("action").get_to(v.action);
    if (auto it = j.find("content"); it != j.end()) v.content = it->get<T>();
}

// ── InputRequest 工厂方法 (对应 C# InputRequest.ForElicitation) ──
inline nlohmann::json MakeInputRequestForElicitation(const ElicitRequestParams& params) {
    nlohmann::json j;
    j["method"] = methods::kElicit;
    j["params"] = params;
    return j;
}

// ── InputResponse 工厂方法 (对应 C# InputResponse.FromElicitResult) ──
inline nlohmann::json MakeInputResponseFromElicitResult(const ElicitResult& result) {
    return result;
}

// 检查 JSON 是否为 InputRequired 结果
inline bool IsInputRequiredResult(const nlohmann::json& j) {
    auto it = j.find("resultType");
    return it != j.end() && *it == "input_required";
}

// 从 response 中提取 InputRequests
inline std::optional<InputRequests> ExtractInputRequests(const nlohmann::json& result) {
    auto it = result.find("inputRequests");
    if (it == result.end()) return std::nullopt;
    return it->get<InputRequests>();
}

// ====================================================================
// Sampling [deprecated] �?use Elicitation instead (SEP-2577)
// ====================================================================
// SamplingMessage is deprecated in 2026-07-28 (SEP-2577).
struct [[deprecated("Sampling is deprecated in protocol version 2026-07-28 (SEP-2577)")]] SamplingMessage {
    std::string role;
    ContentVariant content;
};
inline void to_json(nlohmann::json& j, const SamplingMessage& v) {
    j = nlohmann::json{{"role", v.role}, {"content", v.content}};
}
inline void from_json(const nlohmann::json& j, SamplingMessage& v) {
    v.role = j.at("role").get<std::string>();
    from_json(j.at("content"), v.content);
}

// CreateMessageRequestParams is deprecated in 2026-07-28 (SEP-2577).
struct [[deprecated("Sampling is deprecated in protocol version 2026-07-28 (SEP-2577)")]] CreateMessageRequestParams {
    std::vector<SamplingMessage> messages;
    int64_t max_tokens;
    std::optional<std::string> stop_reason;
    std::optional<std::string> model_preference;
};
inline void to_json(nlohmann::json& j, const CreateMessageRequestParams& v) {
    j = nlohmann::json{{"messages", v.messages}, {"maxTokens", v.max_tokens}};
    if (v.stop_reason)      j["stopReason"] = *v.stop_reason;
    if (v.model_preference) j["modelPreference"] = *v.model_preference;
}
inline void from_json(const nlohmann::json& j, CreateMessageRequestParams& v) {
    v.messages = j.at("messages").get<decltype(v.messages)>();
    v.max_tokens = j.at("maxTokens").get<int64_t>();
    if (auto it = j.find("stopReason"); it != j.end())      v.stop_reason = it->get<std::string>();
    if (auto it = j.find("modelPreference"); it != j.end()) v.model_preference = it->get<std::string>();
}

// CreateMessageResult is deprecated in 2026-07-28 (SEP-2577).
struct [[deprecated("Sampling is deprecated in protocol version 2026-07-28 (SEP-2577)")]] CreateMessageResult : Result {
    std::string role;
    ContentVariant content;
    std::string model;
    std::optional<std::string> stop_reason;
};
inline void to_json(nlohmann::json& j, const CreateMessageResult& v) {
    j = nlohmann::json{{"role", v.role}, {"content", v.content}, {"model", v.model}};
    j["resultType"] = v.result_type;
    if (v.stop_reason) j["stopReason"] = *v.stop_reason;
}
inline void from_json(const nlohmann::json& j, CreateMessageResult& v) {
    v.role = j.at("role").get<std::string>();
    from_json(j.at("content"), v.content);
    v.model = j.at("model").get<std::string>();
    if (auto it = j.find("stopReason"); it != j.end()) v.stop_reason = it->get<std::string>();
    if (auto it = j.find("resultType"); it != j.end()) v.result_type = it->get<ResultType>();
    if (auto it = j.find("_meta"); it != j.end())      v.meta = *it;
}

// ====================================================================
// Roots [deprecated]
// ====================================================================
struct [[deprecated("Roots is deprecated in protocol version 2026-07-28")]] Root {
    std::string uri;
    std::optional<std::string> name;
};
inline void to_json(nlohmann::json& j, const Root& v) {
    j = nlohmann::json{{"uri", v.uri}};
    if (v.name) j["name"] = *v.name;
}
inline void from_json(const nlohmann::json& j, Root& v) {
    v.uri = j.at("uri").get<std::string>();
    if (auto it = j.find("name"); it != j.end()) v.name = it->get<std::string>();
}

struct [[deprecated("Roots is deprecated in protocol version 2026-07-28")]] ListRootsRequestParams {};
inline void to_json(nlohmann::json& j, const ListRootsRequestParams&) { j = nlohmann::json::object(); }
inline void from_json(const nlohmann::json&, ListRootsRequestParams&) {}

struct [[deprecated("Roots is deprecated in protocol version 2026-07-28")]] ListRootsResult : Result {
    std::vector<Root> roots;
};
inline void to_json(nlohmann::json& j, const ListRootsResult& v) {
    j = nlohmann::json{{"roots", v.roots}};
    j["resultType"] = v.result_type;
    if (v.meta) j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, ListRootsResult& v) {
    v.roots = j.at("roots").get<decltype(v.roots)>();
    if (auto it = j.find("resultType"); it != j.end()) v.result_type = it->get<ResultType>();
    if (auto it = j.find("_meta"); it != j.end())      v.meta = *it;
}

// ====================================================================
// RequestOptions / CacheableRequestOptions
// ====================================================================
struct RequestOptions {
    std::optional<nlohmann::json> meta;
    std::optional<int64_t> read_timeout_ms;
    std::optional<nlohmann::json> input_responses;
    std::optional<std::string> request_state;
};

struct CacheableRequestOptions : RequestOptions {
    std::optional<std::string> cache_mode;
    std::optional<int64_t> max_age_ms;
};

// ====================================================================
// SetLevelRequestParams [deprecated]
// ====================================================================
struct [[deprecated("SetLevelRequestParams is deprecated")]] SetLevelRequestParams {
    LoggingLevel level;
};
inline void to_json(nlohmann::json& j, const SetLevelRequestParams& v) {
    j = nlohmann::json{{"level", v.level}};
}
inline void from_json(const nlohmann::json& j, SetLevelRequestParams& v) {
    v.level = j.at("level").get<LoggingLevel>();
}

// ====================================================================
// Tasks
// ====================================================================
struct GetTaskResult : Result {
    std::string task_id;
    std::string status;
    std::string task_result_type{"task"};
    std::optional<nlohmann::json> result;
    std::optional<std::string> error_message;
    std::optional<nlohmann::json> input_required;
};
inline void to_json(nlohmann::json& j, const GetTaskResult& v) {
    j = nlohmann::json{{"taskId", v.task_id}, {"status", v.status}, {"resultType", v.task_result_type}};
    if (v.result)         j["result"] = *v.result;
    if (v.error_message)  j["errorMessage"] = *v.error_message;
    if (v.input_required) j["inputRequired"] = *v.input_required;
    if (v.meta)           j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, GetTaskResult& v) {
    v.task_id = j.at("taskId").get<std::string>();
    v.status = j.at("status").get<std::string>();
    if (auto it = j.find("resultType"); it != j.end()) v.task_result_type = it->get<std::string>();
    if (auto it = j.find("result"); it != j.end()) v.result = *it;
    if (auto it = j.find("errorMessage"); it != j.end()) v.error_message = it->get<std::string>();
    if (auto it = j.find("inputRequired"); it != j.end()) v.input_required = *it;
    if (auto it = j.find("_meta"); it != j.end())    v.meta = *it;
}

struct UpdateTaskResult : Result {};
inline void to_json(nlohmann::json& j, const UpdateTaskResult& v) {
    j = nlohmann::json::object();
    j["resultType"] = v.result_type;
}
inline void from_json(const nlohmann::json& j, UpdateTaskResult& v) {
    if (auto it = j.find("resultType"); it != j.end()) v.result_type = it->get<ResultType>();
}

struct CancelTaskResult : Result {};
inline void to_json(nlohmann::json& j, const CancelTaskResult& v) {
    j = nlohmann::json::object();
    j["resultType"] = v.result_type;
}
inline void from_json(const nlohmann::json& j, CancelTaskResult& v) {
    if (auto it = j.find("resultType"); it != j.end()) v.result_type = it->get<ResultType>();
}

struct GetTaskRequestParams {
    std::string task_id;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GetTaskRequestParams, task_id)

struct UpdateTaskRequestParams {
    std::string task_id;
    std::optional<nlohmann::json> result;
};
inline void to_json(nlohmann::json& j, const UpdateTaskRequestParams& v) {
    j = nlohmann::json{{"taskId", v.task_id}};
    if (v.result) j["result"] = *v.result;
}
inline void from_json(const nlohmann::json& j, UpdateTaskRequestParams& v) {
    v.task_id = j.at("taskId").get<std::string>();
    if (auto it = j.find("result"); it != j.end()) v.result = *it;
}

struct CancelTaskRequestParams {
    std::string task_id;
    std::optional<std::string> reason;
};
inline void to_json(nlohmann::json& j, const CancelTaskRequestParams& v) {
    j = nlohmann::json{{"taskId", v.task_id}};
    if (v.reason) j["reason"] = *v.reason;
}
inline void from_json(const nlohmann::json& j, CancelTaskRequestParams& v) {
    v.task_id = j.at("taskId").get<std::string>();
    if (auto it = j.find("reason"); it != j.end()) v.reason = it->get<std::string>();
}

// ====================================================================
// Options �?registration helpers (移除�?Tool.hpp/Resource.hpp/Prompt.hpp 的替�?
// ====================================================================
struct ToolOptions {
    std::optional<std::string> name;
    std::optional<std::string> description;
    std::optional<std::string> title;
    std::optional<bool> destructive;
    std::optional<bool> idempotent;
    std::optional<bool> read_only_hint;
    std::optional<bool> open_world_hint;
    bool use_structured_content{false};
    std::optional<nlohmann::json> output_schema;
    std::vector<Icon> icons;
    std::optional<nlohmann::json> meta;

    ToolOptions& Description(std::string_view d) { description = std::string(d); return *this; }
    ToolOptions& Title(std::string_view t) { title = std::string(t); return *this; }
};

struct ResourceOptions {
    std::optional<std::string> name;
    std::optional<std::string> description;
    std::optional<std::string> title;
    std::optional<std::string> mime_type;
    std::vector<Icon> icons;

    ResourceOptions& Description(std::string_view d) { description = std::string(d); return *this; }
    ResourceOptions& Title(std::string_view t) { title = std::string(t); return *this; }
    ResourceOptions& MimeType(std::string_view m) { mime_type = std::string(m); return *this; }
};

struct PromptOptions {
    std::optional<std::string> name;
    std::optional<std::string> description;
    std::optional<std::string> title;
    std::vector<Icon> icons;

    PromptOptions& Description(std::string_view d) { description = std::string(d); return *this; }
    PromptOptions& Title(std::string_view t) { title = std::string(t); return *this; }
};

} // namespace mcp
