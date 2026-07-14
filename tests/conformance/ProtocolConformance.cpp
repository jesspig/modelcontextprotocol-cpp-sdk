#include <mcp/McpCore.hpp>
#include <mcp/protocol/WireCodec.hpp>

#include <gtest/gtest.h>

using namespace mcp;

// ====================================================================
// JSON-RPC message round-trip serialization
// ====================================================================
TEST(Conformance, JsonRpcRequestRoundTrip) {
    JsonRpcRequest req;
    req.id = RequestId{int64_t(1)};
    req.method = "tools/list";
    req.params = nlohmann::json::object();

    nlohmann::json j = req;
    auto recovered = j.get<JsonRpcRequest>();

    EXPECT_EQ(recovered.jsonrpc, "2.0");
    EXPECT_EQ(recovered.id, req.id);
    EXPECT_EQ(recovered.method, "tools/list");
    EXPECT_TRUE(recovered.params->empty());
}

TEST(Conformance, JsonRpcNotificationRoundTrip) {
    JsonRpcNotification notif;
    notif.method = "notifications/initialized";

    nlohmann::json j = notif;
    EXPECT_FALSE(j.contains("id"));

    auto recovered = j.get<JsonRpcNotification>();
    EXPECT_EQ(recovered.method, "notifications/initialized");
}

TEST(Conformance, JsonRpcResponseRoundTrip) {
    JsonRpcResponse resp;
    resp.id = RequestId{int64_t(42)};
    resp.result = {{"ok", true}};

    nlohmann::json j = resp;
    auto recovered = j.get<JsonRpcResponse>();
    EXPECT_EQ(recovered.id, resp.id);
    EXPECT_EQ(recovered.result["ok"], true);
}

TEST(Conformance, JsonRpcErrorResponseRoundTrip) {
    JsonRpcErrorResponse err;
    err.id = RequestId{std::string("req-1")};
    err.error = {McpErrorCode::InvalidRequest, "bad request"};

    nlohmann::json j = err;
    auto recovered = j.get<JsonRpcErrorResponse>();
    EXPECT_EQ(recovered.error.code, McpErrorCode::InvalidRequest);
    EXPECT_EQ(recovered.error.message, "bad request");
}

TEST(Conformance, JsonRpcMessageVariantDispatch) {
    nlohmann::json j = {
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"},
        {"params", nlohmann::json::object()}
    };
    auto msg = j.get<JsonRpcMessage>();
    EXPECT_TRUE(IsRequest(msg));

    j = {{"jsonrpc", "2.0"}, {"id", 1}, {"result", {}}};
    msg = j.get<JsonRpcMessage>();
    EXPECT_TRUE(IsResponse(msg));

    j = {{"jsonrpc", "2.0"}, {"id", 1},
         {"error", {{"code", -32601}, {"message", "not found"}}}};
    msg = j.get<JsonRpcMessage>();
    EXPECT_TRUE(IsError(msg));

    j = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    msg = j.get<JsonRpcMessage>();
    EXPECT_TRUE(IsNotification(msg));
}

// ====================================================================
// WireCodec era-gating (2025 vs 2026)
// ====================================================================
TEST(Conformance, WireCodec2025Methods) {
    auto codec = MakeWireCodec("2025-11-25");
    EXPECT_TRUE(codec->HasRequestMethod("tools/list"));
    EXPECT_TRUE(codec->HasRequestMethod("initialize"));
    EXPECT_FALSE(codec->HasRequestMethod("server/discover"));
    EXPECT_FALSE(codec->HasRequestMethod("subscriptions/listen"));
}

TEST(Conformance, WireCodec2026Methods) {
    auto codec = MakeWireCodec("2026-07-28");
    EXPECT_TRUE(codec->HasRequestMethod("tools/list"));
    EXPECT_TRUE(codec->HasRequestMethod("server/discover"));
    EXPECT_TRUE(codec->HasRequestMethod("subscriptions/listen"));
    EXPECT_FALSE(codec->HasRequestMethod("initialize"));
    EXPECT_FALSE(codec->HasRequestMethod("logging/setLevel"));
}

