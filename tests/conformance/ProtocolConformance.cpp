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

// ====================================================================
// Group 1: Elicitation serialization (10-15 tests)
// ====================================================================
TEST(Conformance, ElicitRequestParamsFormRoundTrip) {
    ElicitRequestParams p;
    p.message = "What is your name?";

    nlohmann::json j = p;
    EXPECT_EQ(j["message"], "What is your name?");
    EXPECT_FALSE(j.contains("requestedSchema"));

    auto recovered = j.get<ElicitRequestParams>();
    EXPECT_EQ(recovered.message, "What is your name?");
    EXPECT_FALSE(recovered.requested_schema.has_value());
}

TEST(Conformance, ElicitRequestParamsWithSchemaRoundTrip) {
    ElicitRequestParams p;
    p.message = "Enter your age";
    p.requested_schema = nlohmann::json{
        {"type", "object"},
        {"properties", {{"age", {{"type", "number"}}}}}
    };

    nlohmann::json j = p;
    EXPECT_EQ(j["message"], "Enter your age");
    EXPECT_EQ(j["requestedSchema"]["type"], "object");
    EXPECT_EQ(j["requestedSchema"]["properties"]["age"]["type"], "number");

    auto recovered = j.get<ElicitRequestParams>();
    EXPECT_TRUE(recovered.requested_schema.has_value());
    EXPECT_EQ((*recovered.requested_schema)["properties"]["age"]["type"], "number");
}

TEST(Conformance, ElicitResultAcceptRoundTrip) {
    ElicitResult r;
    r.values = nlohmann::json{{"name", "Alice"}};
    r.result_type = ResultType::Complete;

    nlohmann::json j = r;
    EXPECT_EQ(j["values"]["name"], "Alice");
    EXPECT_EQ(j["resultType"], "complete");

    auto recovered = j.get<ElicitResult>();
    EXPECT_TRUE(recovered.values.has_value());
    EXPECT_EQ((*recovered.values)["name"], "Alice");
    EXPECT_EQ(recovered.result_type, ResultType::Complete);
}

TEST(Conformance, ElicitResultDeclineRoundTrip) {
    ElicitResult r;
    r.result_type = ResultType::InputRequired;

    nlohmann::json j = r;
    EXPECT_EQ(j["resultType"], "input_required");

    auto recovered = j.get<ElicitResult>();
    EXPECT_FALSE(recovered.values.has_value());
    EXPECT_EQ(recovered.result_type, ResultType::InputRequired);
}

TEST(Conformance, ElicitResultWithMeta) {
    ElicitResult r;
    r.values = nlohmann::json{{"ok", true}};
    r.meta = nlohmann::json{{"trace", "abc"}};

    nlohmann::json j = r;
    EXPECT_EQ(j["_meta"]["trace"], "abc");

    auto recovered = j.get<ElicitResult>();
    ASSERT_TRUE(recovered.meta.has_value());
    EXPECT_EQ((*recovered.meta)["trace"], "abc");
}

TEST(Conformance, ElicitResultTypedAccept) {
    ElicitResultTyped<std::string> typed;
    typed.action = "accept";
    typed.content = "Alice";

    nlohmann::json j = typed;
    EXPECT_EQ(j["action"], "accept");
    EXPECT_EQ(j["content"], "Alice");

    auto recovered = j.get<ElicitResultTyped<std::string>>();
    EXPECT_TRUE(recovered.is_accepted());
    EXPECT_TRUE(recovered.content.has_value());
    EXPECT_EQ(*recovered.content, "Alice");
}

TEST(Conformance, ElicitResultTypedDecline) {
    ElicitResultTyped<int> typed;
    typed.action = "decline";

    nlohmann::json j = typed;
    EXPECT_EQ(j["action"], "decline");
    EXPECT_FALSE(j.contains("content"));

    auto recovered = j.get<ElicitResultTyped<int>>();
    EXPECT_FALSE(recovered.is_accepted());
    EXPECT_FALSE(recovered.content.has_value());
}

TEST(Conformance, ElicitResultTypedCancel) {
    ElicitResultTyped<nlohmann::json> typed;
    EXPECT_EQ(typed.action, "cancel");

    nlohmann::json j = typed;
    EXPECT_EQ(j["action"], "cancel");

    auto recovered = j.get<ElicitResultTyped<nlohmann::json>>();
    EXPECT_EQ(recovered.action, "cancel");
}

TEST(Conformance, ElicitMethodConstant) {
    EXPECT_EQ(methods::kElicit, "elicitation/create");
}

