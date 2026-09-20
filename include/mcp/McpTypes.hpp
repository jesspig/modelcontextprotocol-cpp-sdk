#pragma once

#include <mcp/Content.hpp>
#include <mcp/Capabilities.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/Meta.hpp>
#include <mcp/Methods.hpp>

#include <mcp/JsonValue.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

struct ToolAnnotations {
    std::optional<std::string> title;
    std::optional<bool> read_only_hint;
    std::optional<bool> idempotent_hint;
    std::optional<bool> open_world_hint;
    std::optional<bool> destructive_hint;
};

enum class ToolExecutionMode { Auto, Manual, Task };
struct ToolExecution {
    ToolExecutionMode mode{ToolExecutionMode::Auto};
    std::optional<std::string> human_use;
};

struct ResourceAnnotations {
    std::optional<std::vector<std::string>> audience;
    std::optional<double> priority;
    std::optional<std::string> last_modified;
};

struct Tool {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    JsonValue input_schema;
    std::optional<JsonValue> output_schema;
    std::optional<ToolAnnotations> annotations;
    std::vector<Icon> icons;
    std::optional<JsonValue> meta;
    std::optional<ToolExecution> execution;
};

struct Resource {
    std::string uri;
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> mime_type;
    std::optional<int64_t> size;
    std::optional<std::vector<Icon>> icons;
    std::optional<ResourceAnnotations> annotations;
    std::optional<JsonValue> meta;
};

struct ResourceTemplate {
    std::string uri_template;
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> mime_type;
    std::optional<std::vector<Icon>> icons;
    std::optional<ResourceAnnotations> annotations;
    std::optional<JsonValue> meta;
};

struct PromptArgument {
    std::string name;
    std::optional<std::string> description;
    std::optional<bool> required;
};

struct Prompt {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::vector<PromptArgument>> arguments;
    std::optional<std::vector<Icon>> icons;
    std::optional<JsonValue> meta;
};

struct PromptMessage {
    std::string role;
    ContentVariant content;
};

struct Pagination {
    std::optional<std::string> next_cursor;
};

enum class ResultType { Complete, InputRequired };

struct Result {
    std::optional<JsonValue> meta;
    ResultType result_type{ResultType::Complete};
};

struct PaginatedRequestParams {
    std::optional<std::string> cursor;
    std::optional<RequestMeta> meta;
};
using ListToolsRequestParams = PaginatedRequestParams;
using ListResourcesRequestParams = PaginatedRequestParams;
using ListResourceTemplatesRequestParams = PaginatedRequestParams;
using ListPromptsRequestParams = PaginatedRequestParams;

struct ResourceRequestParams {
    std::string uri;
    std::optional<RequestMeta> meta;
};
using ReadResourceRequestParams = ResourceRequestParams;
using SubscribeRequestParams = ResourceRequestParams;
using UnsubscribeRequestParams = ResourceRequestParams;

struct CallToolRequestParams {
    std::string name;
    std::optional<JsonValue> arguments;
    std::optional<RequestMeta> meta;
    std::optional<JsonValue> input_responses;
    std::optional<std::string> request_state;
};

struct GetPromptRequestParams {
    std::string name;
    std::optional<JsonValue> arguments;
    std::optional<RequestMeta> meta;
};

struct CompleteRequestParams {
    JsonValue ref;
    std::string argument_name;
    std::string argument_value;
    std::optional<RequestMeta> meta;
};

struct DiscoverRequestParams {};

struct InitializeRequestParams {
    std::string protocol_version;
    ClientCapabilities capabilities;
    Implementation client_info;
};

struct SubscriptionFilter {
    std::optional<bool> tools_list_changed;
    std::optional<bool> prompts_list_changed;
    std::optional<bool> resources_list_changed;
    std::vector<std::string> resource_subscriptions;
};

struct SubscriptionsListenRequestParams {
    SubscriptionFilter notifications;
    std::optional<RequestMeta> meta;
};

struct SubscriptionsAcknowledgedNotificationParams {
    SubscriptionFilter notifications;
};

struct SamplingMessage {
    std::string role;
    ContentVariant content;
};

struct CreateMessageRequestParams {
    std::vector<SamplingMessage> messages;
    int64_t max_tokens;
    std::optional<std::string> stop_reason;
    std::optional<std::string> model_preference;
};

struct ListRootsRequestParams {};

struct InputRequest {
    std::string method;
    JsonValue params;
};

using InputRequests = std::map<std::string, InputRequest, std::less<>>;

