#include "ConformanceServer.hpp"

#include <mcp/McpError.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace mcp::conformance {
namespace {

constexpr auto kSamplingMethod = "sampling/createMessage";

CallToolResult Reconnection(const ToolContext&)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return MakeTextResult(
        "Reconnection test completed successfully. If you received this, "
        "the client properly reconnected after stream closure.");
}

std::string ArgumentsString(const ToolContext& ctx, const char* key)
{
    if (ctx.Params().arguments) {
        if (auto* value = ctx.Params().arguments->Find(key); value && value->IsString()) {
            return value->GetString();
        }
    }
    return "";
}

CallToolResult Sampling(const ToolContext& ctx)
{
    JsonValue text(JsonValue::object_tag);
    text["type"] = JsonValue("text");
    text["text"] = JsonValue(ArgumentsString(ctx, "prompt"));
    JsonValue message(JsonValue::object_tag);
    message["role"] = JsonValue("user");
    message["content"] = std::move(text);
    JsonValue messages(JsonValue::array_tag);
    messages.PushBack(std::move(message));
    JsonValue params(JsonValue::object_tag);
    params["messages"] = std::move(messages);
    params["maxTokens"] = JsonValue(static_cast<int64_t>(100));
    RequestMeta meta;
    auto negotiated = ctx.Server().GetNegotiatedProtocolVersion();
    meta.protocol_version = negotiated.empty()
        ? std::string(kLatestProtocolVersion) : negotiated;
    try {
        auto future = ctx.Server().GetSessionHandler().SendRequest(
            kSamplingMethod, std::move(params), meta);
        JsonValue result = future.get();
        if (auto* code = result.Find("code"); code && code->IsInt() && code->GetInt() < 0) {
            std::string detail = "request failed";
            if (auto* msg = result.Find("message"); msg && msg->IsString()) {
                detail = msg->GetString();
            }
            return MakeTextResult("Sampling not supported or error: " + detail);
        }
        std::string model_response;
        if (auto* content = result.Find("content")) {
            if (auto* t = content->Find("text"); t && t->IsString()) {
                model_response = t->GetString();
            }
        }
        if (model_response.empty()) {
            if (auto* nested = result.Find("message")) {
                if (auto* content = nested->Find("content")) {
                    if (auto* t = content->Find("text"); t && t->IsString()) {
                        model_response = t->GetString();
                    }
                }
            }
        }
        if (model_response.empty()) {
            model_response = "No response";
        }
        return MakeTextResult("LLM response: " + model_response);
    } catch (const std::exception& e) {
        return MakeTextResult(std::string("Sampling not supported or error: ") + e.what());
    }
}

std::string ElicitationSuccessText(const ElicitResult& result, const char* prefix)
{
    std::string content_json = "{}";
    if (result.content && !result.content->IsNull()) {
        content_json = result.content->Dump();
    }
    return std::string(prefix) + " action=" + result.action + ", content=" + content_json;
}

CallToolResult Elicitation(const ToolContext& ctx)
{
    ElicitRequestParams params;
    params.message = ArgumentsString(ctx, "message");
    JsonValue response_field(JsonValue::object_tag);
    response_field["type"] = JsonValue("string");
    response_field["description"] = JsonValue("User's response");
    JsonValue properties(JsonValue::object_tag);
    properties["response"] = std::move(response_field);
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(properties);
    JsonValue required(JsonValue::array_tag);
    required.PushBack(JsonValue("response"));
    schema["required"] = std::move(required);
    params.requested_schema = std::move(schema);
    try {
        auto result = ctx.Server().Elicit(params).get();
        return MakeTextResult(ElicitationSuccessText(result, "User response:"));
    } catch (const std::exception& e) {
        return MakeTextResult(std::string("Elicitation not supported or error: ") + e.what());
    }
}

CallToolResult ElicitationSep1034Defaults(const ToolContext& ctx)
{
    ElicitRequestParams params;
    params.message = "Please review and update the form fields with defaults";

    JsonValue name(JsonValue::object_tag);
    name["type"] = JsonValue("string");
    name["description"] = JsonValue("User name");
    name["default"] = JsonValue("John Doe");
    JsonValue age(JsonValue::object_tag);
    age["type"] = JsonValue("integer");
    age["description"] = JsonValue("User age");
    age["default"] = JsonValue(static_cast<int64_t>(30));
    JsonValue score(JsonValue::object_tag);
    score["type"] = JsonValue("number");
    score["description"] = JsonValue("User score");
    score["default"] = JsonValue(95.5);
    JsonValue status(JsonValue::object_tag);
    status["type"] = JsonValue("string");
    status["description"] = JsonValue("User status");
    JsonValue status_enum(JsonValue::array_tag);
    status_enum.PushBack(JsonValue("active"));
    status_enum.PushBack(JsonValue("inactive"));
    status_enum.PushBack(JsonValue("pending"));
    status["enum"] = std::move(status_enum);
    status["default"] = JsonValue("active");
    JsonValue verified(JsonValue::object_tag);
    verified["type"] = JsonValue("boolean");
    verified["description"] = JsonValue("Verification status");
    verified["default"] = JsonValue(true);
    JsonValue properties(JsonValue::object_tag);
    properties["name"] = std::move(name);
    properties["age"] = std::move(age);
    properties["score"] = std::move(score);
    properties["status"] = std::move(status);
    properties["verified"] = std::move(verified);
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(properties);
    schema["required"] = JsonValue(JsonValue::array_tag);

    params.requested_schema = std::move(schema);
    try {
        auto result = ctx.Server().Elicit(params).get();
        return MakeTextResult(ElicitationSuccessText(result, "Elicitation completed:"));
    } catch (const std::exception& e) {
        return MakeTextResult(std::string("Elicitation not supported or error: ") + e.what());
    }
}