TEST(Conformance, MakeInputRequestForElicitation) {
    ElicitRequestParams params;
    params.message = "confirm?";
    auto j = MakeInputRequestForElicitation(params);

    EXPECT_EQ(j["method"], "elicitation/create");
    EXPECT_EQ(j["params"]["message"], "confirm?");
}

TEST(Conformance, MakeInputResponseFromElicitResult) {
    ElicitResult result;
    result.values = nlohmann::json{{"ok", true}};
    result.result_type = ResultType::Complete;

    auto j = MakeInputResponseFromElicitResult(result);
    EXPECT_EQ(j["values"]["ok"], true);
    EXPECT_EQ(j["resultType"], "complete");
}

TEST(Conformance, ElicitRequestParamsEmptyMessage) {
    ElicitRequestParams p;
    p.message = "";

    nlohmann::json j = p;
    EXPECT_EQ(j["message"], "");

    auto recovered = j.get<ElicitRequestParams>();
    EXPECT_TRUE(recovered.message.empty());
}

TEST(Conformance, ElicitResultTypedJsonValue) {
    ElicitResultTyped<nlohmann::json> typed;
    typed.action = "accept";
    typed.content = nlohmann::json{{"nested", {{"value", 42}}}};

    nlohmann::json j = typed;
    EXPECT_EQ(j["content"]["nested"]["value"], 42);

    auto recovered = j.get<ElicitResultTyped<nlohmann::json>>();
    ASSERT_TRUE(recovered.content.has_value());
    EXPECT_EQ((*recovered.content)["nested"]["value"], 42);
}

// ====================================================================
// Group 2: MRTR / InputRequired (10-15 tests)
// ====================================================================
TEST(Conformance, InputRequiredResultRoundTrip) {
    InputRequiredResult ir;
    ir.input_requests.elicit = InputRequestElicit{"provide value"};
    ir.request_state = "state-abc";

    nlohmann::json j = ir;
    EXPECT_EQ(j["resultType"], "input_required");
    EXPECT_EQ(j["inputRequests"]["elicit"]["message"], "provide value");
    EXPECT_EQ(j["requestState"], "state-abc");

    auto recovered = j.get<InputRequiredResult>();
    EXPECT_TRUE(recovered.input_requests.elicit.has_value());
    EXPECT_EQ(recovered.input_requests.elicit->message, "provide value");
    EXPECT_TRUE(recovered.request_state.has_value());
    EXPECT_EQ(*recovered.request_state, "state-abc");
}

TEST(Conformance, InputRequiredResultWithConfirm) {
    InputRequiredResult ir;
    ir.input_requests.confirm = InputRequestElicit{"Are you sure?"};

    nlohmann::json j = ir;
    EXPECT_EQ(j["inputRequests"]["confirm"]["message"], "Are you sure?");

    auto recovered = j.get<InputRequiredResult>();
    EXPECT_TRUE(recovered.input_requests.confirm.has_value());
    EXPECT_EQ(recovered.input_requests.confirm->message, "Are you sure?");
}

TEST(Conformance, InputRequiredResultBothRequests) {
    InputRequiredResult ir;
    ir.input_requests.confirm = InputRequestElicit{"Confirm?"};
    ir.input_requests.elicit = InputRequestElicit{"Provide details"};

    nlohmann::json j = ir;
    EXPECT_TRUE(j["inputRequests"].contains("confirm"));
    EXPECT_TRUE(j["inputRequests"].contains("elicit"));

    auto recovered = j.get<InputRequiredResult>();
    EXPECT_TRUE(recovered.input_requests.confirm.has_value());
    EXPECT_TRUE(recovered.input_requests.elicit.has_value());
}

TEST(Conformance, InputRequestElicitWithSchema) {
    InputRequestElicit elicit;
    elicit.message = "pick one";
    elicit.requested_schema = nlohmann::json{{"enum", {"a", "b", "c"}}};

    nlohmann::json j = elicit;
    EXPECT_EQ(j["message"], "pick one");
    EXPECT_EQ(j["requestedSchema"]["enum"][0], "a");

    auto recovered = j.get<InputRequestElicit>();
    EXPECT_TRUE(recovered.requested_schema.has_value());
}

TEST(Conformance, IsInputRequiredResultTrue) {
    nlohmann::json j = {{"resultType", "input_required"}, {"inputRequests", {}}};
    EXPECT_TRUE(IsInputRequiredResult(j));
}

TEST(Conformance, IsInputRequiredResultFalse) {
    nlohmann::json j = {{"resultType", "complete"}};
    EXPECT_FALSE(IsInputRequiredResult(j));
}

