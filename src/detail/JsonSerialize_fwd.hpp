#pragma once
#include <mcp/JsonValue.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/Methods.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/Capabilities.hpp>

namespace mcp {

// Capabilities
JsonValue SerializeServerCapabilities(const ServerCapabilities& v);
ServerCapabilities DeserializeServerCapabilities(const JsonValue& j);
JsonValue SerializeClientCapabilities(const ClientCapabilities& v);
ClientCapabilities DeserializeClientCapabilities(const JsonValue& j);

// Implementation
JsonValue SerializeImplementation(const Implementation& v);
Implementation DeserializeImplementation(const JsonValue& j);

// Request params
JsonValue SerializeCallToolRequestParams(const CallToolRequestParams& v);
CallToolRequestParams DeserializeCallToolRequestParams(const JsonValue& j);
JsonValue SerializeGetPromptRequestParams(const GetPromptRequestParams& v);
GetPromptRequestParams DeserializeGetPromptRequestParams(const JsonValue& j);
JsonValue SerializeCompleteRequestParams(const CompleteRequestParams& v);
CompleteRequestParams DeserializeCompleteRequestParams(const JsonValue& j);
JsonValue SerializeInitializeRequestParams(const InitializeRequestParams& v);
InitializeRequestParams DeserializeInitializeRequestParams(const JsonValue& j);
JsonValue SerializeElicitRequestParams(const ElicitRequestParams& v);
ElicitRequestParams DeserializeElicitRequestParams(const JsonValue& j);
JsonValue SerializeCreateMessageRequestParams(const CreateMessageRequestParams& v);
CreateMessageRequestParams DeserializeCreateMessageRequestParams(const JsonValue& j);
JsonValue SerializeResourceRequestParams(const ResourceRequestParams& v);
ResourceRequestParams DeserializeResourceRequestParams(const JsonValue& j);
JsonValue SerializeSubscriptionsListenRequestParams(const SubscriptionsListenRequestParams& v);
SubscriptionsListenRequestParams DeserializeSubscriptionsListenRequestParams(const JsonValue& j);
JsonValue SerializeGetTaskRequestParams(const GetTaskRequestParams& v);
GetTaskRequestParams DeserializeGetTaskRequestParams(const JsonValue& j);
JsonValue SerializeUpdateTaskRequestParams(const UpdateTaskRequestParams& v);
UpdateTaskRequestParams DeserializeUpdateTaskRequestParams(const JsonValue& j);
JsonValue SerializeCancelTaskRequestParams(const CancelTaskRequestParams& v);
CancelTaskRequestParams DeserializeCancelTaskRequestParams(const JsonValue& j);

// Results
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
JsonValue SerializeEmptyResult(const EmptyResult& v);
EmptyResult DeserializeEmptyResult(const JsonValue& j);
JsonValue SerializeCreateMessageResult(const CreateMessageResult& v);
CreateMessageResult DeserializeCreateMessageResult(const JsonValue& j);
JsonValue SerializeListRootsResult(const ListRootsResult& v);
ListRootsResult DeserializeListRootsResult(const JsonValue& j);
JsonValue SerializeElicitResult(const ElicitResult& v);
ElicitResult DeserializeElicitResult(const JsonValue& j);
JsonValue SerializeGetTaskResult(const GetTaskResult& v);
GetTaskResult DeserializeGetTaskResult(const JsonValue& j);

// InputRequiredResult
JsonValue SerializeInputRequiredResult(const InputRequiredResult& v);
InputRequiredResult DeserializeInputRequiredResult(const JsonValue& j);

// Meta
JsonValue SerializeRequestMeta(const RequestMeta& v);
RequestMeta DeserializeRequestMeta(const JsonValue& j);
JsonValue SerializeCacheHint(const CacheHint& v);
CacheHint DeserializeCacheHint(const JsonValue& j);
JsonValue SerializePagination(const Pagination& v);

// Logging
JsonValue SerializeLoggingLevelValue(LoggingLevel l);
LoggingLevel DeserializeLoggingLevelValue(const JsonValue& j);

// Content
JsonValue SerializePromptMessage(const PromptMessage& v);
PromptMessage DeserializePromptMessage(const JsonValue& j);

// Tool
JsonValue SerializeTool(const Tool& v);
Tool DeserializeTool(const JsonValue& j);

} // namespace mcp
