#include <mcp/Content.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/client/ClientOptions.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/RequestContext.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace mcp_docs {
using namespace mcp;
namespace {

constexpr char kProgressTokenKey[] = "progressToken";

CallToolResult TextResult(const std::string& text) {
    CallToolResult result;
    result.content.push_back(TextContent{"text", text});
    return result;
}

} // namespace

// region progress-server
using ProgressCtx = RequestContext<CallToolRequestParams>;

ToolOptions ImportRowsOptions() {
    JsonValue rows(JsonValue::object_tag);
    rows["type"] = JsonValue("integer");

    JsonValue properties(JsonValue::object_tag);
    properties["rows"] = std::move(rows);

    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(properties);

    ToolOptions options;
    options.Description("Import rows and report progress").InputSchema(std::move(schema));
    return options;
}

CallToolResult ImportRows(const ProgressCtx& ctx) {
    int64_t rows = 4;
    if (ctx.Params().arguments) {
        if (auto* value = ctx.Params().arguments->Find("rows");
            value && value->IsInt()) {
            rows = value->GetInt();
        }
    }

    std::optional<ProgressToken> token;
    if (ctx.Params().meta && ctx.Params().meta->progress_token) {
        token = ctx.Params().meta->progress_token;
    }

    for (int64_t row = 1; row <= rows; ++row) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!token) {
            continue;
        }
        ctx.Server().SendProgress(
            *token,
            static_cast<double>(row),
            static_cast<double>(rows),
            "imported " + std::to_string(row) + " of " + std::to_string(rows));
    }

    if (!token) {
        return TextResult("imported without progress reporting");
    }
    return TextResult("imported " + std::to_string(rows) + " rows");
}
// endregion

// region progress-client
std::string DescribeProgress(const ProgressNotificationParams& progress) {
    const std::string total =
        progress.total ? " / " + std::to_string(static_cast<int64_t>(*progress.total)) : "";
    const std::string message = progress.message ? " " + *progress.message : "";
    return std::to_string(static_cast<int64_t>(progress.progress)) + total + message;
}

CallToolResult ImportWithProgress(McpClient& client) {
    RequestOptions options;
    options.on_progress = [](const ProgressNotificationParams& progress) {
        std::cout << "progress: " << DescribeProgress(progress) << std::endl;
    };

    JsonValue arguments(JsonValue::object_tag);
    arguments["rows"] = JsonValue(4);
    return client.CallTool("import_rows", arguments, options);
}

CallToolResult ImportWithExplicitToken(McpClient& client) {
    JsonValue meta(JsonValue::object_tag);
    meta[kProgressTokenKey] = JsonValue("import-2026-11");

    RequestOptions options;
    options.meta = std::move(meta);
    options.on_progress = [](const ProgressNotificationParams& progress) {
        std::cout << "progress: " << DescribeProgress(progress) << std::endl;
    };

    JsonValue arguments(JsonValue::object_tag);
    arguments["rows"] = JsonValue(4);
    return client.CallTool("import_rows", arguments, options);
}
// endregion

// region progress-total-less
void ReportUnboundedProgress(McpServer& server, const ProgressToken& token) {
    server.SendProgress(token, 1.0);
    server.SendProgress(token, 2.0, std::nullopt, "second chunk decoded");
}
// endregion

// region full
void RunProgressFlow() {
    auto pair = InMemoryTransport::CreatePair();

    ServerOptions server_options;
    server_options.server_info = Implementation{"progress-server", "1.0.0"};
    auto server = McpServer::Create(pair.server, server_options);
    server->RegisterTool(
        "import_rows",
        ImportRowsOptions(),
        std::function<CallToolResult(const ProgressCtx&)>(&ImportRows));
    std::thread worker([&server] { server->Run(); });

    ClientOptions client_options;
    client_options.client_info = Implementation{"progress-client", "1.0.0"};
    auto client = McpClient::Create(pair.client, client_options);

    CallToolResult reported = ImportWithProgress(*client);
    CallToolResult tokened = ImportWithExplicitToken(*client);
    std::cout << "reported parts=" << reported.content.size()
              << " tokened parts=" << tokened.content.size() << std::endl;

    client->Close();
    server->Close();
    worker.join();
}
// endregion

} // namespace mcp_docs