TEST(Conformance, IsInputRequiredResultMissingKey) {
    nlohmann::json j = {{"ok", true}};
    EXPECT_FALSE(IsInputRequiredResult(j));
}

TEST(Conformance, ExtractInputRequestsFound) {
    nlohmann::json j = {
        {"inputRequests", {
            {"elicit", {{"message", "enter value"}}}
        }}
    };
    auto extracted = ExtractInputRequests(j);
    ASSERT_TRUE(extracted.has_value());
    ASSERT_TRUE(extracted->elicit.has_value());
    EXPECT_EQ(extracted->elicit->message, "enter value");
}

TEST(Conformance, ExtractInputRequestsNotFound) {
    nlohmann::json j = {{"result", "ok"}};
    auto extracted = ExtractInputRequests(j);
    EXPECT_FALSE(extracted.has_value());
}

TEST(Conformance, InputRequestsEmpty) {
    InputRequests ir;
    nlohmann::json j = ir;
    EXPECT_TRUE(j.empty());

    auto recovered = j.get<InputRequests>();
    EXPECT_FALSE(recovered.confirm.has_value());
    EXPECT_FALSE(recovered.elicit.has_value());
}

TEST(Conformance, CallToolRequestWithInputResponses) {
    CallToolRequestParams p;
    p.name = "test";
    p.input_responses = nlohmann::json{{"values", {{"key", "val"}}}};
    p.request_state = "state-xyz";

    nlohmann::json j = p;
    EXPECT_EQ(j["inputResponses"]["values"]["key"], "val");
    EXPECT_EQ(j["requestState"], "state-xyz");

    auto recovered = j.get<CallToolRequestParams>();
    ASSERT_TRUE(recovered.input_responses.has_value());
    ASSERT_TRUE(recovered.request_state.has_value());
    EXPECT_EQ(*recovered.request_state, "state-xyz");
}

// ====================================================================
// Group 3: Elicitation Schema types as JSON (8-10 tests)
// ====================================================================
TEST(Conformance, SchemaStringTypeRoundTrip) {
    nlohmann::json schema = {
        {"type", "string"},
        {"minLength", 1},
        {"maxLength", 100},
        {"format", "email"}
    };

    nlohmann::json j = schema;
    EXPECT_EQ(j["type"], "string");
    EXPECT_EQ(j["minLength"], 1);
    EXPECT_EQ(j["maxLength"], 100);
    EXPECT_EQ(j["format"], "email");

    ElicitRequestParams p;
    p.message = "Enter email";
    p.requested_schema = schema;

    nlohmann::json j2 = p;
    EXPECT_EQ(j2["requestedSchema"]["minLength"], 1);
    EXPECT_EQ(j2["requestedSchema"]["format"], "email");

    auto recovered = j2.get<ElicitRequestParams>();
    ASSERT_TRUE(recovered.requested_schema.has_value());
    EXPECT_EQ((*recovered.requested_schema)["format"], "email");
}

TEST(Conformance, SchemaNumberTypeRoundTrip) {
    nlohmann::json schema = {
        {"type", "number"},
        {"minimum", 0},
        {"maximum", 150}
    };

    nlohmann::json j = schema;
    EXPECT_EQ(j["minimum"], 0);
    EXPECT_EQ(j["maximum"], 150);

    ElicitRequestParams p;
    p.message = "Enter age";
    p.requested_schema = schema;

    auto recovered = p.requested_schema;
    EXPECT_EQ((*recovered)["minimum"], 0);
}

TEST(Conformance, SchemaBooleanTypeRoundTrip) {
    nlohmann::json schema = {
        {"type", "boolean"},
        {"default", true}
    };

    nlohmann::json j = schema;
    EXPECT_EQ(j["type"], "boolean");
    EXPECT_EQ(j["default"], true);
}

TEST(Conformance, SchemaEnumTypeRoundTrip) {
    nlohmann::json schema = {
        {"type", "string"},
        {"enum", {"red", "green", "blue"}}
    };

    nlohmann::json j = schema;
    EXPECT_EQ(j["enum"].size(), 3);
    EXPECT_EQ(j["enum"][1], "green");
}

TEST(Conformance, SchemaSingleSelectEnumUntitled) {
    nlohmann::json schema = {
        {"type", "string"},
        {"enum", {"option1", "option2"}}
    };

    ElicitRequestParams p;
    p.message = "Choose one";
    p.requested_schema = schema;

    nlohmann::json j = p;
    EXPECT_EQ(j["requestedSchema"]["enum"].size(), 2);
    EXPECT_FALSE(j["requestedSchema"].contains("title"));
}

