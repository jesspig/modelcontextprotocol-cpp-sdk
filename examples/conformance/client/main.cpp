#include <mcp/client/McpClient.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>

#include <cstdlib>
#include <algorithm>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace mcp;

namespace {

constexpr std::string_view kModernSpecVersion = "2026-07-28";

bool IsModernConformanceRun() {
    const char* version = std::getenv("MCP_CONFORMANCE_PROTOCOL_VERSION");
    return version != nullptr && std::string_view(version) == kModernSpecVersion;
}

std::vector<std::pair<std::string, JsonValue>> ReadToolCallsContext() {
    std::vector<std::pair<std::string, JsonValue>> calls;
    const char* raw = std::getenv("MCP_CONFORMANCE_CONTEXT");
    if (!raw)
        return calls;
    JsonValue parsed = JsonValue::Parse(raw);
    const JsonValue* tool_calls = parsed.Find("toolCalls");
    if (!tool_calls || !tool_calls->IsArray())
        return calls;
    for (const auto& call : tool_calls->GetArray()) {
        const JsonValue* name = call.Find("name");
        if (!name || !name->IsString())
            continue;
        JsonValue arguments(JsonValue::Object{});
        if (const JsonValue* args = call.Find("arguments"))
            arguments = *args;
        calls.emplace_back(name->GetString(), std::move(arguments));
    }
    return calls;
}

const Tool* FindTool(const ListToolsResult& tools, std::string_view name) {
    auto it = std::find_if(tools.tools.begin(), tools.tools.end(),
        [name](const Tool& t) { return t.name == name; });
    return it == tools.tools.end() ? nullptr : &*it;
}

std::unique_ptr<McpClient> ConnectClient(
    const std::string& server_url,
    std::string_view client_name,
    ConnectMode connect_mode,
    std::optional<ClientCapabilities> capabilities,
    bool enable_mrtr_auto_fulfill = false) {
    HttpClientTransportOptions transport_options;
    transport_options.endpoint = server_url;
    StreamableHttpClientTransport http_transport(transport_options);
    auto transport = http_transport.Connect();

    ClientOptions options;
    options.client_info = Implementation{std::string(client_name), "1.0.0"};
    options.connect_mode = connect_mode;
    options.capabilities = std::move(capabilities);
    if (enable_mrtr_auto_fulfill) {
        ClientOptions::InputRequiredConfig mrtr;
        mrtr.auto_fulfill = true;
        options.input_required_config = mrtr;
    }
    auto client = McpClient::Create(transport, options);
    std::cout << "Successfully connected to MCP server (protocol "
              << client->GetNegotiatedProtocolVersion() << ")" << std::endl;
    return client;
}

void RunBasicClient(const std::string& server_url) {
    auto client = ConnectClient(server_url, "test-client", ConnectMode::Legacy,
        ClientCapabilities{});
    auto tools = client->ListTools();
    std::cout << "Successfully listed tools (" << tools.tools.size() << ")"
              << std::endl;
    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunToolsCallClient(const std::string& server_url) {
    ConnectMode mode = IsModernConformanceRun() ? ConnectMode::Auto
                                                : ConnectMode::Legacy;
    auto client = ConnectClient(server_url, "test-client", mode,
        ClientCapabilities{});
    auto tools = client->ListTools();
    std::cout << "Successfully listed tools (" << tools.tools.size() << ")"
              << std::endl;
    if (FindTool(tools, "add_numbers")) {
        JsonValue arguments(JsonValue::Object{
            {"a", JsonValue(int64_t{5})},
            {"b", JsonValue(int64_t{3})}});
        auto result = client->CallTool("add_numbers", arguments);
        for (const auto& content : result.content) {
            if (auto* text = std::get_if<TextContent>(&content))
                std::cout << "Tool call result: " << text->text << std::endl;
        }
    }
    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunRequestMetadataClient(const std::string& server_url) {
    ClientCapabilities capabilities;
    RootsCapability roots_capability;
    roots_capability.list_changed = true;
    capabilities.roots = roots_capability;
    capabilities.sampling = SamplingCapability{};
    capabilities.elicitation = ElicitationCapability{};

    auto client = ConnectClient(server_url, "test-client", ConnectMode::Auto,
        capabilities);
    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunHttpStandardHeadersClient(const std::string& server_url) {
    auto client = ConnectClient(server_url, "test-client", ConnectMode::Auto,
        ClientCapabilities{});

    auto tools = client->ListTools();
    if (!tools.tools.empty())
        client->CallTool(tools.tools.front().name,
            JsonValue(JsonValue::Object{}));

    auto resources = client->ListResources();
    if (!resources.resources.empty())
        client->ReadResource(resources.resources.front().uri);

    auto prompts = client->ListPrompts();
    if (!prompts.prompts.empty())
        client->GetPrompt(prompts.prompts.front().name,
            JsonValue(JsonValue::Object{}));

    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunHttpCustomHeadersClient(const std::string& server_url) {
    auto client = ConnectClient(server_url, "test-client", ConnectMode::Auto,
        ClientCapabilities{});
    auto tools = client->ListTools();
    std::cout << "listed tools:";
    for (const auto& tool : tools.tools)
        std::cout << " " << tool.name;
    std::cout << std::endl;

    for (const auto& [name, arguments] : ReadToolCallsContext())
        client->CallTool(name, arguments);
    client->Close();
}

void RunHttpInvalidToolHeadersClient(const std::string& server_url) {
    auto client = ConnectClient(server_url, "test-client", ConnectMode::Auto,
        ClientCapabilities{});
    auto tools = client->ListTools();
    std::cout << "post-exclusion tools:";
    for (const auto& tool : tools.tools)
        std::cout << " " << tool.name;
    std::cout << std::endl;

    JsonValue arguments(JsonValue::Object{{"region", JsonValue("us-west1")}});
    for (const auto& tool : tools.tools) {
        try {
            client->CallTool(tool.name, arguments);
        } catch (const std::exception& e) {
            std::cout << "call " << tool.name << " rejected: " << e.what()
                      << std::endl;
        }
    }
    client->Close();
}

void RunMrtrClient(const std::string& server_url) {
    ClientCapabilities capabilities;
    capabilities.elicitation = ElicitationCapability{};

    auto client = ConnectClient(server_url, "test-client", ConnectMode::Auto,
        capabilities, true);
    client->SetElicitationHandler([](const ElicitRequestParams& params) {
        std::cout << "Fulfilling embedded elicitation request: "
                  << params.message << std::endl;
        ElicitResult result;
        result.action = "accept";
        result.content = JsonValue(JsonValue::Object{{"confirmed", JsonValue(true)}});
        return result;
    });

    JsonValue empty_arguments(JsonValue::Object{});
    auto echo_result = client->CallTool("test_mrtr_echo_state", empty_arguments);
    std::cout << "test_mrtr_echo_state done" << std::endl;

    auto no_state_result = client->CallTool("test_mrtr_no_state", empty_arguments);
    std::cout << "test_mrtr_no_state done" << std::endl;

    auto unrelated_result = client->CallTool("test_mrtr_unrelated", empty_arguments);
    std::cout << "test_mrtr_unrelated done" << std::endl;

    try {
        client->CallTool("test_mrtr_no_result_type", empty_arguments);
        std::cout << "test_mrtr_no_result_type done" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "test_mrtr_no_result_type rejected locally (no retry): "
                  << e.what() << std::endl;
    }

    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunElicitationDefaultsClient(const std::string& server_url) {
    ClientCapabilities capabilities;
    ElicitationCapability elicitation_capability;
    elicitation_capability.form =
        JsonValue(JsonValue::Object{{"applyDefaults", JsonValue(true)}});
    capabilities.elicitation = elicitation_capability;

    auto client = ConnectClient(server_url, "elicitation-defaults-test-client",
        ConnectMode::Legacy, capabilities);
    client->SetElicitationHandler([](const ElicitRequestParams& params) {
        std::cout << "Received elicitation request: " << params.message
                  << std::endl;
        std::cout << "Accepting with empty content - SDK should apply defaults"
                  << std::endl;
        ElicitResult result;
        result.action = "accept";
        result.content = JsonValue(JsonValue::Object{});
        return result;
    });

    auto tools = client->ListTools();
    if (!FindTool(tools, "test_client_elicitation_defaults"))
        throw std::runtime_error(
            "Test tool not found: test_client_elicitation_defaults");

    auto result = client->CallTool("test_client_elicitation_defaults",
        JsonValue(JsonValue::Object{}));
    std::cout << "Tool result content count: " << result.content.size()
              << std::endl;

    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunSSERetryClient(const std::string& server_url) {
    auto client = ConnectClient(server_url, "sse-retry-test-client",
        ConnectMode::Legacy, ClientCapabilities{});

    auto tools = client->ListTools();
    if (!FindTool(tools, "test_reconnection"))
        throw std::runtime_error("Test tool not found: test_reconnection");

    std::cout << "Calling test_reconnection tool..." << std::endl;
    auto result = client->CallTool("test_reconnection",
        JsonValue(JsonValue::Object{}));
    std::cout << "Tool result content count: " << result.content.size()
              << std::endl;

    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunJsonSchemaRefNoDerefClient(const std::string& server_url) {
    auto client = ConnectClient(server_url, "json-schema-ref-no-deref-client",
        ConnectMode::Legacy, ClientCapabilities{});
    auto tools = client->ListTools();
    std::cout << "Available tools: " << tools.tools.size() << std::endl;
    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

void RunJsonSchema2020_12PreservationClient(const std::string& server_url) {
    ConnectMode mode = IsModernConformanceRun() ? ConnectMode::Auto
                                                : ConnectMode::Legacy;
    auto client = ConnectClient(server_url,
        "json-schema-2020-12-preservation-client", mode, ClientCapabilities{});

    auto tools = client->ListTools();
    std::cout << "Available tools: " << tools.tools.size() << std::endl;

    const Tool* focal = FindTool(tools, "json_schema_2020_12_tool");
    if (!focal)
        throw std::runtime_error(
            "Tool 'json_schema_2020_12_tool' not advertised by the server");
    std::cout << "Observed inputSchema: " << focal->input_schema.Dump()
              << std::endl;

    JsonValue arguments(JsonValue::Object{{"schema", focal->input_schema}});
    auto result = client->CallTool("json_schema_echo", arguments);
    std::cout << "Echo result content count: " << result.content.size()
              << std::endl;

    client->Close();
    std::cout << "Connection closed successfully" << std::endl;
}

using ScenarioHandler = void (*)(const std::string&);

const std::map<std::string, ScenarioHandler>& ScenarioRegistry() {
    static const std::map<std::string, ScenarioHandler> registry = {
        {"initialize", RunBasicClient},
        {"tools_call", RunToolsCallClient},
        {"request-metadata", RunRequestMetadataClient},
        {"http-standard-headers", RunHttpStandardHeadersClient},
        {"http-custom-headers", RunHttpCustomHeadersClient},
        {"http-invalid-tool-headers", RunHttpInvalidToolHeadersClient},
        {"sep-2322-client-request-state", RunMrtrClient},
        {"elicitation-sep1034-client-defaults", RunElicitationDefaultsClient},
        {"sse-retry", RunSSERetryClient},
        {"json-schema-ref-no-deref", RunJsonSchemaRefNoDerefClient},
        {"json-schema-2020-12-preservation",
            RunJsonSchema2020_12PreservationClient},
    };
    return registry;
}

void PrintUsageAndScenarios(std::ostream& out) {
    out << "Usage: MCP_CONFORMANCE_SCENARIO=<scenario> conformance-client "
           "<server-url>" << std::endl;
    out << "\nThe MCP_CONFORMANCE_SCENARIO env var is set automatically by "
           "the conformance runner." << std::endl;
    out << "\nAvailable scenarios:" << std::endl;
    for (const auto& [name, handler] : ScenarioRegistry())
        out << "  - " << name << std::endl;
}

} // namespace

int main(int argc, char* argv[]) {
    const char* scenario_name = std::getenv("MCP_CONFORMANCE_SCENARIO");
    if (!scenario_name || argc < 2) {
        PrintUsageAndScenarios(std::cerr);
        return 1;
    }

    const auto& registry = ScenarioRegistry();
    auto it = registry.find(scenario_name);
    if (it == registry.end()) {
        std::cerr << "Unknown scenario: " << scenario_name << std::endl;
        PrintUsageAndScenarios(std::cerr);
        return 1;
    }

    try {
        it->second(argv[1]);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
