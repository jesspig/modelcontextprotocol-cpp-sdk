#include <mcp/Content.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/http/EventStore.hpp>
#include <mcp/http/SessionStore.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/RequestContext.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace mcp;

// region stateful
void stateful_server() {
    StreamableHttpServerOptions http_options;
    http_options.port = 3001;
    http_options.endpoint = "/mcp";
    http_options.stateless = false;
    http_options.session_store = std::make_shared<InMemorySessionStore>();
    http_options.allowed_hosts = {"localhost:3001"};
    http_options.allowed_origins = {"http://localhost:5173"};

    auto transport = std::make_shared<StreamableHttpServerTransport>(http_options);

    ServerOptions server_options;
    server_options.server_info = Implementation{"my-http-server", "1.0.0"};

    auto server = McpServer::Create(transport, server_options);
    server->RegisterTool(
        "ping",
        ToolOptions{}.Description("ping"),
        [](const RequestContext<CallToolRequestParams>&) -> CallToolResult {
            CallToolResult result;
            result.content.push_back(TextContent{"text", "pong"});
            return result;
        });

    server->Run();
}
// endregion

// region stateless-hold-open
void stateless_hold_open(StreamableHttpServerOptions& http_options) {
    http_options.stateless = true;
    http_options.listen_hold_open = true;
}
// endregion

// region stateless
void stateless_server_options() {
    StreamableHttpServerOptions http_options;
    http_options.stateless = true;
    http_options.listen_hold_open = true;
    http_options.enable_legacy_sse = true;
    http_options.event_store = std::make_shared<EventStore>();
}
// endregion