TEST(Conformance, SchemaSingleSelectEnumTitled) {
    nlohmann::json schema = {
        {"type", "string"},
        {"enum", {"yes", "no"}},
        {"title", "Confirmation"}
    };

    ElicitRequestParams p;
    p.message = "Confirm?";
    p.requested_schema = schema;

    nlohmann::json j = p;
    EXPECT_EQ(j["requestedSchema"]["title"], "Confirmation");
    EXPECT_EQ(j["requestedSchema"]["enum"][0], "yes");
}

TEST(Conformance, SchemaMultiSelectUntitled) {
    nlohmann::json schema = {
        {"type", "array"},
        {"items", {{"type", "string"}, {"enum", {"a", "b", "c"}}}}
    };

    nlohmann::json j = schema;
    EXPECT_EQ(j["items"]["enum"].size(), 3);
}

TEST(Conformance, SchemaMultiSelectTitled) {
    nlohmann::json schema = {
        {"type", "array"},
        {"items", {{"type", "string"}, {"enum", {"x", "y"}}}},
        {"title", "Pick multiple"}
    };

    ElicitRequestParams p;
    p.message = "Select";
    p.requested_schema = schema;

    nlohmann::json j = p;
    EXPECT_EQ(j["requestedSchema"]["title"], "Pick multiple");
}

// ====================================================================
// Group 4: Structured Meta (8-10 tests)
// ====================================================================
TEST(Conformance, MetaObjectRoundTrip) {
    MetaObject mo;
    mo.progress_token = int64_t(42);

    nlohmann::json j = mo;
    EXPECT_EQ(j["progressToken"], 42);

    auto recovered = j.get<MetaObject>();
    ASSERT_TRUE(recovered.progress_token.has_value());
    EXPECT_EQ(std::get<int64_t>(*recovered.progress_token), 42);
}

TEST(Conformance, MetaObjectStringProgressToken) {
    MetaObject mo;
    mo.progress_token = std::string("token-abc");

    nlohmann::json j = mo;
    EXPECT_EQ(j["progressToken"], "token-abc");

    auto recovered = j.get<MetaObject>();
    ASSERT_TRUE(recovered.progress_token.has_value());
    EXPECT_EQ(std::get<std::string>(*recovered.progress_token), "token-abc");
}

TEST(Conformance, MetaObjectExtensions) {
    MetaObject mo;
    mo.extensions = nlohmann::json{{"customField", "customValue"}};

    nlohmann::json j = mo;
    EXPECT_EQ(j["customField"], "customValue");

    auto recovered = j.get<MetaObject>();
    ASSERT_TRUE(recovered.extensions.has_value());
    EXPECT_EQ((*recovered.extensions)["customField"], "customValue");
}

TEST(Conformance, RequestMetaObjectRoundTrip) {
    RequestMetaObject rmo;
    rmo.protocol_version = "2026-07-28";
    rmo.client_info = Implementation{"test-client", "1.0"};
    rmo.client_capabilities = ClientCapabilities{};
    rmo.client_capabilities->elicitation = ElicitationCapability{};
    rmo.client_capabilities->elicitation->form = nlohmann::json::object();

    nlohmann::json j = rmo;
    EXPECT_EQ(j["io.modelcontextprotocol/protocolVersion"], "2026-07-28");
    EXPECT_EQ(j["io.modelcontextprotocol/clientInfo"]["name"], "test-client");
    EXPECT_TRUE(j["io.modelcontextprotocol/clientCapabilities"].contains("elicitation"));

    auto recovered = j.get<RequestMetaObject>();
    EXPECT_EQ(recovered.protocol_version, "2026-07-28");
    ASSERT_TRUE(recovered.client_info.has_value());
    EXPECT_EQ(recovered.client_info->name, "test-client");
    ASSERT_TRUE(recovered.client_capabilities.has_value());
}

TEST(Conformance, RequestMetaObjectWithLogLevel) {
    RequestMetaObject rmo;
    rmo.log_level = LoggingLevel::Debug;

    nlohmann::json j = rmo;
    EXPECT_EQ(j["io.modelcontextprotocol/logLevel"], "debug");

    auto recovered = j.get<RequestMetaObject>();
    ASSERT_TRUE(recovered.log_level.has_value());
    EXPECT_EQ(*recovered.log_level, LoggingLevel::Debug);
}

TEST(Conformance, RequestMetaObjectWithProgressToken) {
    RequestMetaObject rmo;
    rmo.progress_token = int64_t(99);

    nlohmann::json j = rmo;
    EXPECT_EQ(j["progressToken"], 99);

    auto recovered = j.get<RequestMetaObject>();
    ASSERT_TRUE(recovered.progress_token.has_value());
}