JsonValue MakeEnumConst(const char* value, const char* title)
{
    JsonValue entry(JsonValue::object_tag);
    entry["const"] = JsonValue(value);
    entry["title"] = JsonValue(title);
    return entry;
}

CallToolResult ElicitationSep1330Enums(const ToolContext& ctx)
{
    ElicitRequestParams params;
    params.message = "Please select options from the enum fields";

    JsonValue untitled_single(JsonValue::object_tag);
    untitled_single["type"] = JsonValue("string");
    untitled_single["description"] = JsonValue("Select one option");
    JsonValue untitled_enum(JsonValue::array_tag);
    untitled_enum.PushBack(JsonValue("option1"));
    untitled_enum.PushBack(JsonValue("option2"));
    untitled_enum.PushBack(JsonValue("option3"));
    untitled_single["enum"] = std::move(untitled_enum);

    JsonValue titled_single(JsonValue::object_tag);
    titled_single["type"] = JsonValue("string");
    titled_single["description"] = JsonValue("Select one option with titles");
    JsonValue one_of(JsonValue::array_tag);
    one_of.PushBack(MakeEnumConst("value1", "First Option"));
    one_of.PushBack(MakeEnumConst("value2", "Second Option"));
    one_of.PushBack(MakeEnumConst("value3", "Third Option"));
    titled_single["oneOf"] = std::move(one_of);

    JsonValue legacy_enum(JsonValue::object_tag);
    legacy_enum["type"] = JsonValue("string");
    legacy_enum["description"] = JsonValue("Select one option (legacy)");
    JsonValue legacy_values(JsonValue::array_tag);
    legacy_values.PushBack(JsonValue("opt1"));
    legacy_values.PushBack(JsonValue("opt2"));
    legacy_values.PushBack(JsonValue("opt3"));
    legacy_enum["enum"] = std::move(legacy_values);
    JsonValue enum_names(JsonValue::array_tag);
    enum_names.PushBack(JsonValue("Option One"));
    enum_names.PushBack(JsonValue("Option Two"));
    enum_names.PushBack(JsonValue("Option Three"));
    legacy_enum["enumNames"] = std::move(enum_names);

    JsonValue multi_items_enum(JsonValue::object_tag);
    multi_items_enum["type"] = JsonValue("string");
    JsonValue multi_enum(JsonValue::array_tag);
    multi_enum.PushBack(JsonValue("option1"));
    multi_enum.PushBack(JsonValue("option2"));
    multi_enum.PushBack(JsonValue("option3"));
    multi_items_enum["enum"] = std::move(multi_enum);
    JsonValue untitled_multi(JsonValue::object_tag);
    untitled_multi["type"] = JsonValue("array");
    untitled_multi["description"] = JsonValue("Select multiple options");
    untitled_multi["minItems"] = JsonValue(static_cast<int64_t>(1));
    untitled_multi["maxItems"] = JsonValue(static_cast<int64_t>(3));
    untitled_multi["items"] = std::move(multi_items_enum);

    JsonValue multi_items_any_of(JsonValue::array_tag);
    multi_items_any_of.PushBack(MakeEnumConst("value1", "First Choice"));
    multi_items_any_of.PushBack(MakeEnumConst("value2", "Second Choice"));
    multi_items_any_of.PushBack(MakeEnumConst("value3", "Third Choice"));
    JsonValue titled_multi_items(JsonValue::object_tag);
    titled_multi_items["anyOf"] = std::move(multi_items_any_of);
    JsonValue titled_multi(JsonValue::object_tag);
    titled_multi["type"] = JsonValue("array");
    titled_multi["description"] = JsonValue("Select multiple options with titles");
    titled_multi["minItems"] = JsonValue(static_cast<int64_t>(1));
    titled_multi["maxItems"] = JsonValue(static_cast<int64_t>(3));
    titled_multi["items"] = std::move(titled_multi_items);

    JsonValue properties(JsonValue::object_tag);
    properties["untitledSingle"] = std::move(untitled_single);
    properties["titledSingle"] = std::move(titled_single);
    properties["legacyEnum"] = std::move(legacy_enum);
    properties["untitledMulti"] = std::move(untitled_multi);
    properties["titledMulti"] = std::move(titled_multi);
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(properties);
    schema["required"] = JsonValue(JsonValue::array_tag);

    params.requested_schema = std::move(schema);
    try {
        auto result = ctx.Server().Elicit(params).get();
        return MakeTextResult(ElicitationSuccessText(result, "Elicitation completed:"));
    } catch (const std::exception& e) {
        return MakeTextResult(std::string("Elicitation not supported or error: ") + e.what());
    }
}

