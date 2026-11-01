#include <mcp/Capabilities.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/client/ClientOptions.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/RequestContext.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

using namespace mcp;
using ToolCtx = RequestContext<CallToolRequestParams>;

namespace collect_user_input {

// region form-schema
JsonValue MakeAddressSchema() {
    JsonValue properties(JsonValue::object_tag);
    for (const char* field : {"street", "city", "zip"}) {
        JsonValue entry(JsonValue::object_tag);
        entry["type"] = JsonValue("string");
        entry["title"] = JsonValue(field);
        properties[field] = std::move(entry);
    }
    JsonValue required(JsonValue::array_tag);
    for (const char* field : {"street", "city", "zip"}) {
        required.PushBack(JsonValue(field));
    }
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(properties);
    schema["required"] = std::move(required);
    return schema;
}
// endregion

// region read-back
enum class AnswerState { Unanswered, Accepted, Declined, Cancelled };

struct ElicitAnswer {
    AnswerState state{AnswerState::Unanswered};
    std::optional<JsonValue> content;
};

ElicitAnswer ReadElicitationAnswer(const CallToolRequestParams& params,
                                  std::string_view key) {
    if (!params.input_responses) return ElicitAnswer{};
    const JsonValue* entry = params.input_responses->Find(key);
    if (!entry || !entry->IsObject()) return ElicitAnswer{};
    const JsonValue* action = entry->Find("action");
    const std::string decision =
        (action && action->IsString()) ? action->GetString() : std::string("cancel");
    ElicitAnswer answer;
    if (decision == "accept") {
        answer.state = AnswerState::Accepted;
        const JsonValue* content = entry->Find("content");
        if (content && !content->IsNull()) answer.content = *content;
        return answer;
    }
    answer.state = (decision == "decline") ? AnswerState::Declined
                                          : AnswerState::Cancelled;
    return answer;
}

std::string DecisionName(AnswerState state) {
    switch (state) {
        case AnswerState::Accepted: return "accept";
        case AnswerState::Declined: return "decline";
        case AnswerState::Cancelled: return "cancel";
        case AnswerState::Unanswered: break;
    }
    return "cancel";
}
// endregion

// region outcome
CallToolResult ReportDecision(std::string_view decision,
                             const std::optional<JsonValue>& content) {
    CallToolResult result;
    std::string text(decision);
    if (content) text += " " + content->Dump();
    result.content.push_back(TextContent{"text", std::move(text)});
    return result;
}

CallToolResult ReportFailure(std::string_view reason) {
    CallToolResult result;
    result.is_error = true;
    result.content.push_back(TextContent{"text", std::string(reason)});
    return result;
}
// endregion

// region form-tool-handler
CallToolResult AskShippingAddress(const ToolCtx& ctx) {
    const ElicitAnswer answered =
        ReadElicitationAnswer(ctx.Params(), "elicitation");
    if (answered.state != AnswerState::Unanswered) {
        return ReportDecision(DecisionName(answered.state), answered.content);
    }

    ElicitRequestParams params;
    params.message = "Please confirm the delivery address";
    params.requested_schema = MakeAddressSchema();

    const ElicitResult result =
        ctx.Server().Elicit(params, ctx.SessionId()).get();
    return ReportDecision(result.action, result.content);
}
// endregion

// region url-tool-handler
CallToolResult AuthorizeOffSite(const ToolCtx& ctx) {
    const ElicitAnswer answered =
        ReadElicitationAnswer(ctx.Params(), "elicitation");
    if (answered.state != AnswerState::Unanswered) {
        return ReportDecision(DecisionName(answered.state), answered.content);
    }

    std::future<ElicitResult> pending = ctx.Server().ElicitUrl(
        "https://example.com/consent",
        "Finish the sign-in on the opened page",
        std::chrono::seconds{120},
        ctx.SessionId());

    try {
        const ElicitResult result = pending.get();
        return ReportDecision(result.action, result.content);
    } catch (const McpError& error) {
        if (error.Code() == McpErrorCode::RequestTimeout) {
            return ReportFailure("url elicitation timed out");
        }
        return ReportFailure(error.ErrorMessage());
    }
}
// endregion

// region server-options
ServerOptions MakeInteractiveServerOptions() {
    ServerOptions options;
    options.server_info = Implementation{"docs-server", "1.0.0"};
    ServerOptions::InputRequiredConfig config;
    config.round_timeout = std::chrono::seconds{90};
    options.input_required_config = config;
    return options;
}
// endregion

// region server-registration
std::unique_ptr<McpServer> MakeInteractiveServer(
    std::shared_ptr<ITransport> transport, const ServerOptions& options) {
    std::unique_ptr<McpServer> server = McpServer::Create(transport, options);
    server->RegisterTool("ask-shipping-address",
        ToolOptions{}.Description("Collect a delivery address from the user"),
        std::function<CallToolResult(const ToolCtx&)>(AskShippingAddress));
    server->RegisterTool("authorize-off-site",
        ToolOptions{}.Description("Send the user to a page and wait"),
        std::function<CallToolResult(const ToolCtx&)>(AuthorizeOffSite));
    return server;
}
// endregion

// region client-handlers
ClientOptions MakeInteractiveClientOptions() {
    ClientOptions options;
    options.client_info = Implementation{"docs-client", "1.0.0"};
    options.elicitation_handler = [](const ElicitRequestParams& params) -> ElicitResult {
        ElicitResult result;
        result.action = params.message.empty() ? "cancel" : "accept";
        if (result.action != "accept") return result;
        JsonValue content(JsonValue::object_tag);
        content["street"] = JsonValue("1 Main Street");
        content["city"] = JsonValue("Springfield");
        content["zip"] = JsonValue("12345");
        result.content = std::move(content);
        return result;
    };
    options.url_elicitation_handler = [](const ElicitRequestParams& params) -> ElicitResult {
        ElicitResult result;
        result.action = params.url ? "accept" : "decline";
        return result;
    };
    return options;
}
// endregion

// region end-to-end
CallToolResult CollectAddressEndToEnd() {
    InMemoryTransport::Pair pair = InMemoryTransport::CreatePair();
    std::unique_ptr<McpServer> server = MakeInteractiveServer(
        pair.server, MakeInteractiveServerOptions());
    std::thread runner([&server]() { server->Run(); });

    std::unique_ptr<McpClient> client = McpClient::Create(
        pair.client, MakeInteractiveClientOptions());
    CallToolResult result = client->CallTool(
        "ask-shipping-address", JsonValue(JsonValue::object_tag));

    client->Close();
    server->Close();
    runner.join();
    return result;
}
// endregion

} // namespace collect_user_input