TEST(Conformance, NotificationMetaObjectRoundTrip) {
    NotificationMetaObject nmo;
    nmo.subscription_id = RequestId{int64_t(7)};

    nlohmann::json j = nmo;
    EXPECT_EQ(j["io.modelcontextprotocol/subscriptionId"], 7);

    auto recovered = j.get<NotificationMetaObject>();
    ASSERT_TRUE(recovered.subscription_id.has_value());
    EXPECT_EQ(std::get<int64_t>(*recovered.subscription_id), 7);
}

TEST(Conformance, NotificationMetaObjectStringId) {
    NotificationMetaObject nmo;
    nmo.subscription_id = RequestId{std::string("sub-abc")};

    nlohmann::json j = nmo;
    EXPECT_EQ(j["io.modelcontextprotocol/subscriptionId"], "sub-abc");

    auto recovered = j.get<NotificationMetaObject>();
    ASSERT_TRUE(recovered.subscription_id.has_value());
    EXPECT_EQ(std::get<std::string>(*recovered.subscription_id), "sub-abc");
}

TEST(Conformance, NotificationMetaObjectEmpty) {
    NotificationMetaObject nmo;
    nlohmann::json j = nmo;
    EXPECT_TRUE(j.empty());

    auto recovered = j.get<NotificationMetaObject>();
    EXPECT_FALSE(recovered.subscription_id.has_value());
}

// ====================================================================
// Group 5: Extensions Capability (5-8 tests)
// ====================================================================
TEST(Conformance, ExtensionsCapabilityServerRoundTrip) {
    ServerCapabilities caps;
    caps.extensions = ExtensionsCapability{};

    nlohmann::json j = caps;
    EXPECT_TRUE(j.contains("extensions"));
    EXPECT_TRUE(j["extensions"].is_object());

    auto recovered = j.get<ServerCapabilities>();
    EXPECT_TRUE(recovered.extensions.has_value());
}

TEST(Conformance, ExtensionsCapabilityClientRoundTrip) {
    ClientCapabilities caps;
    caps.extensions = ExtensionsCapability{};

    nlohmann::json j = caps;
    EXPECT_TRUE(j.contains("extensions"));

    auto recovered = j.get<ClientCapabilities>();
    EXPECT_TRUE(recovered.extensions.has_value());
}

TEST(Conformance, ServerCapabilitiesNoExtensions) {
    ServerCapabilities caps;
    caps.tools = ToolsCapability{};

    nlohmann::json j = caps;
    EXPECT_FALSE(j.contains("extensions"));

    auto recovered = j.get<ServerCapabilities>();
    EXPECT_FALSE(recovered.extensions.has_value());
}

TEST(Conformance, ClientCapabilitiesNoExtensions) {
    ClientCapabilities caps;
    nlohmann::json j = caps;
    EXPECT_FALSE(j.contains("extensions"));
}

TEST(Conformance, ExtensionsCapabilityWithOtherCaps) {
    ServerCapabilities caps;
    caps.tools = ToolsCapability{};
    caps.extensions = ExtensionsCapability{};
    caps.subscriptions = SubscriptionsCapability{};

    nlohmann::json j = caps;
    EXPECT_TRUE(j.contains("extensions"));
    EXPECT_TRUE(j.contains("subscriptions"));
    EXPECT_TRUE(j.contains("tools"));
}

// ====================================================================
// Group 6: ResultType enum (8-10 tests)
// ====================================================================
TEST(Conformance, CallToolResultHasResultType) {
    CallToolResult r;
    r.content = {TextContent{"text", "hello"}};

    nlohmann::json j = r;
    EXPECT_EQ(j["resultType"], "complete");

    auto recovered = j.get<CallToolResult>();
    EXPECT_EQ(recovered.result_type, ResultType::Complete);
}

TEST(Conformance, ListToolsResultHasResultType) {
    ListToolsResult r;
    r.tools = {};

    nlohmann::json j = r;
    EXPECT_EQ(j["resultType"], "complete");

    auto recovered = j.get<ListToolsResult>();
    EXPECT_EQ(recovered.result_type, ResultType::Complete);
}

TEST(Conformance, ListResourcesResultHasResultType) {
    ListResourcesResult r;
    r.resources = {};

    nlohmann::json j = r;
    EXPECT_EQ(j["resultType"], "complete");

    auto recovered = j.get<ListResourcesResult>();
    EXPECT_EQ(recovered.result_type, ResultType::Complete);
}

