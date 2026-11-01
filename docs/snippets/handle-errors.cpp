#include <mcp/Content.hpp>
#include <mcp/ErrorCodes.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/Transport.hpp>
#include <mcp/client/ClientOptions.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/client/RequestHandle.hpp>
#include <mcp/protocol/IncomingRequestMeta.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/RequestContext.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace mcp;

namespace handle_errors {

using Ctx = RequestContext<CallToolRequestParams>;

enum class FailureKind : int {
    Success,
    Cancelled,
    TimedOut,
    SessionGone,
    VersionRejected,
    Protocol,
    Transport,
};

// region classify-outcome
FailureKind Classify(const RequestOutcome& outcome) {
    if (std::holds_alternative<JsonValue>(outcome)) {
        return FailureKind::Success;
    }
    const ErrorData& error = std::get<ErrorData>(outcome);
    switch (error.code) {
        case McpErrorCode::RequestCancelled: return FailureKind::Cancelled;
        case McpErrorCode::RequestTimeout: return FailureKind::TimedOut;
        case McpErrorCode::SessionExpired: return FailureKind::SessionGone;
        case McpErrorCode::UnsupportedProtocolVersion: return FailureKind::VersionRejected;
        case McpErrorCode::ConnectionClosed:
        case McpErrorCode::ConnectionRefused:
        case McpErrorCode::TlsHandshakeFailed: return FailureKind::Transport;
        default: return FailureKind::Protocol;
    }
}

std::optional<std::string> DataReason(const RequestOutcome& outcome) {
    const auto* error = std::get_if<ErrorData>(&outcome);
    if (!error || !error->data) return std::nullopt;
    auto* reason = error->data->Find("reason");
    if (!reason || !reason->IsString()) return std::nullopt;
    return reason->GetString();
}
// endregion

// region version-rejection
std::vector<std::string> SupportedVersionsOf(const McpError& error) {
    std::vector<std::string> versions;
    if (error.Code() != McpErrorCode::UnsupportedProtocolVersion || !error.Data()) {
        return versions;
    }
    auto* supported = error.Data()->Find("supported");
    if (!supported || !supported->IsArray()) return versions;
    for (const JsonValue& entry : supported->GetArray()) {
        if (entry.IsString()) versions.push_back(entry.GetString());
    }
    return versions;
}
// endregion

// region retry-policy
CallToolResult CallWithRecovery(McpClient& client, const std::string& tool_name) {
    RequestOptions options;
    options.timeout = std::chrono::milliseconds(5000);
    try {
        return client.CallTool(tool_name, JsonValue(JsonValue::object_tag), options);
    } catch (const McpError& error) {
        if (error.Code() == McpErrorCode::SessionExpired) {
            return client.CallTool(tool_name, JsonValue(JsonValue::object_tag), options);
        }
        if (error.Code() == McpErrorCode::RequestTimeout) {
            throw;
        }
        CallToolResult result;
        result.is_error = true;
        result.content.push_back(TextContent{"text", std::string(error.what())});
        return result;
    }
}
// endregion

// region raw-channel
bool RawValueCarriesError(const JsonValue& value) {
    auto* code = value.Find("code");
    return code != nullptr && code->IsInt() && code->GetInt() < 0;
}

std::string RawOrThrow(McpClient& client) {
    JsonValue value = client.SendRequestRaw("tools/call", JsonValue(JsonValue::object_tag)).get();
    if (RawValueCarriesError(value)) {
        return std::string("peer error: ") + value.Dump();
    }
    return value.Dump();
}
// endregion

// region tool-error-surface
CallToolResult Rejected(const Ctx& ctx) {
    const std::optional<JsonValue> arguments = ctx.Params().arguments;
    if (!arguments) {
        CallToolResult result;
        result.is_error = true;
        result.content.push_back(TextContent{"text", "arguments is required"});
        return result;
    }
    if (!arguments->Find("path")) {
        throw McpError(McpErrorCode::InvalidParams, "path is missing");
    }
    throw std::runtime_error("disk full");
}
// endregion

// region registration-errors
bool RegisterGuarded(McpServer& server, const std::string& tool_name) {
    try {
        server.RegisterTool(tool_name, ToolOptions{}, &Rejected);
        return true;
    } catch (const McpError& error) {
        return error.Code() != McpErrorCode::InvalidParams;
    }
}
// endregion

// region full
std::string RunErrorProbe() {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(pair.server);
    server->RegisterTool("guarded", ToolOptions{}, &Rejected);

    auto client = McpClient::Create(pair.client);

    RequestOptions options;
    options.timeout = std::chrono::milliseconds(120);
    options.cancel_on_timeout = false;

    RequestHandle handle = client->SendRequest(
        "tools/call", JsonValue(JsonValue::object_tag), options);
    const RequestOutcome outcome = handle.future.get();

    const std::string summary = std::to_string(static_cast<int>(Classify(outcome)));
    client->Close();
    server->Close();
    return summary;
}
// endregion

} // namespace handle_errors