struct InputRequiredResult {
    InputRequests input_requests;
    std::optional<std::string> request_state;
};

struct EmptyResult : Result {};

struct CallToolResult : Result {
    std::vector<ContentVariant> content;
    std::optional<JsonValue> structured_content;
    bool is_error{false};
    std::optional<InputRequiredResult> input_required;
};

struct ListToolsResult : Result {
    std::vector<Tool> tools;
    std::optional<std::string> next_cursor;
    std::optional<CacheHint> cache_hint;
};

struct ListResourcesResult : Result {
    std::vector<Resource> resources;
    std::optional<std::string> next_cursor;
    std::optional<CacheHint> cache_hint;
};

struct ListResourceTemplatesResult : Result {
    std::vector<ResourceTemplate> resource_templates;
    std::optional<std::string> next_cursor;
    std::optional<CacheHint> cache_hint;
};

struct ReadResourceResult : Result {
    std::vector<ResourceContents> contents;
    std::optional<CacheHint> cache_hint;
};

struct ListPromptsResult : Result {
    std::vector<Prompt> prompts;
    std::optional<std::string> next_cursor;
    std::optional<CacheHint> cache_hint;
};

struct GetPromptResult : Result {
    std::vector<PromptMessage> messages;
    std::optional<std::string> description;
};

struct CompleteResult : Result {
    JsonValue completion;
};

struct InitializeResult : Result {
    std::string protocol_version;
    ServerCapabilities capabilities;
    Implementation server_info;
    std::optional<std::string> instructions;
};

struct DiscoverResult : Result {
    std::vector<std::string> supported_versions;
    ServerCapabilities capabilities;
    Implementation server_info;
    std::optional<std::string> instructions;
    std::optional<CacheHint> cache_hint;
};

using PingResult = EmptyResult;

struct ProgressNotificationParams {
    ProgressToken progress_token;
    double progress;
    std::optional<double> total;
    std::optional<std::string> message;
};

struct CancelledNotificationParams {
    RequestId request_id;
    std::optional<std::string> reason;
};

struct LoggingMessageNotificationParams {
    LoggingLevel level;
    std::optional<std::string> logger;
    JsonValue data;
};

struct ElicitRequestParams {
    std::string message;
    std::optional<JsonValue> requested_schema;
    std::string mode{"form"};
    std::optional<std::string> url;
    std::optional<std::string> elicitation_id;
};

struct ElicitResult : Result {
    std::string action;
    std::optional<JsonValue> content;
};

template <typename T>
struct ElicitResultTyped {
    std::string action = "cancel";
    std::optional<T> content;

    bool is_accepted() const { return action == "accept"; }
};

InputRequest MakeInputRequestForElicitation(const ElicitRequestParams& params);
InputRequest MakeInputRequestForSampling(const CreateMessageRequestParams& params);
InputRequest MakeInputRequestForRoots(const ListRootsRequestParams& params);
JsonValue MakeInputResponseFromElicitResult(const ElicitResult& result);
bool IsInputRequiredResult(const JsonValue& j);
std::optional<InputRequests> ExtractInputRequests(const JsonValue& result);

struct CreateMessageResult : Result {
    std::string role;
    ContentVariant content;
    std::string model;
    std::optional<std::string> stop_reason;
};

struct Root {
    std::string uri;
    std::optional<std::string> name;
};

struct ListRootsResult : Result {
    std::vector<Root> roots;
};

JsonValue MakeInputResponseFromCreateMessageResult(const CreateMessageResult& result);
JsonValue MakeInputResponseFromListRootsResult(const ListRootsResult& result);

struct RequestOptions {
    std::optional<JsonValue> meta;
    std::optional<int64_t> read_timeout_ms;
    std::optional<JsonValue> input_responses;
    std::optional<std::string> request_state;
    std::function<void(const ProgressNotificationParams&)> on_progress;
};

struct CacheableRequestOptions : RequestOptions {
    std::optional<std::string> cache_mode;
    std::optional<int64_t> max_age_ms;
};

struct SetLevelRequestParams {
    LoggingLevel level;
};

struct GetTaskResult {
    std::string task_id;
    std::string status;
    std::string task_result_type{"task"};
    std::optional<JsonValue> result;
    std::optional<std::string> error_message;
    std::optional<JsonValue> input_required;
    std::optional<JsonValue> meta;
};

struct CreateTaskResult : Result {
    std::string task_id;
    std::string status;
    std::string created_at;
};

using UpdateTaskResult = EmptyResult;
using CancelTaskResult = EmptyResult;

