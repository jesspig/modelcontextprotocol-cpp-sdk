#include <mcp/Capabilities.hpp>
#include <mcp/Content.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/Transport.hpp>
#include <mcp/client/ClientOptions.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/client/RequestHandle.hpp>
#include <mcp/protocol/IncomingRequestMeta.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace mcp;

namespace handle_server_initiated_requests {

// region form-handler
ElicitResult AskForConfirmation(const ElicitRequestParams& params) {
    ElicitResult result;
    if (!params.requested_schema) {
        result.action = "cancel";
        return result;
    }

    result.action = "accept";
    JsonValue content(JsonValue::object_tag);
    content["approved"] = JsonValue(true);
    result.content = std::move(content);
    return result;
}

ClientOptions WithFormHandler() {
    ClientOptions options;
    options.elicitation_handler = &AskForConfirmation;
    return options;
}
// endregion

// region url-handler
ElicitResult AcceptExternalSignIn(const ElicitRequestParams& params) {
    ElicitResult result;
    result.action = params.url ? "accept" : "decline";
    return result;
}

ClientOptions WithUrlHandler() {
    ClientOptions options;
    options.url_elicitation_handler = &AcceptExternalSignIn;
    return options;
}

void AttachUrlHandlerAfterCreate(McpClient& client) {
    client.SetUrlElicitationHandler(&AcceptExternalSignIn);
}
// endregion

// region capability-derivation
bool DeclaresBothElicitationModes(const ClientOptions& options) {
    std::optional<ClientCapabilities> caps = DeriveClientCapabilities(options);
    return caps.has_value() && caps->elicitation.has_value() &&
        caps->elicitation->form.has_value() && caps->elicitation->url.has_value();
}
// endregion

// region sampling-roots
CreateMessageResult AnswerLocally(const CreateMessageRequestParams& params) {
    CreateMessageResult result;
    result.role = "assistant";
    result.model = "in-process-rule-engine";
    result.content = TextContent{"text",
        "max_tokens=" + std::to_string(params.max_tokens)};
    return result;
}

ListRootsResult PublishWorkspaceRoots(const ListRootsRequestParams&) {
    ListRootsResult result;
    result.roots.push_back(Root{"file:///workspace/src", "sources"});
    return result;
}

ClientOptions WithSamplingAndRoots() {
    ClientOptions options;
    options.sampling_handler = &AnswerLocally;
    options.roots_handler = &PublishWorkspaceRoots;
    return options;
}
// endregion

// region manual-round
JsonValue ResumeRound(
    const std::string& tool_name,
    const InputRequiredResult& pending,
    const ElicitResult& answer) {
    JsonValue arguments(JsonValue::object_tag);
    arguments["path"] = JsonValue("reports/2026-11.csv");

    JsonValue params(JsonValue::object_tag);
    params["name"] = JsonValue(tool_name);
    params["arguments"] = std::move(arguments);

    JsonValue responses(JsonValue::object_tag);
    for (const auto& entry : pending.input_requests) {
        responses[entry.first] = MakeInputResponseFromElicitResult(answer);
    }
    params["inputResponses"] = std::move(responses);
    if (pending.request_state) {
        params["requestState"] = JsonValue(*pending.request_state);
    }
    return params;
}

std::string RunManualRound(McpClient& client, const CallToolResult& first) {
    if (!first.input_required) {
        return "no further input needed";
    }

    ElicitResult answer;
    answer.action = "accept";
    JsonValue content(JsonValue::object_tag);
    content["approved"] = JsonValue(true);
    answer.content = std::move(content);

    JsonValue params = ResumeRound("export_rows", *first.input_required, answer);

    RequestOptions options;
    options.timeout = std::chrono::milliseconds(60000);
    RequestHandle handle = client.SendRequest("tools/call", std::move(params), options);

    const RequestOutcome outcome = handle.future.get();
    if (const auto* value = std::get_if<JsonValue>(&outcome)) {
        return value->Dump();
    }
    return std::get<ErrorData>(outcome).message;
}
// endregion

// region passive-notifications
void AttachNotificationHandlers(McpClient& client) {
    client.SetNotificationHandler("notifications/elicitation/complete",
        [](const JsonRpcNotification& note) {
            const std::string method = note.method;
            (void)method;
        });

    client.SetLoggingHandler([](const LoggingMessageNotificationParams& entry) {
        const std::string payload = entry.data.Dump();
        (void)payload;
    });
}
// endregion

// region disable-auto-fulfil
ClientOptions WithoutAutoFulfilment() {
    ClientOptions options;
    ClientOptions::InputRequiredConfig config;
    config.auto_fulfill = false;
    options.input_required_config = config;
    return options;
}
// endregion

// region full
std::unique_ptr<McpClient> BuildPassiveClient(std::shared_ptr<ITransport> session) {
    ClientOptions options;
    options.client_info = Implementation{"release-bot", "1.4.0"};
    options.elicitation_handler = &AskForConfirmation;
    options.url_elicitation_handler = &AcceptExternalSignIn;
    options.connect_mode = ConnectMode::Auto;

    auto client = McpClient::Create(std::move(session), options);
    client->SetLoggingHandler([](const LoggingMessageNotificationParams& entry) {
        (void)entry.level;
    });
    return client;
}

InMemoryTransport::Pair OpenPassivePair() {
    return InMemoryTransport::CreatePair();
}
// endregion

} // namespace handle_server_initiated_requests
