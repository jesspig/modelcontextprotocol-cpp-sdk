#include "ConformanceServer.hpp"

#include <mcp/McpError.hpp>

#include <chrono>
#include <functional>
#include <initializer_list>
#include <string>
#include <thread>
#include <variant>

namespace mcp::conformance {
namespace {

std::string ProgressTokenToString(const ProgressToken& token)
{
    if (auto* text = std::get_if<std::string>(&token)) return *text;
    if (auto* number = std::get_if<int64_t>(&token)) return std::to_string(*number);
    return "";
}

void SendRequestScopedLog(McpServer& server, const char* data)
{
    JsonValue params(JsonValue::object_tag);
    params["level"] = JsonValue("info");
    params["data"] = JsonValue(data);
    server.GetSessionHandler().SendNotification(
        mcp::notifications::kMessage, std::move(params));
}

CallToolResult SimpleText(const ToolContext&)
{
    return MakeTextResult("This is a simple text response for testing.");
}

CallToolResult MakeImage(const ToolContext&)
{
    CallToolResult result;
    result.content.push_back(ImageContent{"image", kTestImageBase64, "image/png"});
    return result;
}

CallToolResult MakeAudio(const ToolContext&)
{
    CallToolResult result;
    result.content.push_back(AudioContent{"audio", kTestAudioBase64, "audio/wav"});
    return result;
}

CallToolResult MakeEmbeddedResource(const ToolContext&)
{
    TextResourceContents contents;
    contents.uri = "test://embedded-resource";
    contents.mime_type = "text/plain";
    contents.text = "This is an embedded resource content.";
    EmbeddedResource embedded;
    embedded.resource = ResourceContents{std::move(contents)};
    CallToolResult result;
    result.content.push_back(std::move(embedded));
    return result;
}

CallToolResult MultipleContentTypes(const ToolContext&)
{
    TextResourceContents resource_text;
    resource_text.uri = "test://mixed-content-resource";
    resource_text.mime_type = "application/json";
    resource_text.text = R"({"test":"data","value":123})";
    EmbeddedResource embedded;
    embedded.resource = ResourceContents{std::move(resource_text)};

    CallToolResult result;
    result.content.push_back(TextContent{"text", "Multiple content types test:"});
    result.content.push_back(ImageContent{"image", kTestImageBase64, "image/png"});
    result.content.push_back(std::move(embedded));
    return result;
}

CallToolResult ErrorHandling(const ToolContext&)
{
    throw McpError(McpErrorCode::InternalError,
        "This tool intentionally returns an error for testing");
}

CallToolResult XMcpHeader(const ToolContext& ctx)
{
    std::string region = "<none>";
    if (ctx.Params().arguments) {
        if (auto* value = ctx.Params().arguments->Find("region"); value && value->IsString()) {
            region = value->GetString();
        }
    }
    return MakeTextResult("region=" + region);
}

CallToolResult JsonSchema2020_12(const ToolContext& ctx)
{
    std::string dumped = "{}";
    if (ctx.Params().arguments) {
        dumped = ctx.Params().arguments->Dump();
    }
    return MakeTextResult("JSON Schema 2020-12 tool called with: " + dumped);
}

JsonValue MakeTypeProp(const char* type)
{
    JsonValue value(JsonValue::object_tag);
    value["type"] = JsonValue(type);
    return value;
}

JsonValue MakeArray(std::initializer_list<const char*> items)
{
    JsonValue array(JsonValue::array_tag);
    for (const char* item : items) {
        array.PushBack(JsonValue(item));
    }
    return array;
}

JsonValue MakeSchema()
{
    JsonValue street(JsonValue::object_tag);
    street["type"] = JsonValue("string");
    JsonValue city(JsonValue::object_tag);
    city["type"] = JsonValue("string");
    JsonValue address_properties(JsonValue::object_tag);
    address_properties["street"] = std::move(street);
    address_properties["city"] = std::move(city);

    JsonValue address(JsonValue::object_tag);
    address["$anchor"] = JsonValue("addressDef");
    address["type"] = JsonValue("object");
    address["properties"] = std::move(address_properties);
    JsonValue defs(JsonValue::object_tag);
    defs["address"] = std::move(address);

    JsonValue contact_method(JsonValue::object_tag);
    contact_method["type"] = JsonValue("string");
    contact_method["enum"] = MakeArray({"phone", "email"});
    JsonValue address_ref(JsonValue::object_tag);
    address_ref["$ref"] = JsonValue("#/$defs/address");
    JsonValue properties(JsonValue::object_tag);
    properties["name"] = MakeTypeProp("string");
    properties["address"] = std::move(address_ref);
    properties["contactMethod"] = std::move(contact_method);
    properties["phone"] = MakeTypeProp("string");
    properties["email"] = MakeTypeProp("string");

    JsonValue required_phone(JsonValue::object_tag);
    required_phone["required"] = MakeArray({"phone"});
    JsonValue required_email(JsonValue::object_tag);
    required_email["required"] = MakeArray({"email"});
    JsonValue any_of_entry(JsonValue::object_tag);
    JsonValue any_of(JsonValue::array_tag);
    any_of.PushBack(std::move(required_phone));
    any_of.PushBack(std::move(required_email));
    any_of_entry["anyOf"] = std::move(any_of);
    JsonValue all_of(JsonValue::array_tag);
    all_of.PushBack(std::move(any_of_entry));

    JsonValue if_contact_method_const(JsonValue::object_tag);
    if_contact_method_const["const"] = JsonValue("phone");
    JsonValue if_properties(JsonValue::object_tag);
    if_properties["contactMethod"] = std::move(if_contact_method_const);
    JsonValue if_branch(JsonValue::object_tag);
    if_branch["properties"] = std::move(if_properties);
    if_branch["required"] = MakeArray({"contactMethod"});

    JsonValue then_branch(JsonValue::object_tag);
    then_branch["required"] = MakeArray({"phone"});
    JsonValue else_branch(JsonValue::object_tag);
    else_branch["required"] = MakeArray({"email"});

    JsonValue schema(JsonValue::object_tag);
    schema["$schema"] = JsonValue("https://json-schema.org/draft/2020-12/schema");
    schema["type"] = JsonValue("object");
    schema["$defs"] = std::move(defs);
    schema["properties"] = std::move(properties);
    schema["allOf"] = std::move(all_of);
    schema["if"] = std::move(if_branch);
    schema["then"] = std::move(then_branch);
    schema["else"] = std::move(else_branch);
    schema["additionalProperties"] = JsonValue(false);
    return schema;
}

JsonValue MakeXMcpHeaderSchema()
{
    JsonValue region(JsonValue::object_tag);
    region["type"] = JsonValue("string");
    region["description"] = JsonValue("mirrored into Mcp-Param-Region");
    region["x-mcp-header"] = JsonValue("Region");
    JsonValue level(JsonValue::object_tag);
    level["type"] = JsonValue("integer");
    level["description"] = JsonValue("non-mirrored argument");
    JsonValue properties(JsonValue::object_tag);
    properties["region"] = std::move(region);
    properties["level"] = std::move(level);
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(properties);
    return schema;
}

CallToolResult ToolWithLogging(const ToolContext& ctx)
{
    SendRequestScopedLog(ctx.Server(), "Tool execution started");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    SendRequestScopedLog(ctx.Server(), "Tool processing data");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    SendRequestScopedLog(ctx.Server(), "Tool execution completed");
    return MakeTextResult("Tool with logging executed successfully");
}

CallToolResult ToolWithProgress(const ToolContext& ctx)
{
    const ProgressToken* token = nullptr;
    if (ctx.Params().meta && ctx.Params().meta->progress_token) {
        token = &*ctx.Params().meta->progress_token;
    }
    if (token) {
        for (int progress : {0, 50, 100}) {
            ctx.Server().SendProgress(*token, static_cast<double>(progress),
                100.0, "Completed step " + std::to_string(progress) + " of 100");
            if (progress < 100) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        return MakeTextResult(ProgressTokenToString(*token));
    }
    return MakeTextResult("no-progress-token");
}

CallToolResult LoggingTool(const ToolContext& ctx)
{
    if (ctx.Params().meta && ctx.Params().meta->log_level
        && static_cast<int>(LoggingLevel::Info) >= static_cast<int>(*ctx.Params().meta->log_level)) {
        ctx.Log(LoggingLevel::Info,
            "test_logging_tool ran (delivered only when the request set _meta.logLevel)");
    }
    return MakeTextResult("logged through the request-scoped, logLevel-gated channel");
}

CallToolResult StreamingElicitation(const ToolContext&)
{
    return MakeTextResult("stream observed: result frames only, no top-level requests");
}

} // namespace

void RegisterContentTools(McpServer& server)
{
    server.RegisterTool("test_simple_text",
        ToolOptions{}.Description("Tests simple text content response"),
        std::function<CallToolResult(const ToolContext&)>(&SimpleText));

    server.RegisterTool("test_image_content",
        ToolOptions{}.Description("Tests image content response"),
        std::function<CallToolResult(const ToolContext&)>(&MakeImage));

    server.RegisterTool("test_audio_content",
        ToolOptions{}.Description("Tests audio content response"),
        std::function<CallToolResult(const ToolContext&)>(&MakeAudio));

    server.RegisterTool("test_embedded_resource",
        ToolOptions{}.Description("Tests embedded resource content response"),
        std::function<CallToolResult(const ToolContext&)>(&MakeEmbeddedResource));

    server.RegisterTool("test_multiple_content_types",
        ToolOptions{}.Description("Tests response with multiple content types (text, image, resource)"),
        std::function<CallToolResult(const ToolContext&)>(&MultipleContentTypes));

    server.RegisterTool("test_error_handling",
        ToolOptions{}.Description("Tests error response handling"),
        std::function<CallToolResult(const ToolContext&)>(&ErrorHandling));

    server.RegisterTool("test_x_mcp_header",
        ToolOptions{}
            .Description("Tests SEP-2243 Mcp-Param-* server-side validation")
            .InputSchema(MakeXMcpHeaderSchema()),
        std::function<CallToolResult(const ToolContext&)>(&XMcpHeader));

    server.RegisterTool("json_schema_2020_12_tool",
        ToolOptions{}
            .Description("Tool with JSON Schema 2020-12 features for conformance testing (SEP-1613, SEP-2106)")
            .InputSchema(MakeSchema()),
        std::function<CallToolResult(const ToolContext&)>(&JsonSchema2020_12));

    server.RegisterTool("test_logging_tool",
        ToolOptions{}
            .Description("SEP-2575: logs via ctx.mcpReq.log so the no-log-without-logLevel rule is exercised"),
        std::function<CallToolResult(const ToolContext&)>(&LoggingTool));

    server.RegisterTool("test_tool_with_logging",
        ToolOptions{}.Description("Tests tool that emits log messages during execution"),
        std::function<CallToolResult(const ToolContext&)>(&ToolWithLogging));

    server.RegisterTool("test_tool_with_progress",
        ToolOptions{}.Description("Tests tool that reports progress notifications"),
        std::function<CallToolResult(const ToolContext&)>(&ToolWithProgress));

    server.RegisterTool("test_streaming_elicitation",
        ToolOptions{}
            .Description("SEP-2575: yields a response stream carrying no independent top-level JSON-RPC requests"),
        std::function<CallToolResult(const ToolContext&)>(&StreamingElicitation));
}

}