TEST(Conformance, WireCodec2026ValidatesMetaRequirement) {
    auto codec = MakeWireCodec("2026-07-28");
    nlohmann::json body = {{"name", "test"}};

    EXPECT_EQ(codec->ValidateRequest("tools/call", body),
              WireValidation::Invalid);
    EXPECT_EQ(codec->ValidateRequest("server/discover", body),
              WireValidation::Ok);
}

TEST(Conformance, WireCodec2025StampIsNoop) {
    auto codec = MakeWireCodec("2025-11-25");
    nlohmann::json body = nlohmann::json::object();
    RequestMeta meta;
    meta.protocol_version = "2025-11-25";
    codec->StampOutgoingRequest(body, meta);
    EXPECT_TRUE(body.empty());
}

TEST(Conformance, WireCodec2026StampAddsMeta) {
    auto codec = MakeWireCodec("2026-07-28");
    nlohmann::json body = nlohmann::json::object();
    RequestMeta meta;
    meta.protocol_version = "2026-07-28";
    meta.client_info = Implementation{"test", "1.0"};
    codec->StampOutgoingRequest(body, meta);

    ASSERT_TRUE(body.contains("_meta"));
    EXPECT_EQ(body["_meta"]["io.modelcontextprotocol/protocolVersion"],
              "2026-07-28");
    EXPECT_EQ(body["_meta"]["io.modelcontextprotocol/clientInfo"]["name"],
              "test");
}

TEST(Conformance, WireCodec2026EncodeResult) {
    auto codec = MakeWireCodec("2026-07-28");
    nlohmann::json result = {{"content", nlohmann::json::array()}};
    auto encoded = codec->EncodeResult("tools/call", result);
    EXPECT_EQ(encoded["resultType"], "complete");
}

TEST(Conformance, WireCodecUnknownVersionFallsBack) {
    auto codec = MakeWireCodec("2024-01-01");
    EXPECT_EQ(codec->Era(), "2025-11-25");
}

// ====================================================================
// Version negotiation (discover vs initialize)
// ====================================================================
TEST(Conformance, DiscoverResultRoundTrip) {
    DiscoverResult r;
    r.supported_versions = {"2025-11-25", "2026-07-28"};
    r.capabilities = ServerCapabilities{};
    r.capabilities.tools = ToolsCapability{};
    r.server_info = Implementation{"server", "1.0.0"};

    nlohmann::json j = r;
    auto recovered = j.get<DiscoverResult>();

    ASSERT_EQ(recovered.supported_versions.size(), 2);
    EXPECT_EQ(recovered.supported_versions[1], "2026-07-28");
    EXPECT_EQ(recovered.server_info.name, "server");
}

TEST(Conformance, InitializeResultRoundTrip) {
    InitializeResult r;
    r.protocol_version = "2026-07-28";
    r.capabilities = ServerCapabilities{};
    r.capabilities.tools = ToolsCapability{};
    r.server_info = Implementation{"server", "1.0"};

    nlohmann::json j = r;
    auto recovered = j.get<InitializeResult>();

    EXPECT_EQ(recovered.protocol_version, "2026-07-28");
    EXPECT_TRUE(recovered.capabilities.tools.has_value());
}

TEST(Conformance, DiscoverRequestSerialization) {
    DiscoverRequestParams params;
    nlohmann::json j = params;
    EXPECT_TRUE(j.is_object());
    EXPECT_TRUE(j.empty());
}

TEST(Conformance, InitializeRequestSerialization) {
    InitializeRequestParams params;
    params.protocol_version = "2026-07-28";
    params.capabilities = ClientCapabilities{};
    params.client_info = Implementation{"client", "1.0"};

    nlohmann::json j = params;
    EXPECT_EQ(j["protocolVersion"], "2026-07-28");
    EXPECT_EQ(j["clientInfo"]["name"], "client");
}

// ====================================================================
// Error code mappings
// ====================================================================
TEST(Conformance, StandardErrorCodes) {
    EXPECT_EQ(static_cast<int32_t>(McpErrorCode::ParseError), -32700);
    EXPECT_EQ(static_cast<int32_t>(McpErrorCode::InvalidRequest), -32600);
    EXPECT_EQ(static_cast<int32_t>(McpErrorCode::MethodNotFound), -32601);
    EXPECT_EQ(static_cast<int32_t>(McpErrorCode::InvalidParams), -32602);
    EXPECT_EQ(static_cast<int32_t>(McpErrorCode::InternalError), -32603);
}