struct GetTaskRequestParams {
    std::string task_id;
};

struct UpdateTaskRequestParams {
    std::string task_id;
    std::optional<JsonValue> result;
};

struct CancelTaskRequestParams {
    std::string task_id;
    std::optional<std::string> reason;
};

struct TaskStatusNotificationParams {
    std::string task_id;
    std::string status;
};

struct ToolOptions {
    std::optional<std::string> name;
    std::optional<std::string> description;
    std::optional<std::string> title;
    std::optional<bool> destructive;
    std::optional<bool> idempotent;
    std::optional<bool> read_only_hint;
    std::optional<bool> open_world_hint;
    bool use_structured_content{false};
    std::optional<JsonValue> input_schema;
    std::optional<JsonValue> output_schema;
    std::vector<Icon> icons;
    std::optional<JsonValue> meta;
    std::optional<ToolExecution> execution;

    ToolOptions& Description(std::string_view d) { description = std::string(d); return *this; }
    ToolOptions& Title(std::string_view t) { title = std::string(t); return *this; }
    ToolOptions& InputSchema(JsonValue s) { input_schema = std::move(s); return *this; }
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
    std::optional<std::vector<PromptArgument>> arguments;

    PromptOptions& Description(std::string_view d) { description = std::string(d); return *this; }
    PromptOptions& Title(std::string_view t) { title = std::string(t); return *this; }
    PromptOptions& Arguments(std::vector<PromptArgument> a) { arguments = std::move(a); return *this; }
};

JsonValue SerializeToolAnnotations(const ToolAnnotations& v);
ToolAnnotations DeserializeToolAnnotations(const JsonValue& j);
JsonValue SerializeToolExecution(const ToolExecution& v);
ToolExecution DeserializeToolExecution(const JsonValue& j);
JsonValue SerializeResourceAnnotations(const ResourceAnnotations& v);
ResourceAnnotations DeserializeResourceAnnotations(const JsonValue& j);

JsonValue SerializeEmptyResult(const EmptyResult& v);
EmptyResult DeserializeEmptyResult(const JsonValue& j);
JsonValue SerializeTool(const Tool& v);
Tool DeserializeTool(const JsonValue& j);
JsonValue SerializeResource(const Resource& v);
Resource DeserializeResource(const JsonValue& j);
JsonValue SerializeResourceTemplate(const ResourceTemplate& v);
ResourceTemplate DeserializeResourceTemplate(const JsonValue& j);
JsonValue SerializePromptArgument(const PromptArgument& v);
PromptArgument DeserializePromptArgument(const JsonValue& j);
JsonValue SerializePrompt(const Prompt& v);
Prompt DeserializePrompt(const JsonValue& j);
JsonValue SerializePromptMessage(const PromptMessage& v);
PromptMessage DeserializePromptMessage(const JsonValue& j);

JsonValue SerializePaginatedRequestParams(const PaginatedRequestParams& v);
PaginatedRequestParams DeserializePaginatedRequestParams(const JsonValue& j);
JsonValue SerializeResourceRequestParams(const ResourceRequestParams& v);
ResourceRequestParams DeserializeResourceRequestParams(const JsonValue& j);
JsonValue SerializeCallToolRequestParams(const CallToolRequestParams& v);
CallToolRequestParams DeserializeCallToolRequestParams(const JsonValue& j);
GetPromptRequestParams DeserializeGetPromptRequestParams(const JsonValue& j);
JsonValue SerializeCompleteRequestParams(const CompleteRequestParams& v);
CompleteRequestParams DeserializeCompleteRequestParams(const JsonValue& j);
JsonValue SerializeDiscoverRequestParams(const DiscoverRequestParams& v);
JsonValue SerializeInitializeRequestParams(const InitializeRequestParams& v);
InitializeRequestParams DeserializeInitializeRequestParams(const JsonValue& j);

JsonValue SerializeSubscriptionFilter(const SubscriptionFilter& v);
SubscriptionFilter DeserializeSubscriptionFilter(const JsonValue& j);
JsonValue SerializeSubscriptionsListenRequestParams(const SubscriptionsListenRequestParams& v);
SubscriptionsListenRequestParams DeserializeSubscriptionsListenRequestParams(const JsonValue& j);
JsonValue SerializeSubscriptionsAcknowledgedNotificationParams(const SubscriptionsAcknowledgedNotificationParams& v);
SubscriptionsAcknowledgedNotificationParams DeserializeSubscriptionsAcknowledgedNotificationParams(const JsonValue& j);

