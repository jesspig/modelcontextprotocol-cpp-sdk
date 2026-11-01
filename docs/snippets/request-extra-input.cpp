#include <mcp/Capabilities.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>
#include <mcp/Methods.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/client/ClientOptions.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/RequestContext.hpp>
#include <mcp/server/RequestState.hpp>
#include <mcp/server/ServerOptions.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace mcp;
using ToolCtx = RequestContext<CallToolRequestParams>;

namespace request_extra_input {

// region form-schema
JsonValue MakeSingleFieldSchema(std::string_view property, std::string_view type) {
    JsonValue field(JsonValue::object_tag);
    field["type"] = JsonValue(std::string(type));
    JsonValue properties(JsonValue::object_tag);
    properties[property] = std::move(field);
    JsonValue required(JsonValue::array_tag);
    required.PushBack(JsonValue(std::string(property)));
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(properties);
    schema["required"] = std::move(required);
    return schema;
}
// endregion

// region input-request
InputRequest MakeNameRequest() {
    ElicitRequestParams params;
    params.message = "What is your name?";
    params.requested_schema = MakeSingleFieldSchema("name", "string");
    return MakeInputRequestForElicitation(params);
}

InputRequest MakeColorRequest() {
    ElicitRequestParams params;
    params.message = "Which colour do you prefer?";
    params.requested_schema = MakeSingleFieldSchema("color", "string");
    return MakeInputRequestForElicitation(params);
}
// endregion

// region read-back
const JsonValue* AcceptedContent(const std::optional<JsonValue>& responses,
                                std::string_view key) {
    if (!responses) return nullptr;
    const JsonValue* entry = responses->Find(key);
    if (!entry || !entry->IsObject()) return nullptr;
    const JsonValue* action = entry->Find("action");
    if (!action || !action->IsString() || action->GetString() != "accept") return nullptr;
    const JsonValue* content = entry->Find("content");
    return (content && content->IsObject()) ? content : nullptr;
}

std::string StringField(const JsonValue* object, std::string_view key,
                        std::string fallback) {
    if (!object) return fallback;
    const JsonValue* value = object->Find(key);
    return (value && value->IsString()) ? value->GetString() : std::move(fallback);
}
// endregion

// region state-payload
int RoundOf(const std::optional<std::string>& request_state) {
    if (!request_state) return 0;
    const std::optional<JsonValue> payload =
        detail::DecodeRequestStatePayload(*request_state);
    if (!payload || !payload->IsObject()) return 0;
    if (const JsonValue* tool = payload->Find("tool");
        !tool || !tool->IsString() || tool->GetString() != "profile") {
        return 0;
    }
    const JsonValue* stored = payload->Find("round");
    return (stored && stored->IsInt()) ? static_cast<int>(stored->GetInt()) : 0;
}

std::string StatePayload(int round, std::string_view name) {
    JsonValue payload(JsonValue::object_tag);
    payload["tool"] = JsonValue("profile");
    payload["round"] = JsonValue(static_cast<int64_t>(round));
    if (!name.empty()) payload["name"] = JsonValue(std::string(name));
    return payload.Dump();
}
// endregion

// region input-required-result
CallToolResult AskColour(int round, std::string_view known_name) {
    CallToolResult result;
    InputRequiredResult input_required;
    input_required.input_requests["color"] = MakeColorRequest();
    input_required.request_state = StatePayload(round + 1, known_name);
    result.input_required = std::move(input_required);
    return result;
}

CallToolResult AskName() {
    CallToolResult result;
    InputRequiredResult input_required;
    input_required.input_requests["name"] = MakeNameRequest();
    input_required.request_state = StatePayload(1, std::string_view());
    result.input_required = std::move(input_required);
    return result;
}
// endregion

// region full-tool
CallToolResult BuildProfile(const ToolCtx& ctx) {
    const int round = RoundOf(ctx.Params().request_state);
    const JsonValue* name_answer = AcceptedContent(ctx.Params().input_responses, "name");
    const JsonValue* colour_answer = AcceptedContent(ctx.Params().input_responses, "color");

    if (round == 0) return AskName();
    if (round == 1) {
        if (!name_answer) return AskName();
        return AskColour(round, StringField(name_answer, "name", "anonymous"));
    }
    if (!name_answer) return AskName();
    if (!colour_answer) return AskColour(round, StringField(name_answer, "name", "x"));

    CallToolResult result;
    const std::string stored = StringField(name_answer, "name", "anonymous");
    const std::string colour = StringField(colour_answer, "color", "none");
    result.content.push_back(
        TextContent{"text", "profile built for " + stored + " who likes " + colour});
    return result;
}
// endregion

// region server-signing
ServerOptions MakeSigningServerOptions() {
    ServerOptions options;
    options.server_info = Implementation{"docs-server", "1.0.0"};
    options.request_state_key = std::string("change-me-with-an-env-secret");
    options.request_state_ttl = std::chrono::seconds{300};
    ServerOptions::InputRequiredConfig config;
    config.max_rounds = 5;
    config.round_timeout = std::chrono::seconds{120};
    options.input_required_config = config;
    return options;
}
// endregion

// region custom-verifier
ServerOptions MakeVerifyingServerOptions() {
    ServerOptions options = MakeSigningServerOptions();
    options.request_state_key.reset();
    options.request_state_verifier = [](std::string_view state) -> bool {
        const std::optional<JsonValue> payload = detail::DecodeRequestStatePayload(state);
        if (!payload || !payload->IsObject()) return false;
        const JsonValue* tool = payload->Find("tool");
        return tool && tool->IsString() && tool->GetString() == "profile";
    };
    return options;
}
// endregion

// region tool-registration
std::unique_ptr<McpServer> MakeProfileServer(std::shared_ptr<ITransport> transport) {
    std::unique_ptr<McpServer> server =
        McpServer::Create(transport, MakeSigningServerOptions());
    server->RegisterTool("profile",
        ToolOptions{}.Description("Ask the user for a name and a colour"),
        std::function<CallToolResult(const ToolCtx&)>(BuildProfile));
    return server;
}
// endregion

// region client-auto
ClientOptions MakeAutoClientOptions() {
    ClientOptions options;
    options.client_info = Implementation{"docs-client", "1.0.0"};
    ClientOptions::InputRequiredConfig config;
    config.auto_fulfill = true;
    config.max_rounds = 5;
    config.round_timeout = std::chrono::seconds{120};
    options.input_required_config = config;
    options.elicitation_handler = [](const ElicitRequestParams& params) -> ElicitResult {
        ElicitResult result;
        result.action = "accept";
        const bool wants_colour = params.message.find("colour") != std::string::npos;
        JsonValue content(JsonValue::object_tag);
        if (wants_colour) {
            content["color"] = JsonValue("teal");
        } else {
            content["name"] = JsonValue("Ada");
        }
        result.content = std::move(content);
        return result;
    };
    return options;
}
// endregion

// region manual-round
CallToolResult RunProfileAuto(std::shared_ptr<ITransport> transport) {
    std::unique_ptr<McpClient> client =
        McpClient::Create(transport, MakeAutoClientOptions());
    CallToolResult result =
        client->CallTool("profile", JsonValue(JsonValue::object_tag));
    client->Close();
    return result;
}

JsonValue FulfilWithoutClientLoop(std::shared_ptr<ITransport> transport,
                                 const CallToolResult& pending,
                                 const ElicitationHandler& handler) {
    if (!pending.input_required) return JsonValue(JsonValue::object_tag);

    JsonValue responses(JsonValue::object_tag);
    for (const auto& entry : pending.input_required->input_requests) {
        const std::string& key = entry.first;
        const InputRequest& request = entry.second;
        if (request.method != std::string(methods::kElicit)) continue;
        const ElicitRequestParams params =
            DeserializeElicitRequestParams(request.params);
        responses[key] = MakeInputResponseFromElicitResult(handler(params));
    }

    CallToolRequestParams retry;
    retry.name = "profile";
    retry.arguments = JsonValue(JsonValue::object_tag);
    retry.input_responses = std::move(responses);
    retry.request_state = pending.input_required->request_state;

    std::unique_ptr<McpClient> client = McpClient::Create(transport, ClientOptions{});
    std::future<JsonValue> future = client->SendRequest(
        methods::kCallTool, SerializeCallToolRequestParams(retry));
    JsonValue raw = future.get();
    client->Close();
    return raw;
}
// endregion

} // namespace request_extra_input
