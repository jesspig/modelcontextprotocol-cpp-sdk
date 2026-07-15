#pragma once

#include <mcp/McpTypes.hpp>

#include <functional>

namespace mcp {

template <typename TParams>
class RequestContext;

// ── Typed handler delegates (对应 C# McpServerHandlers) ──
// Each handles one MCP method. The RequestContext carries
// typed params, server access, and transport context.

template <typename TParams, typename TResult>
using TypedRequestHandler = std::function<
    void(const RequestContext<TParams>&, std::promise<TResult>)>;

template <typename TParams>
using TypedNotificationHandler = std::function<
    void(const RequestContext<TParams>&)>;

// ── Concrete handler types for each MCP method ──
using ListToolsHandler = TypedRequestHandler<ListToolsRequestParams, ListToolsResult>;
using CallToolHandler = TypedRequestHandler<CallToolRequestParams, CallToolResult>;
using ListResourcesHandler = TypedRequestHandler<ListResourcesRequestParams, ListResourcesResult>;
using ReadResourceHandler = TypedRequestHandler<ReadResourceRequestParams, ReadResourceResult>;
using ListResourceTemplatesHandler = TypedRequestHandler<ListResourceTemplatesRequestParams, ListResourceTemplatesResult>;
using SubscribeResourcesHandler = TypedRequestHandler<SubscribeRequestParams, EmptyResult>;
using UnsubscribeResourcesHandler = TypedRequestHandler<UnsubscribeRequestParams, EmptyResult>;
using ListPromptsHandler = TypedRequestHandler<ListPromptsRequestParams, ListPromptsResult>;
using GetPromptHandler = TypedRequestHandler<GetPromptRequestParams, GetPromptResult>;
using CompleteHandler = TypedRequestHandler<CompleteRequestParams, CompleteResult>;
using SetLoggingLevelHandler = TypedRequestHandler<SetLevelRequestParams, EmptyResult>;

// ── ServerHandlers aggregate (对应 C# McpServerHandlers) ──
struct ServerHandlers {
    ListToolsHandler list_tools_handler;
    CallToolHandler call_tool_handler;
    ListResourcesHandler list_resources_handler;
    ReadResourceHandler read_resource_handler;
    ListResourceTemplatesHandler list_resource_templates_handler;
    SubscribeResourcesHandler subscribe_handler;
    UnsubscribeResourcesHandler unsubscribe_handler;
    ListPromptsHandler list_prompts_handler;
    GetPromptHandler get_prompt_handler;
    CompleteHandler complete_handler;
    SetLoggingLevelHandler set_logging_level_handler;
};

} // namespace mcp