JsonValue SerializeCallToolResult(const CallToolResult& v);
CallToolResult DeserializeCallToolResult(const JsonValue& j);
JsonValue SerializeListToolsResult(const ListToolsResult& v);
ListToolsResult DeserializeListToolsResult(const JsonValue& j);
JsonValue SerializeListResourcesResult(const ListResourcesResult& v);
ListResourcesResult DeserializeListResourcesResult(const JsonValue& j);
JsonValue SerializeListResourceTemplatesResult(const ListResourceTemplatesResult& v);
ListResourceTemplatesResult DeserializeListResourceTemplatesResult(const JsonValue& j);
JsonValue SerializeReadResourceResult(const ReadResourceResult& v);
ReadResourceResult DeserializeReadResourceResult(const JsonValue& j);
JsonValue SerializeListPromptsResult(const ListPromptsResult& v);
ListPromptsResult DeserializeListPromptsResult(const JsonValue& j);
JsonValue SerializeGetPromptResult(const GetPromptResult& v);
GetPromptResult DeserializeGetPromptResult(const JsonValue& j);
JsonValue SerializeCompleteResult(const CompleteResult& v);
CompleteResult DeserializeCompleteResult(const JsonValue& j);
JsonValue SerializeInitializeResult(const InitializeResult& v);
InitializeResult DeserializeInitializeResult(const JsonValue& j);
JsonValue SerializeDiscoverResult(const DiscoverResult& v);
DiscoverResult DeserializeDiscoverResult(const JsonValue& j);

JsonValue SerializeInputRequest(const InputRequest& v);
InputRequest DeserializeInputRequest(const JsonValue& j);
JsonValue SerializeInputRequests(const InputRequests& v);
InputRequests DeserializeInputRequests(const JsonValue& j);
JsonValue SerializeInputRequiredResult(const InputRequiredResult& v);
InputRequiredResult DeserializeInputRequiredResult(const JsonValue& j);

JsonValue SerializeProgressNotificationParams(const ProgressNotificationParams& v);
ProgressNotificationParams DeserializeProgressNotificationParams(const JsonValue& j);
JsonValue SerializeLoggingMessageNotificationParams(const LoggingMessageNotificationParams& v);
LoggingMessageNotificationParams DeserializeLoggingMessageNotificationParams(const JsonValue& j);

JsonValue SerializeElicitRequestParams(const ElicitRequestParams& v);
ElicitRequestParams DeserializeElicitRequestParams(const JsonValue& j);
JsonValue SerializeElicitResult(const ElicitResult& v);
ElicitResult DeserializeElicitResult(const JsonValue& j);

JsonValue SerializeSamplingMessage(const SamplingMessage& v);
SamplingMessage DeserializeSamplingMessage(const JsonValue& j);
JsonValue SerializeCreateMessageRequestParams(const CreateMessageRequestParams& v);
CreateMessageRequestParams DeserializeCreateMessageRequestParams(const JsonValue& j);
JsonValue SerializeCreateMessageResult(const CreateMessageResult& v);

JsonValue SerializeRoot(const Root& v);
JsonValue SerializeListRootsRequestParams(const ListRootsRequestParams& v);
ListRootsRequestParams DeserializeListRootsRequestParams(const JsonValue& j);
JsonValue SerializeListRootsResult(const ListRootsResult& v);

JsonValue SerializeSetLevelRequestParams(const SetLevelRequestParams& v);
SetLevelRequestParams DeserializeSetLevelRequestParams(const JsonValue& j);

JsonValue SerializeGetTaskResult(const GetTaskResult& v);
GetTaskResult DeserializeGetTaskResult(const JsonValue& j);
JsonValue SerializeCreateTaskResult(const CreateTaskResult& v);
CreateTaskResult DeserializeCreateTaskResult(const JsonValue& j);
JsonValue SerializeGetTaskRequestParams(const GetTaskRequestParams& v);
GetTaskRequestParams DeserializeGetTaskRequestParams(const JsonValue& j);
JsonValue SerializeUpdateTaskRequestParams(const UpdateTaskRequestParams& v);
UpdateTaskRequestParams DeserializeUpdateTaskRequestParams(const JsonValue& j);
JsonValue SerializeCancelTaskRequestParams(const CancelTaskRequestParams& v);
CancelTaskRequestParams DeserializeCancelTaskRequestParams(const JsonValue& j);
JsonValue SerializeTaskStatusNotificationParams(const TaskStatusNotificationParams& v);

} // namespace mcp