TEST(Conformance, McpSpecificErrorCodes) {
    EXPECT_EQ(static_cast<int32_t>(McpErrorCode::HeaderMismatch), -32020);
    EXPECT_EQ(static_cast<int32_t>(McpErrorCode::MissingRequiredClientCapability), -32021);
    EXPECT_EQ(static_cast<int32_t>(McpErrorCode::UnsupportedProtocolVersion), -32022);
    EXPECT_EQ(static_cast<int32_t>(McpErrorCode::UrlElicitationRequired), -32042);
    EXPECT_EQ(static_cast<int32_t>(McpErrorCode::ConnectionClosed), -32000);
    EXPECT_EQ(static_cast<int32_t>(McpErrorCode::RequestTimeout), -32001);
}

TEST(Conformance, ErrorCodeRoundTrip) {
    ErrorData ed{McpErrorCode::MethodNotFound, "method not found"};
    nlohmann::json j = ed;
    auto recovered = j.get<ErrorData>();
    EXPECT_EQ(recovered.code, McpErrorCode::MethodNotFound);
    EXPECT_EQ(recovered.message, "method not found");
}

TEST(Conformance, ErrorCodeWithData) {
    ErrorData ed;
    ed.code = McpErrorCode::InvalidParams;
    ed.message = "bad param";
    ed.data = nlohmann::json{{"param", "x"}};
    nlohmann::json j = ed;
    auto recovered = j.get<ErrorData>();
    ASSERT_TRUE(recovered.data.has_value());
    EXPECT_EQ((*recovered.data)["param"], "x");
}

TEST(Conformance, WireCodec2026ErrorRemapping) {
    auto codec = MakeWireCodec("2026-07-28");
    EXPECT_EQ(codec->EncodeErrorCode(-32001), -32020);
    EXPECT_EQ(codec->EncodeErrorCode(-32003), -32021);
    EXPECT_EQ(codec->EncodeErrorCode(-32004), -32022);
    EXPECT_EQ(codec->EncodeErrorCode(-32601), -32601);
}

// ====================================================================
// Tool / Resource / Prompt type serialization
// ====================================================================
TEST(Conformance, ToolSerialization) {
    Tool t;
    t.name = "echo";
    t.description = "Echo input back";
    t.input_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};

    nlohmann::json j = t;
    EXPECT_EQ(j["name"], "echo");
    EXPECT_EQ(j["description"], "Echo input back");
    EXPECT_EQ(j["inputSchema"]["type"], "object");

    auto recovered = j.get<Tool>();
    EXPECT_EQ(recovered.name, "echo");
}

TEST(Conformance, ToolWithAnnotations) {
    Tool t;
    t.name = "read-only-tool";
    t.input_schema = nlohmann::json::object();
    t.annotations = ToolAnnotations{};
    t.annotations->read_only_hint = true;
    t.annotations->idempotent_hint = true;

    nlohmann::json j = t;
    EXPECT_EQ(j["annotations"]["readOnlyHint"], true);
    EXPECT_EQ(j["annotations"]["idempotentHint"], true);
    EXPECT_FALSE(j["annotations"].contains("openWorldHint"));

    auto recovered = j.get<Tool>();
    ASSERT_TRUE(recovered.annotations.has_value());
    EXPECT_TRUE(recovered.annotations->read_only_hint.value_or(false));
}

TEST(Conformance, ToolWithIcons) {
    Tool t;
    t.name = "icon-tool";
    t.input_schema = nlohmann::json::object();
    t.icons = {Icon{"https://example.com/icon.svg", "image/svg+xml"}};

    nlohmann::json j = t;
    ASSERT_TRUE(j.contains("icons"));
    EXPECT_EQ(j["icons"][0]["src"], "https://example.com/icon.svg");
}

TEST(Conformance, ResourceSerialization) {
    Resource r;
    r.uri = "file:///data.txt";
    r.name = "Data";
    r.mime_type = "text/plain";

    nlohmann::json j = r;
    EXPECT_EQ(j["uri"], "file:///data.txt");
    EXPECT_EQ(j["name"], "Data");
    EXPECT_EQ(j["mimeType"], "text/plain");

    auto recovered = j.get<Resource>();
    EXPECT_EQ(recovered.uri, "file:///data.txt");
    EXPECT_EQ(recovered.name, "Data");
}

