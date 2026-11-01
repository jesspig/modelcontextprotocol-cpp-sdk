#include <mcp/CancelToken.hpp>
#include <mcp/Content.hpp>
#include <mcp/ErrorCodes.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/Transport.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/client/RequestHandle.hpp>
#include <mcp/protocol/IncomingRequestMeta.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/RequestContext.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace mcp;

namespace cancel_a_request {

using Ctx = RequestContext<CallToolRequestParams>;

JsonValue ToolCallParams(const std::string& tool_name) {
    JsonValue arguments(JsonValue::object_tag);
    arguments["path"] = JsonValue("reports/2026-11.csv");

    JsonValue params(JsonValue::object_tag);
    params["name"] = JsonValue(tool_name);
    params["arguments"] = std::move(arguments);
    return params;
}

// region typed-handle
std::string OutcomeSummary(const RequestOutcome& outcome) {
    if (const auto* value = std::get_if<JsonValue>(&outcome)) {
        return value->Dump();
    }
    const ErrorData& error = std::get<ErrorData>(outcome);
    return std::to_string(static_cast<int32_t>(error.code)) + ": " + error.message;
}

std::string CancelWhileRunning(McpClient& client) {
    RequestOptions options;
    options.timeout = std::chrono::milliseconds(60000);

    RequestHandle handle = client.SendRequest(
        "tools/call", ToolCallParams("export_rows"), options);

    handle.Cancel("operator pressed stop");
    return OutcomeSummary(handle.future.get());
}
// endregion

// region timeout-only
std::string TimeoutWithoutWireCancel(McpClient& client) {
    RequestOptions options;
    options.timeout = std::chrono::milliseconds(200);
    options.cancel_on_timeout = false;

    RequestHandle handle = client.SendRequest(
        "tools/call", ToolCallParams("export_rows"), options);
    return OutcomeSummary(handle.future.get());
}
// endregion

// region cancel-token-fanout
std::vector<std::string> CancelWholeBatch(McpClient& client) {
    CancelToken token;

    RequestOptions options;
    options.cancel_token = token;
    options.timeout = std::chrono::milliseconds(60000);

    RequestHandle export_handle = client.SendRequest(
        "tools/call", ToolCallParams("export_rows"), options);
    RequestHandle render_handle = client.SendRequest(
        "tools/call", ToolCallParams("render_chart"), options);

    token.Cancel("user left the page");

    std::vector<std::string> summaries;
    summaries.push_back(OutcomeSummary(export_handle.future.get()));
    summaries.push_back(OutcomeSummary(render_handle.future.get()));
    summaries.push_back(token.Reason());
    return summaries;
}
// endregion

// region cancel-by-id
void CancelKnownRequest(McpClient& client, const RequestId& id) {
    client.CancelRequest(id, "superseded by a newer request");
    client.CancelRequest(id, "second call is a no-op");
}
// endregion

// region three-channels
bool FailureLooksLikeException(McpClient& client) {
    RequestOptions options;
    options.timeout = std::chrono::milliseconds(150);
    options.cancel_on_timeout = false;

    try {
        (void)client.CallTool("export_rows", JsonValue(JsonValue::object_tag), options);
        return false;
    } catch (const McpError& error) {
        return error.Code() == McpErrorCode::RequestTimeout;
    }
}

bool FailureLooksLikeErrorObject(McpClient& client) {
    JsonValue body = client.SendRequestRaw("tools/call", ToolCallParams("export_rows")).get();
    return body.Contains("code") && body["code"].GetInt() < 0;
}

bool FailureLooksLikeOutcome(McpClient& client) {
    RequestOptions options;
    options.timeout = std::chrono::milliseconds(150);

    RequestHandle handle = client.SendRequest(
        "tools/call", ToolCallParams("export_rows"), options);
    handle.Cancel("abandoned");

    const RequestOutcome outcome = handle.future.get();
    const auto* error = std::get_if<ErrorData>(&outcome);
    return error != nullptr && error->code == McpErrorCode::RequestCancelled;
}
// endregion

// region server-side-poll
CallToolResult ExportRows(const Ctx& ctx) {
    CallToolResult result;
    for (int64_t page = 0; page < 500; ++page) {
        if (ctx.IsCancellationRequested()) {
            result.is_error = true;
            result.content.push_back(
                TextContent{"text", "stopped after page " + std::to_string(page)});
            return result;
        }
    }
    result.content.push_back(TextContent{"text", "exported 500 pages"});
    return result;
}
// endregion

// region full
std::string RunCancellationDemo() {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(pair.server);
    server->RegisterTool("export_rows", ToolOptions{}, &ExportRows);

    auto client = McpClient::Create(pair.client);

    RequestOptions options;
    options.timeout = std::chrono::milliseconds(60000);
    RequestHandle handle = client->SendRequest(
        "tools/call", ToolCallParams("export_rows"), options);

    handle.Cancel("shutting down");
    const std::string summary = OutcomeSummary(handle.future.get());

    client->Close();
    server->Close();
    return summary;
}
// endregion

} // namespace cancel_a_request
