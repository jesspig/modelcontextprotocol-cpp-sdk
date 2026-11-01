#include <mcp/Capabilities.hpp>
#include <mcp/ErrorCodes.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/ProtocolVersion.hpp>
#include <mcp/Transport.hpp>
#include <mcp/client/ClientOptions.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/transport/HttpTransportMode.hpp>
#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/transport/StdioClientTransport.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace mcp;

namespace connect_to_a_server {

// region stdio-transport
StdioClientTransportOptions StdioOptions() {
    StdioClientTransportOptions options;
    options.command = "mcp-echo-server";
    options.arguments = {"--stdio"};
    options.working_directory = "build/debug/bin";
    options.inherit_environment_variables = false;
    options.environment_variables = {{"MCP_LOG_LEVEL", "2"}};
    options.stderr_path = "echo-server.stderr.log";
    options.shutdown_timeout = std::chrono::milliseconds(2000);
    options.terminate_process_tree = true;
    return options;
}

std::shared_ptr<ITransport> OpenStdioSession(StdioClientTransport& transport) {
    std::shared_ptr<ITransport> session = transport.Connect();
    return session;
}

bool HasLiveChildProcess(const StdioClientTransport& transport) {
    std::optional<int64_t> pid = transport.Pid();
    return pid.has_value() && *pid > 0;
}
// endregion

// region http-transport
std::shared_ptr<ITransport> OpenHttpSession(const std::string& access_token) {
    HttpClientTransportOptions options;
    options.endpoint = "https://mcp.example.internal/mcp";
    options.transport_mode = HttpTransportMode::AutoDetect;
    options.additional_headers = {{"authorization", "Bearer " + access_token}};
    options.enable_listen_stream = true;

    StreamableHttpClientTransport transport(options);
    return transport.Connect();
}
// endregion

// region in-memory-pair
InMemoryTransport::Pair MakeInMemoryPair() {
    return InMemoryTransport::CreatePair();
}
// endregion

// region client-options
ElicitResult AnswerWithUnknown(const ElicitRequestParams&) {
    ElicitResult result;
    result.action = "accept";
    result.content = JsonValue(JsonValue::object_tag);
    (*result.content)["answer"] = JsonValue("unknown");
    return result;
}

ClientOptions BuildClientOptions() {
    ClientOptions options;
    options.client_info = Implementation{"release-bot", "1.4.0"};
    options.connect_mode = ConnectMode::Auto;
    options.discover_probe_timeout = std::chrono::seconds(3);
    options.initialization_timeout = std::chrono::seconds(20);
    options.elicitation_handler = &AnswerWithUnknown;
    options.enable_etag_cache = true;
    options.on_transport_close = []() {};
    options.on_transport_error = [](std::string_view) {};
    return options;
}
// endregion

// region open-client
std::unique_ptr<McpClient> OpenClient(
    std::shared_ptr<ITransport> session, const ClientOptions& options) {
    return McpClient::Create(std::move(session), options);
}
// endregion

// region negotiate-failure
bool ConnectOrReportRetryable(std::shared_ptr<ITransport> session) {
    try {
        std::unique_ptr<McpClient> client = McpClient::Create(std::move(session));
        return client != nullptr;
    } catch (const McpError& error) {
        return error.Code() == McpErrorCode::RequestTimeout ||
            error.Code() == McpErrorCode::ConnectionClosed;
    }
}
// endregion

// region negotiated-snapshot
struct SessionSnapshot {
    std::string protocol_version;
    bool modern;
    bool serves_tools;
    bool serves_resources;
    bool serves_prompts;
    bool accepts_updates;
    std::string server_name;
    std::optional<std::string> instructions;
};

SessionSnapshot ReadSnapshot(const McpClient& client) {
    const ServerCapabilities& capabilities = client.GetServerCapabilities();
    SessionSnapshot snapshot;
    snapshot.protocol_version = std::string(client.GetNegotiatedProtocolVersion());
    snapshot.modern = client.IsModernProtocol();
    snapshot.serves_tools = capabilities.tools.has_value();
    snapshot.serves_resources = capabilities.resources.has_value();
    snapshot.serves_prompts = capabilities.prompts.has_value();
    snapshot.accepts_updates = capabilities.resources.has_value() &&
        capabilities.resources->subscribe.has_value() &&
        *capabilities.resources->subscribe;
    snapshot.server_name = client.GetServerInfo().name;
    snapshot.instructions = client.GetInstructions();
    return snapshot;
}

bool IsModernNegotiation(const McpClient& client) {
    return IsModernProtocolVersion(client.GetNegotiatedProtocolVersion());
}
// endregion

// region passive-handlers
void AttachLoggingHandler(McpClient& client) {
    client.SetLoggingHandler([](const LoggingMessageNotificationParams& entry) {
        const std::string payload = entry.data.Dump();
        (void)payload;
    });
}
// endregion

// region shutdown
void ShutdownClient(std::unique_ptr<McpClient>& client) {
    client->Close();
    client.reset();
}
// endregion

} // namespace connect_to_a_server