TEST(Conformance, ResourceTemplateSerialization) {
    ResourceTemplate rt;
    rt.uri_template = "file:///{path}";
    rt.name = "Dynamic Files";

    nlohmann::json j = rt;
    EXPECT_EQ(j["uriTemplate"], "file:///{path}");

    auto recovered = j.get<ResourceTemplate>();
    EXPECT_EQ(recovered.uri_template, "file:///{path}");
}

TEST(Conformance, PromptSerialization) {
    Prompt p;
    p.name = "greet";
    p.description = "Generate a greeting";

    PromptArgument arg;
    arg.name = "name";
    arg.description = "Person to greet";
    arg.required = true;
    p.arguments = {arg};

    nlohmann::json j = p;
    EXPECT_EQ(j["name"], "greet");
    ASSERT_TRUE(j.contains("arguments"));
    EXPECT_EQ(j["arguments"][0]["name"], "name");
    EXPECT_EQ(j["arguments"][0]["required"], true);

    auto recovered = j.get<Prompt>();
    EXPECT_EQ(recovered.name, "greet");
    ASSERT_TRUE(recovered.arguments.has_value());
    EXPECT_TRUE(recovered.arguments->at(0).required.value_or(false));
}

TEST(Conformance, PromptMessageSerialization) {
    PromptMessage pm;
    pm.role = "user";
    pm.content = TextContent{"text", "Hello"};

    nlohmann::json j = pm;
    EXPECT_EQ(j["role"], "user");
    EXPECT_EQ(j["content"]["type"], "text");
    EXPECT_EQ(j["content"]["text"], "Hello");

    auto recovered = j.get<PromptMessage>();
    EXPECT_EQ(recovered.role, "user");
    EXPECT_TRUE(std::holds_alternative<TextContent>(recovered.content));
}

// ====================================================================
// Content variant serialization
// ====================================================================
TEST(Conformance, ContentVariantText) {
    ContentVariant cv = TextContent{"text", "hello"};
    nlohmann::json j = cv;
    EXPECT_EQ(j["type"], "text");
    EXPECT_EQ(j["text"], "hello");
}

TEST(Conformance, ContentVariantImage) {
    ContentVariant cv = ImageContent{"image", "base64data", "image/png"};
    nlohmann::json j = cv;
    EXPECT_EQ(j["type"], "image");
}

TEST(Conformance, ContentVariantAudio) {
    ContentVariant cv = AudioContent{"audio", "base64data", "audio/wav"};
    nlohmann::json j = cv;
    EXPECT_EQ(j["type"], "audio");
}

TEST(Conformance, ContentVariantEmbeddedResource) {
    TextResourceContents trc;
    trc.uri = "file:///doc.txt";
    trc.text = "embedded";
    EmbeddedResource er;
    er.resource = ResourceContents(trc);

    ContentVariant cv = er;
    nlohmann::json j = cv;
    EXPECT_EQ(j["type"], "resource");
    EXPECT_EQ(j["resource"]["text"], "embedded");
}

// ====================================================================
// Implementation / ServerInfo
// ====================================================================
TEST(Conformance, ImplementationRoundTrip) {
    Implementation impl;
    impl.name = "test-server";
    impl.version = "1.0.0";

    nlohmann::json j = impl;
    auto recovered = j.get<Implementation>();
    EXPECT_EQ(recovered.name, "test-server");
    EXPECT_EQ(recovered.version, "1.0.0");
}

// ====================================================================
// ProtocolVersion helpers
// ====================================================================
TEST(Conformance, IsModernProtocolVersion) {
    EXPECT_FALSE(IsModernProtocolVersion("2025-11-25"));
    EXPECT_TRUE(IsModernProtocolVersion("2026-07-28"));
    EXPECT_TRUE(IsModernProtocolVersion("2027-01-01"));
}

TEST(Conformance, LatestProtocolVersion) {
    EXPECT_EQ(kLatestProtocolVersion, "2026-07-28");
}

