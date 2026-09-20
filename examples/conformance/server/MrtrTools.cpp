#include "ConformanceServer.hpp"

#include <mcp/server/RequestState.hpp>

#include <mcp/McpError.hpp>

#include <functional>
#include <string>

namespace mcp::conformance {
namespace {

JsonValue MakeFormSchema(const char* property, const char* type)
{
    JsonValue properties(JsonValue::object_tag);
    JsonValue field(JsonValue::object_tag);
    field["type"] = JsonValue(type);
    properties[property] = std::move(field);
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(properties);
    schema["required"] = JsonValue(JsonValue::array_tag);
    schema["required"].PushBack(JsonValue(property));
    return schema;
}

JsonValue MakeConfirmOkSchema()
{
    JsonValue field(JsonValue::object_tag);
    field["type"] = JsonValue("boolean");
    JsonValue properties(JsonValue::object_tag);
    properties["ok"] = std::move(field);
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(properties);
    JsonValue required(JsonValue::array_tag);
    required.PushBack(JsonValue("ok"));
    schema["required"] = std::move(required);
    return schema;
}

const JsonValue* AcceptedElicitContent(
    const std::optional<JsonValue>& responses, const char* key)
{
    if (!responses) return nullptr;
    auto* entry = responses->Find(key);
    if (!entry || !entry->IsObject()) return nullptr;
    if (auto* action = entry->Find("action")) {
        if (!action->IsString() || std::string(action->GetString()) != "accept") {
            return nullptr;
        }
        auto* content = entry->Find("content");
        return (content && content->IsObject()) ? content : nullptr;
    }
    return entry;
}

const JsonValue* ResponseEntry(
    const std::optional<JsonValue>& responses, const char* key)
{
    return responses ? responses->Find(key) : nullptr;
}

const JsonValue* TrySamplingText(const JsonValue& entry, std::string& text)
{
    if (auto* content = entry.Find("content"); content && content->IsObject()) {
        if (auto* value = content->Find("text"); value && value->IsString()) {
            text = value->GetString();
            return value;
        }
    }
    return nullptr;
}

const JsonValue* RootsResponseArray(const JsonValue& entry)
{
    if (entry.IsArray()) return &entry;
    if (entry.IsObject()) {
        if (auto* roots = entry.Find("roots"); roots && roots->IsArray()) {
            return roots;
        }
    }
    return nullptr;
}

InputRequest MakeNameElicitRequest()
{
    ElicitRequestParams params;
    params.message = "What is your name?";
    params.requested_schema = MakeFormSchema("name", "string");
    return MakeInputRequestForElicitation(params);
}

InputRequest MakeGreetingSamplingRequest(const char* instruction, int64_t max_tokens)
{
    SamplingMessage message;
    message.role = "user";
    TextContent content;
    content.text = instruction;
    message.content = std::move(content);
    CreateMessageRequestParams params;
    params.messages.push_back(std::move(message));
    params.max_tokens = max_tokens;
    return MakeInputRequestForSampling(params);
}

InputRequest MakeConfirmRequest()
{
    ElicitRequestParams params;
    params.message = "Please confirm";
    params.requested_schema = MakeConfirmOkSchema();
    return MakeInputRequestForElicitation(params);
}

CallToolResult MakeInputRequired(InputRequiredResult ir)
{
    CallToolResult result;
    result.input_required = std::move(ir);
    return result;
}

CallToolResult Elicitation(const ToolContext& ctx)
{
    auto* content = AcceptedElicitContent(ctx.Params().input_responses, "name");
    if (content) {
        auto* name = content->Find("user_name");
        if (name && name->IsString())
            return MakeTextResult("Hello, " + name->GetString() + "!");
    }

    CallToolResult result;
    InputRequiredResult ir;
    ElicitRequestParams params;
    params.message = "What is your name?";
    params.requested_schema = MakeFormSchema("user_name", "string");
    ir.input_requests["name"] = MakeInputRequestForElicitation(params);
    result.input_required = std::move(ir);
    return result;
}

CallToolResult Sampling(const ToolContext& ctx)
{
    auto* entry = ResponseEntry(ctx.Params().input_responses, "message");
    if (!entry) {
        InputRequiredResult ir;
        ir.input_requests["message"] =
            MakeGreetingSamplingRequest("What is the capital of France?", 100);
        return MakeInputRequired(std::move(ir));
    }
    std::string text;
    if (!entry->IsObject() || !TrySamplingText(*entry, text)) {
        text = entry->Dump();
    }
    return MakeTextResult("Sampling response: " + text);
}

CallToolResult ListRoots(const ToolContext& ctx)
{
    auto* entry = ResponseEntry(ctx.Params().input_responses, "paths");
    auto* roots = entry ? RootsResponseArray(*entry) : nullptr;
    if (roots) {
        std::string uris;
        for (const auto& root : roots->GetArray()) {
            if (!uris.empty()) uris += ", ";
            if (auto* uri = root.Find("uri"); uri && uri->IsString()) {
                uris += uri->GetString();
            } else {
                uris += "undefined";
            }
        }
        return MakeTextResult("Client exposed " + std::to_string(roots->GetArray().size())
            + " root(s): " + uris);
    }
    InputRequiredResult ir;
    ir.input_requests["paths"] = MakeInputRequestForRoots(ListRootsRequestParams{});
    return MakeInputRequired(std::move(ir));
}

CallToolResult RequestState(const ToolContext& ctx)
{
    auto* confirmation = AcceptedElicitContent(ctx.Params().input_responses, "confirm");
    if (!confirmation) {
        JsonValue payload(JsonValue::object_tag);
        payload["tool"] = JsonValue("request_state");
        InputRequiredResult ir;
        ir.input_requests["confirm"] = MakeConfirmRequest();
        ir.request_state = payload.Dump();
        return MakeInputRequired(std::move(ir));
    }
    if (!ctx.Params().request_state) {
        throw McpError(McpErrorCode::InvalidParams,
            "Invalid requestState: missing or failed integrity verification");
    }
    return MakeTextResult("state-ok: requestState verified and confirmation received");
}

CallToolResult MultipleInputs(const ToolContext& ctx)
{
    const auto& responses = ctx.Params().input_responses;
    auto* name_content = AcceptedElicitContent(responses, "name");
    auto* name = name_content ? name_content->Find("user_name") : nullptr;
    std::string greeting;
    bool has_greeting = false;
    if (auto* entry = ResponseEntry(responses, "message"); entry && entry->IsObject()) {
        has_greeting = TrySamplingText(*entry, greeting) != nullptr;
    }
    auto* roots_entry = ResponseEntry(responses, "paths");
    auto* roots = roots_entry ? RootsResponseArray(*roots_entry) : nullptr;
    if (!name || !name->IsString() || !has_greeting || !roots) {
        InputRequiredResult ir;
        ir.input_requests["name"] = MakeNameElicitRequest();
        ir.input_requests["message"] = MakeGreetingSamplingRequest("Generate a greeting", 50);
        ir.input_requests["paths"] = MakeInputRequestForRoots(ListRootsRequestParams{});
        JsonValue payload(JsonValue::object_tag);
        payload["tool"] = JsonValue("multiple_inputs");
        ir.request_state = payload.Dump();
        return MakeInputRequired(std::move(ir));
    }
    return MakeTextResult(greeting + " " + name->GetString() + " — "
        + std::to_string(roots->GetArray().size()) + " root(s) visible");
}

CallToolResult MultiRound(const ToolContext& ctx)
{
    int round = 0;
    std::string stored_name = "unknown";
    if (ctx.Params().request_state) {
        auto payload = detail::DecodeRequestStatePayload(*ctx.Params().request_state);
        if (payload && payload->IsObject()) {
            auto* tool = payload->Find("tool");
            auto* stored_round = payload->Find("round");
            if (tool && tool->IsString() && std::string(tool->GetString()) == "multi_round"
                && stored_round && stored_round->IsInt()) {
                round = static_cast<int>(stored_round->GetInt());
            }
            auto* name = payload->Find("name");
            if (name && name->IsString()) stored_name = name->GetString();
        }
    }

    if (round == 0) {
        ElicitRequestParams params;
        params.message = "Step 1: What is your name?";
        params.requested_schema = MakeFormSchema("name", "string");
        JsonValue payload(JsonValue::object_tag);
        payload["tool"] = JsonValue("multi_round");
        payload["round"] = JsonValue(static_cast<int64_t>(1));
        InputRequiredResult ir;
        ir.input_requests["name"] = MakeInputRequestForElicitation(params);
        ir.request_state = payload.Dump();
        return MakeInputRequired(std::move(ir));
    }
    if (round == 1) {
        std::string name = "unknown";
        auto* content = AcceptedElicitContent(ctx.Params().input_responses, "name");
        if (content) {
            auto* value = content->Find("name");
            if (value && value->IsString()) name = value->GetString();
        }
        ElicitRequestParams params;
        params.message = "Step 2: What is your favorite color?";
        params.requested_schema = MakeFormSchema("color", "string");
        JsonValue payload(JsonValue::object_tag);
        payload["tool"] = JsonValue("multi_round");
        payload["round"] = JsonValue(static_cast<int64_t>(2));
        payload["name"] = JsonValue(name);
        InputRequiredResult ir;
        ir.input_requests["color"] = MakeInputRequestForElicitation(params);
        ir.request_state = payload.Dump();
        return MakeInputRequired(std::move(ir));
    }

    std::string color = "unknown";
    auto* content = AcceptedElicitContent(ctx.Params().input_responses, "color");
    if (content) {
        auto* value = content->Find("color");
        if (value && value->IsString()) color = value->GetString();
    }
    return MakeTextResult("Multi-round complete: " + stored_name + " likes " + color);
}

CallToolResult TamperedState(const ToolContext& ctx)
{
    auto* confirmation = AcceptedElicitContent(ctx.Params().input_responses, "confirm");
    if (ctx.Params().request_state && confirmation) {
        return MakeTextResult("integrity-ok: requestState verified");
    }
    InputRequiredResult ir;
    ir.input_requests["confirm"] = MakeConfirmRequest();
    JsonValue payload(JsonValue::object_tag);
    payload["tool"] = JsonValue("tampered_state");
    ir.request_state = payload.Dump();
    return MakeInputRequired(std::move(ir));
}

CallToolResult Capabilities(const ToolContext& ctx)
{
    if (ctx.Params().input_responses) {
        return MakeTextResult("Capability-aware input requests fulfilled");
    }
    std::optional<ClientCapabilities> declared;
    if (ctx.Params().meta && ctx.Params().meta->client_capabilities) {
        declared = *ctx.Params().meta->client_capabilities;
    } else if (auto caps = ctx.Server().GetClientCapabilities()) {
        declared = *caps;
    }
    InputRequiredResult ir;
    if (declared && declared->elicitation) {
        ir.input_requests["name"] = MakeNameElicitRequest();
    }
    if (declared && declared->sampling) {
        ir.input_requests["message"] =
            MakeGreetingSamplingRequest("Generate a short greeting", 50);
    }
    if (declared && declared->roots) {
        ir.input_requests["paths"] = MakeInputRequestForRoots(ListRootsRequestParams{});
    }
    if (ir.input_requests.empty()) {
        return MakeTextResult("No declared client capability supports an in-band input request");
    }
    return MakeInputRequired(std::move(ir));
}

} // namespace

void RegisterMrtrTools(McpServer& server)
{
    server.RegisterTool("test_input_required_result_elicitation",
        ToolOptions{}
            .Description("MRTR (SEP-2322): asks for the caller name via an in-band elicitation request"),
        std::function<CallToolResult(const ToolContext&)>(&Elicitation));

    server.RegisterTool("test_input_required_result_sampling",
        ToolOptions{}
            .Description("MRTR (SEP-2322): asks for an LLM completion via an in-band sampling request"),
        std::function<CallToolResult(const ToolContext&)>(&Sampling));

    server.RegisterTool("test_input_required_result_list_roots",
        ToolOptions{}
            .Description("MRTR (SEP-2322): asks for the client roots via an in-band roots/list request"),
        std::function<CallToolResult(const ToolContext&)>(&ListRoots));

    server.RegisterTool("test_input_required_result_request_state",
        ToolOptions{}
            .Description("MRTR (SEP-2322): round-trips integrity-protected requestState alongside an elicitation request"),
        std::function<CallToolResult(const ToolContext&)>(&RequestState));

    server.RegisterTool("test_input_required_result_multiple_inputs",
        ToolOptions{}
            .Description("MRTR (SEP-2322): asks for elicitation, sampling and roots input in a single round"),
        std::function<CallToolResult(const ToolContext&)>(&MultipleInputs));

    server.RegisterTool("test_input_required_result_multi_round",
        ToolOptions{}
            .Description("MRTR (SEP-2322): two elicitation rounds with evolving requestState before completing"),
        std::function<CallToolResult(const ToolContext&)>(&MultiRound));

    server.RegisterTool("test_input_required_result_tampered_state",
        ToolOptions{}
            .Description("MRTR (SEP-2322): rejects retries whose requestState fails integrity verification"),
        std::function<CallToolResult(const ToolContext&)>(&TamperedState));

    server.RegisterTool("test_input_required_result_capabilities",
        ToolOptions{}
            .Description("MRTR (SEP-2322): only requests input kinds the declared client capabilities cover"),
        std::function<CallToolResult(const ToolContext&)>(&Capabilities));
}

}