TEST(Conformance, ReadResourceResultHasResultType) {
    ReadResourceResult r;
    TextResourceContents trc;
    trc.uri = "file:///a.txt";
    trc.text = "data";
    r.contents = {ResourceContents(trc)};

    nlohmann::json j = r;
    EXPECT_EQ(j["resultType"], "complete");

    auto recovered = j.get<ReadResourceResult>();
    EXPECT_EQ(recovered.result_type, ResultType::Complete);
}

TEST(Conformance, ListPromptsResultHasResultType) {
    ListPromptsResult r;
    r.prompts = {};

    nlohmann::json j = r;
    EXPECT_EQ(j["resultType"], "complete");
}

TEST(Conformance, GetPromptResultHasResultType) {
    GetPromptResult r;
    r.messages = {};

    nlohmann::json j = r;
    EXPECT_EQ(j["resultType"], "complete");
}

TEST(Conformance, InitializeResultHasResultType) {
    InitializeResult r;
    r.protocol_version = "2026-07-28";
    r.server_info = Implementation{"s", "1"};
    r.capabilities = ServerCapabilities{};

    nlohmann::json j = r;
    EXPECT_EQ(j["resultType"], "complete");
}

TEST(Conformance, DiscoverResultHasResultType) {
    DiscoverResult r;
    r.supported_versions = {"2026-07-28"};
    r.server_info = Implementation{"s", "1"};
    r.capabilities = ServerCapabilities{};

    nlohmann::json j = r;
    EXPECT_EQ(j["resultType"], "complete");

    auto recovered = j.get<DiscoverResult>();
    EXPECT_EQ(recovered.result_type, ResultType::Complete);
}

TEST(Conformance, CompleteResultHasResultType) {
    CompleteResult r;
    r.completion = {{"values", {"a", "b"}}};

    nlohmann::json j = r;
    EXPECT_EQ(j["resultType"], "complete");
}

TEST(Conformance, ResultTypeDefaultWhenMissing) {
    nlohmann::json j = {{"tools", nlohmann::json::array()}};
    auto recovered = j.get<ListToolsResult>();
    EXPECT_EQ(recovered.result_type, ResultType::Complete);
}

// ====================================================================
// Group 7: SubscriptionFilter (5-8 tests)
// ====================================================================
TEST(Conformance, SubscriptionFilterRoundTrip) {
    SubscriptionFilter f;
    f.tools_list_changed = true;
    f.prompts_list_changed = false;
    f.resources_list_changed = true;

    nlohmann::json j = f;
    EXPECT_EQ(j["toolsListChanged"], true);
    EXPECT_EQ(j["promptsListChanged"], false);
    EXPECT_EQ(j["resourcesListChanged"], true);

    auto recovered = j.get<SubscriptionFilter>();
    EXPECT_TRUE(recovered.tools_list_changed.value_or(false));
    EXPECT_FALSE(recovered.prompts_list_changed.value_or(true));
    EXPECT_TRUE(recovered.resources_list_changed.value_or(false));
}

TEST(Conformance, SubscriptionFilterWithResourceSubscriptions) {
    SubscriptionFilter f;
    f.resource_subscriptions = {"file:///a.txt", "file:///b.txt"};

    nlohmann::json j = f;
    ASSERT_TRUE(j.contains("resourceSubscriptions"));
    EXPECT_EQ(j["resourceSubscriptions"].size(), 2);
    EXPECT_EQ(j["resourceSubscriptions"][1], "file:///b.txt");

    auto recovered = j.get<SubscriptionFilter>();
    EXPECT_EQ(recovered.resource_subscriptions.size(), 2);
}

TEST(Conformance, SubscriptionFilterEmpty) {
    SubscriptionFilter f;
    nlohmann::json j = f;
    EXPECT_TRUE(j.empty());
}

TEST(Conformance, SubscriptionsListenRequestParamsRoundTrip) {
    SubscriptionsListenRequestParams p;
    p.notifications.tools_list_changed = true;
    p.notifications.resource_subscriptions = {"file:///data.txt"};

    nlohmann::json j = p;
    EXPECT_TRUE(j["notifications"]["toolsListChanged"]);
    EXPECT_EQ(j["notifications"]["resourceSubscriptions"][0], "file:///data.txt");

    auto recovered = j.get<SubscriptionsListenRequestParams>();
    EXPECT_TRUE(recovered.notifications.tools_list_changed.value_or(false));
}

