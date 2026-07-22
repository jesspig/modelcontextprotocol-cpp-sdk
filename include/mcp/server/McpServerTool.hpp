#pragma once

#include <mcp/Export.hpp>

#include <mcp/McpTypes.hpp>
#include <mcp/McpError.hpp>
#include <mcp/server/RequestContext.hpp>

#include <functional>
#include <memory>
#include <string_view>

namespace mcp {

class MCP_API McpServerTool : public std::enable_shared_from_this<McpServerTool> {
public:
    virtual ~McpServerTool() = default;
    virtual const Tool& ProtocolTool() const = 0;
    virtual void InvokeAsync(
        const RequestContext<CallToolRequestParams>& ctx,
        std::promise<CallToolResult> result_promise) = 0;

    template <typename Callable>
    static std::shared_ptr<McpServerTool> Create(
        std::string_view name,
        Callable&& callable,
        const ToolOptions& options = {});

};

template <typename Callable>
class MCP_API McpServerToolImpl : public McpServerTool {
public:
    McpServerToolImpl(
        std::string name,
        Callable callable,
        ToolOptions options)
        : callable_(std::move(callable))
    {
        tool_.name = std::move(name);
        if (options.title) tool_.title = std::move(options.title);
        if (options.description) tool_.description = std::move(options.description);
        if (options.icons.size()) tool_.icons = std::move(options.icons);
        if (options.meta) tool_.meta = std::move(options.meta);
        if (options.use_structured_content) {
            JsonValue::Object os;
            os["type"] = JsonValue("object");
            tool_.output_schema = JsonValue(std::move(os));
        }
        if (options.input_schema.has_value()) {
            tool_.input_schema = *options.input_schema;
        } else {
            JsonValue::Object is;
            is["type"] = JsonValue("object");
            is["properties"] = JsonValue(JsonValue::object_tag);
            tool_.input_schema = JsonValue(std::move(is));
        }
        if (options.read_only_hint || options.idempotent ||
            options.destructive || options.open_world_hint)
        {
            ToolAnnotations ann;
            ann.read_only_hint = options.read_only_hint;
            ann.idempotent_hint = options.idempotent;
            ann.destructive_hint = options.destructive;
            ann.open_world_hint = options.open_world_hint;
            tool_.annotations = std::move(ann);
        }
        if (options.output_schema) tool_.output_schema = options.output_schema;
    }

    const Tool& ProtocolTool() const override { return tool_; }

    void InvokeAsync(
        const RequestContext<CallToolRequestParams>& ctx,
        std::promise<CallToolResult> result_promise) override
    {
        try {
            auto result = callable_(ctx);
            result_promise.set_value(std::move(result));
        } catch (const McpError& e) {
            CallToolResult err_result;
            err_result.is_error = true;
            err_result.content.push_back(TextContent{"text", e.what()});
            result_promise.set_value(std::move(err_result));
        } catch (...) {
            CallToolResult err_result;
            err_result.is_error = true;
            err_result.content.push_back(TextContent{"text", "internal error"});
            result_promise.set_value(std::move(err_result));
        }
    }

private:
    Tool tool_;
    Callable callable_;
};

template <typename Callable>
std::shared_ptr<McpServerTool> McpServerTool::Create(
    std::string_view name,
    Callable&& callable,
    const ToolOptions& options)
{
    return std::make_shared<McpServerToolImpl<std::decay_t<Callable>>>(
        std::string(name),
        std::forward<Callable>(callable),
        options);
}

} // namespace mcp