TEST(Conformance, JsonRpcVersionConstant) {
    EXPECT_EQ(kJsonRpcVersion, "2.0");
}

// ====================================================================
// List results with pagination
// ====================================================================
TEST(Conformance, ListToolsResultWithCursor) {
    ListToolsResult r;
    r.tools = {Tool{}};
    r.tools[0].name = "t1";
    r.tools[0].input_schema = nlohmann::json::object();
    r.next_cursor = "next-page";

    nlohmann::json j = r;
    EXPECT_EQ(j["nextCursor"], "next-page");
}

TEST(Conformance, ListResourcesResultRoundTrip) {
    ListResourcesResult r;
    Resource res;
    res.uri = "file:///a.txt";
    res.name = "A";
    r.resources = {res};

    nlohmann::json j = r;
    auto recovered = j.get<ListResourcesResult>();
    ASSERT_EQ(recovered.resources.size(), 1);
    EXPECT_EQ(recovered.resources[0].name, "A");
}

TEST(Conformance, ListPromptsResultRoundTrip) {
    ListPromptsResult r;
    Prompt p;
    p.name = "test-prompt";
    r.prompts = {p};

    nlohmann::json j = r;
    auto recovered = j.get<ListPromptsResult>();
    ASSERT_EQ(recovered.prompts.size(), 1);
    EXPECT_EQ(recovered.prompts[0].name, "test-prompt");
}

// ====================================================================
// Capabilities serialization
// ====================================================================
TEST(Conformance, ServerCapabilitiesRoundTrip) {
    ServerCapabilities caps;
    caps.tools = ToolsCapability{true};
    caps.resources = ResourcesCapability{true, false};
    caps.prompts = PromptsCapability{};

    nlohmann::json j = caps;
    EXPECT_TRUE(j["tools"]["listChanged"]);
    EXPECT_TRUE(j["resources"]["subscribe"]);
    EXPECT_FALSE(j["resources"]["listChanged"]);
    EXPECT_TRUE(j["prompts"].is_object());

    auto recovered = j.get<ServerCapabilities>();
    EXPECT_TRUE(recovered.tools.has_value());
    EXPECT_TRUE(recovered.resources.has_value());
}

TEST(Conformance, ClientCapabilitiesRoundTrip) {
    ClientCapabilities caps;
    caps.roots = RootsCapability{};
    caps.elicitation = ElicitationCapability{};
    caps.elicitation->form = nlohmann::json::object();

    nlohmann::json j = caps;
    EXPECT_TRUE(j.contains("roots"));
    EXPECT_TRUE(j.contains("elicitation"));

    auto recovered = j.get<ClientCapabilities>();
    EXPECT_TRUE(recovered.roots.has_value());
    EXPECT_TRUE(recovered.elicitation.has_value());
}

// ====================================================================
// CallToolResult with structured content
// ====================================================================
TEST(Conformance, CallToolResultWithStructuredContent) {
    CallToolResult r;
    r.content = {TextContent{"text", "result"}};
    r.structured_content = nlohmann::json{{"key", "value"}};
    r.is_error = false;

    nlohmann::json j = r;
    EXPECT_EQ(j["structuredContent"]["key"], "value");
    EXPECT_EQ(j["isError"], false);

    auto recovered = j.get<CallToolResult>();
    EXPECT_TRUE(recovered.structured_content.has_value());
    EXPECT_EQ((*recovered.structured_content)["key"], "value");
}

// ====================================================================
// RequestMeta with full fields
// ====================================================================
TEST(Conformance, RequestMetaFullRoundTrip) {
    RequestMeta m;
    m.protocol_version = "2026-07-28";
    m.client_info = Implementation{"client", "1.0"};
    m.client_capabilities = ClientCapabilities{};
    m.client_capabilities->roots = RootsCapability{};

    nlohmann::json j = m;
    EXPECT_EQ(j["io.modelcontextprotocol/protocolVersion"], "2026-07-28");
    EXPECT_EQ(j["io.modelcontextprotocol/clientInfo"]["name"], "client");

    auto recovered = j.get<RequestMeta>();
    EXPECT_EQ(recovered.protocol_version, "2026-07-28");
    EXPECT_TRUE(recovered.client_info.has_value());
    EXPECT_TRUE(recovered.client_capabilities.has_value());
}