TEST(Conformance, SubscriptionsAcknowledgedRoundTrip) {
    SubscriptionsAcknowledgedNotificationParams n;
    n.notifications.prompts_list_changed = true;

    nlohmann::json j = n;
    EXPECT_TRUE(j["notifications"]["promptsListChanged"]);

    auto recovered = j.get<SubscriptionsAcknowledgedNotificationParams>();
    EXPECT_TRUE(recovered.notifications.prompts_list_changed.value_or(false));
}

TEST(Conformance, SubscriptionFilterPartialFlags) {
    SubscriptionFilter f;
    f.resources_list_changed = true;

    nlohmann::json j = f;
    EXPECT_FALSE(j.contains("toolsListChanged"));
    EXPECT_FALSE(j.contains("promptsListChanged"));
    EXPECT_TRUE(j["resourcesListChanged"]);

    auto recovered = j.get<SubscriptionFilter>();
    EXPECT_FALSE(recovered.tools_list_changed.has_value());
    EXPECT_TRUE(recovered.resources_list_changed.has_value());
}

// ====================================================================
// Group 8: Tasks (8-10 tests)
// ====================================================================
TEST(Conformance, GetTaskResultWorking) {
    GetTaskResult r;
    r.task_id = "task-1";
    r.status = "working";

    nlohmann::json j = r;
    EXPECT_EQ(j["taskId"], "task-1");
    EXPECT_EQ(j["status"], "working");
    EXPECT_EQ(j["resultType"], "task");

    auto recovered = j.get<GetTaskResult>();
    EXPECT_EQ(recovered.task_id, "task-1");
    EXPECT_EQ(recovered.status, "working");
}

TEST(Conformance, GetTaskResultCompleted) {
    GetTaskResult r;
    r.task_id = "task-2";
    r.status = "completed";
    r.result = nlohmann::json{{"output", "done"}};

    nlohmann::json j = r;
    EXPECT_EQ(j["status"], "completed");
    EXPECT_EQ(j["result"]["output"], "done");

    auto recovered = j.get<GetTaskResult>();
    EXPECT_EQ(recovered.status, "completed");
    ASSERT_TRUE(recovered.result.has_value());
}

TEST(Conformance, GetTaskResultFailed) {
    GetTaskResult r;
    r.task_id = "task-3";
    r.status = "failed";
    r.error_message = "something went wrong";

    nlohmann::json j = r;
    EXPECT_EQ(j["status"], "failed");
    EXPECT_EQ(j["errorMessage"], "something went wrong");

    auto recovered = j.get<GetTaskResult>();
    EXPECT_EQ(recovered.status, "failed");
    ASSERT_TRUE(recovered.error_message.has_value());
    EXPECT_EQ(*recovered.error_message, "something went wrong");
}

TEST(Conformance, GetTaskResultCancelled) {
    GetTaskResult r;
    r.task_id = "task-4";
    r.status = "cancelled";

    nlohmann::json j = r;
    EXPECT_EQ(j["status"], "cancelled");
}

TEST(Conformance, GetTaskResultInputRequired) {
    GetTaskResult r;
    r.task_id = "task-5";
    r.status = "working";
    r.input_required = nlohmann::json{{"fields", {"name"}}};

    nlohmann::json j = r;
    ASSERT_TRUE(j.contains("inputRequired"));
    EXPECT_EQ(j["inputRequired"]["fields"][0], "name");
}

TEST(Conformance, UpdateTaskResultRoundTrip) {
    UpdateTaskResult r;
    nlohmann::json j = r;
    EXPECT_TRUE(j.contains("resultType"));

    auto recovered = j.get<UpdateTaskResult>();
    EXPECT_EQ(recovered.result_type, ResultType::Complete);
}

TEST(Conformance, CancelTaskResultRoundTrip) {
    CancelTaskResult r;
    nlohmann::json j = r;
    EXPECT_TRUE(j.contains("resultType"));

    auto recovered = j.get<CancelTaskResult>();
    EXPECT_EQ(recovered.result_type, ResultType::Complete);
}

TEST(Conformance, GetTaskRequestParamsRoundTrip) {
    GetTaskRequestParams p;
    p.task_id = "task-1";

    nlohmann::json j = p;
    EXPECT_EQ(j["task_id"], "task-1");

    auto recovered = j.get<GetTaskRequestParams>();
    EXPECT_EQ(recovered.task_id, "task-1");
}

TEST(Conformance, UpdateTaskRequestParamsRoundTrip) {
    UpdateTaskRequestParams p;
    p.task_id = "task-2";
    p.result = nlohmann::json{{"progress", 0.5}};

    nlohmann::json j = p;
    EXPECT_EQ(j["taskId"], "task-2");
    EXPECT_EQ(j["result"]["progress"], 0.5);

    auto recovered = j.get<UpdateTaskRequestParams>();
    EXPECT_EQ(recovered.task_id, "task-2");
}

