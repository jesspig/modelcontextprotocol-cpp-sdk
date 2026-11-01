#include <mcp/Content.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/Log.hpp>
#include <mcp/client/ClientOptions.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/RequestContext.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace mcp_docs {
using namespace mcp;
namespace {

constexpr char kRequestLogLevelKey[] = "io.modelcontextprotocol/logLevel";

CallToolResult TextResult(const std::string& text) {
    CallToolResult result;
    result.content.push_back(TextContent{"text", text});
    return result;
}

} // namespace

// region log-capability
ServerOptions BuildLoggingServerOptions() {
    ServerOptions options;
    options.server_info = Implementation{"logging-server", "1.0.0"};
    options.declare_logging = true;
    return options;
}
// endregion

// region log-in-handler
using LogCtx = RequestContext<CallToolRequestParams>;

CallToolResult RebuildIndex(const LogCtx& ctx) {
    ctx.Log(LoggingLevel::Info, "rebuild started");

    int64_t empty_shards = 0;
    for (int64_t shard = 0; shard < 4; ++shard) {
        ctx.Log(LoggingLevel::Debug, "indexed shard " + std::to_string(shard));
        if (shard > 1) {
            ++empty_shards;
        }
    }

    if (empty_shards > 0) {
        ctx.Log(LoggingLevel::Warning,
            std::to_string(empty_shards) + " shards were empty");
    }
    return TextResult("index rebuilt");
}
// endregion

// region log-outside-handler
void EmitServerLogs(McpServer& server) {
    server.SendLoggingMessage(LoggingLevel::Info, "cache warmed");
    server.SendLoggingMessage(LoggingLevel::Warning, "slow upstream", LoggingLevel::Warning);
}
// endregion

// region log-client-receive
void ReceiveLogs(McpClient& client) {
    client.SetLoggingHandler([](const LoggingMessageNotificationParams& message) {
        std::cout << "logger=" << (message.logger ? *message.logger : "<none>")
                  << " data=" << message.data.Dump() << std::endl;
    });
}

void RequestLegacyLevel(McpClient& client) {
    client.SetLoggingLevel(LoggingLevel::Warning);
}

CallToolResult CallWithRequestLogLevel(McpClient& client) {
    JsonValue meta(JsonValue::object_tag);
    meta[kRequestLogLevelKey] = JsonValue("debug");

    RequestOptions options;
    options.meta = std::move(meta);
    return client.CallTool("rebuild_index", std::nullopt, options);
}
// endregion

// region log-vs-internal-log
void EnableInternalSdkLogs() {
    mcp::SetLogLevel(mcp::LogLevel::Debug);
    mcp::SetLogHandler([](const mcp::LogRecord& record) {
        std::cerr << "[sdk] " << record.message << std::endl;
    });
}
// endregion

// region full
void RunLoggingFlow() {
    auto pair = InMemoryTransport::CreatePair();

    auto server = McpServer::Create(pair.server, BuildLoggingServerOptions());
    server->RegisterTool(
        "rebuild_index",
        ToolOptions().Description("Rebuild the search index"),
        std::function<CallToolResult(const LogCtx&)>(&RebuildIndex));
    std::thread worker([&server] { server->Run(); });

    ClientOptions client_options;
    client_options.client_info = Implementation{"logging-client", "1.0.0"};
    auto client = McpClient::Create(pair.client, client_options);

    ReceiveLogs(*client);
    CallWithRequestLogLevel(*client);
    EmitServerLogs(*server);

    client->Close();
    server->Close();
    worker.join();
}
// endregion

} // namespace mcp_docs