CallToolResult MissingCapability(const ToolContext& ctx)
{
    if (ctx.Params().input_responses && ctx.Params().input_responses->Contains("message")) {
        return MakeTextResult("sampling round-trip complete");
    }
    SamplingMessage message;
    message.role = "user";
    TextContent content;
    content.text = "Reply with the single word: pong";
    message.content = std::move(content);
    CreateMessageRequestParams params;
    params.messages.push_back(std::move(message));
    params.max_tokens = 16;
    CallToolResult result;
    InputRequiredResult ir;
    ir.input_requests["message"] = MakeInputRequestForSampling(params);
    result.input_required = std::move(ir);
    return result;
}

JsonValue MakeStringDescription(const char* description)
{
    JsonValue value(JsonValue::object_tag);
    value["type"] = JsonValue("string");
    value["description"] = JsonValue(description);
    return value;
}

JsonValue MakeSamplingSchema()
{
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    JsonValue properties(JsonValue::object_tag);
    properties["prompt"] = MakeStringDescription("The prompt to send to the LLM");
    schema["properties"] = std::move(properties);
    JsonValue required(JsonValue::array_tag);
    required.PushBack(JsonValue("prompt"));
    schema["required"] = std::move(required);
    return schema;
}

JsonValue MakeElicitationSchema()
{
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    JsonValue properties(JsonValue::object_tag);
    properties["message"] = MakeStringDescription("The message to show the user");
    schema["properties"] = std::move(properties);
    JsonValue required(JsonValue::array_tag);
    required.PushBack(JsonValue("message"));
    schema["required"] = std::move(required);
    return schema;
}

CallToolResult TriggerToolChange(McpServer& server, const ToolContext&)
{
    server.SendToolListChanged();
    return MakeTextResult("tools_list_changed published");
}

CallToolResult TriggerPromptChange(McpServer& server, const ToolContext&)
{
    server.SendPromptListChanged();
    return MakeTextResult("prompts_list_changed published");
}

std::function<CallToolResult(const ToolContext&)> BindTrigger(
    McpServer& server,
    CallToolResult (*fn)(McpServer&, const ToolContext&))
{
    return [&server, fn](const ToolContext& ctx) { return fn(server, ctx); };
}

} // namespace

void RegisterInteractionTools(McpServer& server)
{
    server.RegisterTool("test_reconnection",
        ToolOptions{}
            .Description(
                "Tests SSE stream disconnection and client reconnection (SEP-1699). "
                "Server will close the stream mid-call and send the result after client reconnects."),
        std::function<CallToolResult(const ToolContext&)>(&Reconnection));

    server.RegisterTool("test_sampling",
        ToolOptions{}
            .Description("Tests server-initiated sampling (LLM completion request)")
            .InputSchema(MakeSamplingSchema()),
        std::function<CallToolResult(const ToolContext&)>(&Sampling));

    server.RegisterTool("test_elicitation",
        ToolOptions{}
            .Description("Tests server-initiated elicitation (user input request)")
            .InputSchema(MakeElicitationSchema()),
        std::function<CallToolResult(const ToolContext&)>(&Elicitation));

    server.RegisterTool("test_elicitation_sep1034_defaults",
        ToolOptions{}
            .Description("Tests elicitation with default values per SEP-1034"),
        std::function<CallToolResult(const ToolContext&)>(&ElicitationSep1034Defaults));

    server.RegisterTool("test_elicitation_sep1330_enums",
        ToolOptions{}
            .Description("Tests elicitation with enum schema improvements per SEP-1330"),
        std::function<CallToolResult(const ToolContext&)>(&ElicitationSep1330Enums));

    server.RegisterTool("test_missing_capability",
        ToolOptions{}
            .Description(
                "SEP-2575: requires the `sampling` client capability "
                "(drives the -32021 undeclared-capability rejection)"),
        std::function<CallToolResult(const ToolContext&)>(&MissingCapability));

    server.RegisterTool("test_trigger_tool_change",
        ToolOptions{}
            .Description("Listen diagnostic (SEP-2575): publishes a tools/list_changed event onto the handler bus"),
        BindTrigger(server, &TriggerToolChange));

    server.RegisterTool("test_trigger_prompt_change",
        ToolOptions{}
            .Description("Listen diagnostic (SEP-2575): publishes a prompts/list_changed event onto the handler bus"),
        BindTrigger(server, &TriggerPromptChange));
}

}