TEST(Conformance, CancelTaskRequestParamsRoundTrip) {
    CancelTaskRequestParams p;
    p.task_id = "task-3";
    p.reason = "no longer needed";

    nlohmann::json j = p;
    EXPECT_EQ(j["taskId"], "task-3");
    EXPECT_EQ(j["reason"], "no longer needed");

    auto recovered = j.get<CancelTaskRequestParams>();
    EXPECT_EQ(recovered.task_id, "task-3");
    ASSERT_TRUE(recovered.reason.has_value());
    EXPECT_EQ(*recovered.reason, "no longer needed");
}

// ====================================================================
// Group 9: Logging (5-8 tests)
// ====================================================================
TEST(Conformance, LoggingLevelDebug) {
    LoggingMessageNotificationParams p;
    p.level = LoggingLevel::Debug;
    p.logger = "test";
    p.data = "debug message";

    nlohmann::json j = p;
    EXPECT_EQ(j["level"], "debug");
    EXPECT_EQ(j["logger"], "test");
    EXPECT_EQ(j["data"], "debug message");

    auto recovered = j.get<LoggingMessageNotificationParams>();
    EXPECT_EQ(recovered.level, LoggingLevel::Debug);
}

TEST(Conformance, LoggingLevelInfo) {
    LoggingMessageNotificationParams p;
    p.level = LoggingLevel::Info;
    p.logger = "app";
    p.data = nlohmann::json{{"event", "started"}};

    nlohmann::json j = p;
    EXPECT_EQ(j["level"], "info");
}

TEST(Conformance, LoggingLevelWarning) {
    LoggingMessageNotificationParams p;
    p.level = LoggingLevel::Warning;
    p.logger = "app";
    p.data = "warning";

    nlohmann::json j = p;
    EXPECT_EQ(j["level"], "warning");

    auto recovered = j.get<LoggingMessageNotificationParams>();
    EXPECT_EQ(recovered.level, LoggingLevel::Warning);
}

TEST(Conformance, LoggingLevelErrorCritical) {
    LoggingMessageNotificationParams p;
    p.level = LoggingLevel::Error;
    p.logger = "sys";
    p.data = "error";

    nlohmann::json j = p;
    EXPECT_EQ(j["level"], "error");

    p.level = LoggingLevel::Critical;
    j = p;
    EXPECT_EQ(j["level"], "critical");

    auto recovered = j.get<LoggingMessageNotificationParams>();
    EXPECT_EQ(recovered.level, LoggingLevel::Critical);
}

TEST(Conformance, LoggingLevelAllValues) {
    auto check_level = [](LoggingLevel level, const char* expected) {
        LoggingMessageNotificationParams p;
        p.level = level;
        p.logger = "t";
        p.data = "x";
        nlohmann::json j = p;
        EXPECT_STREQ(j["level"].get<std::string>().c_str(), expected);
    };

    check_level(LoggingLevel::Debug, "debug");
    check_level(LoggingLevel::Info, "info");
    check_level(LoggingLevel::Notice, "notice");
    check_level(LoggingLevel::Warning, "warning");
    check_level(LoggingLevel::Error, "error");
    check_level(LoggingLevel::Critical, "critical");
    check_level(LoggingLevel::Alert, "alert");
    check_level(LoggingLevel::Emergency, "emergency");
}

TEST(Conformance, SetLevelRequestParamsRoundTrip) {
    SetLevelRequestParams p;
    p.level = LoggingLevel::Warning;

    nlohmann::json j = p;
    EXPECT_EQ(j["level"], "warning");

    auto recovered = j.get<SetLevelRequestParams>();
    EXPECT_EQ(recovered.level, LoggingLevel::Warning);
}

TEST(Conformance, SetLevelRequestParamsAllLevels) {
    auto check = [](LoggingLevel level) {
        SetLevelRequestParams p;
        p.level = level;
        nlohmann::json j = p;
        auto recovered = j.get<SetLevelRequestParams>();
        EXPECT_EQ(recovered.level, level);
    };

    check(LoggingLevel::Debug);
    check(LoggingLevel::Info);
    check(LoggingLevel::Notice);
    check(LoggingLevel::Warning);
    check(LoggingLevel::Error);
    check(LoggingLevel::Critical);
    check(LoggingLevel::Alert);
    check(LoggingLevel::Emergency);
}
