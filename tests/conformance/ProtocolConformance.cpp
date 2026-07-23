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
    req.params = JsonValue(JsonValue::object_tag);

    auto json_str = SerializeMessage(JsonRpcMessage(req));
    EXPECT_NE(json_str.find("\"method\":\"tools/list\""), std::string::npos);
    EXPECT_NE(json_str.find("\"jsonrpc\":\"2.0\""), std::string::npos);
}

TEST(Conformance, JsonRpcNotificationRoundTrip) {
    JsonRpcNotification notif;
    notif.method = "notifications/initialized";

    auto json_str = SerializeMessage(JsonRpcMessage(notif));
    EXPECT_EQ(json_str.find("\"id\""), std::string::npos);
    EXPECT_NE(json_str.find("\"method\":\"notifications/initialized\""), std::string::npos);
}

TEST(Conformance, JsonRpcResponseRoundTrip) {
    JsonRpcResponse resp;
    resp.id = RequestId{int64_t(42)};
    resp.result = JsonValue(JsonValue::object_tag);
    resp.result["ok"] = true;

    auto json_str = SerializeMessage(JsonRpcMessage(resp));
    EXPECT_NE(json_str.find("\"id\":42"), std::string::npos);
    EXPECT_NE(json_str.find("\"ok\":true"), std::string::npos);
}

TEST(Conformance, JsonRpcErrorResponseRoundTrip) {
    JsonRpcErrorResponse err;
    err.id = RequestId{std::string("req-1")};
    err.error = {McpErrorCode::InvalidRequest, "bad request"};

    auto json_str = SerializeMessage(JsonRpcMessage(err));
    EXPECT_NE(json_str.find("\"code\":-32600"), std::string::npos);
    EXPECT_NE(json_str.find("\"message\":\"bad request\""), std::string::npos);
}

TEST(Conformance, JsonRpcMessageVariantDispatch) {
    JsonRpcRequest req;
    req.id = RequestId{int64_t(1)};
    req.method = "ping";
    req.params = JsonValue(JsonValue::object_tag);
    EXPECT_TRUE(IsRequest(JsonRpcMessage(req)));

    JsonRpcResponse resp;
    resp.id = RequestId{int64_t(1)};
    resp.result = JsonValue(JsonValue::object_tag);
    EXPECT_TRUE(IsResponse(JsonRpcMessage(resp)));

    JsonRpcErrorResponse err;
    err.id = RequestId{int64_t(1)};
    err.error = {McpErrorCode::MethodNotFound, "not found"};
    EXPECT_TRUE(IsError(JsonRpcMessage(err)));

    JsonRpcNotification notif;
    notif.method = "notifications/initialized";
    EXPECT_TRUE(IsNotification(JsonRpcMessage(notif)));
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
    JsonValue body(JsonValue::object_tag);
    body["name"] = "test";

    EXPECT_EQ(codec->ValidateRequest("tools/call", body),
              WireValidation::Invalid);
    EXPECT_EQ(codec->ValidateRequest("server/discover", body),
              WireValidation::Ok);
}

TEST(Conformance, WireCodec2025StampIsNoop) {
    auto codec = MakeWireCodec("2025-11-25");
    JsonValue body(JsonValue::object_tag);
    RequestMeta meta;
    meta.protocol_version = "2025-11-25";
    codec->StampOutgoingRequest(body, meta);
    EXPECT_TRUE(body.Empty());
}

TEST(Conformance, WireCodec2026StampAddsMeta) {
    auto codec = MakeWireCodec("2026-07-28");
    JsonValue body(JsonValue::object_tag);
    RequestMeta meta;
    meta.protocol_version = "2026-07-28";
    meta.client_info = Implementation{"test", "1.0"};
    codec->StampOutgoingRequest(body, meta);

    ASSERT_TRUE(body.Contains("_meta"));
    EXPECT_EQ(body["_meta"]["io.modelcontextprotocol/protocolVersion"].GetString(),
              "2026-07-28");
    EXPECT_EQ(body["_meta"]["io.modelcontextprotocol/clientInfo"]["name"].GetString(),
              "test");
}

TEST(Conformance, WireCodec2026EncodeResult) {
    auto codec = MakeWireCodec("2026-07-28");
    JsonValue result(JsonValue::object_tag);
    result["content"] = JsonValue(JsonValue::array_tag);
    auto encoded = codec->EncodeResult("tools/call", result);
    EXPECT_EQ(encoded["resultType"].GetString(), "complete");
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

    auto jv = SerializeDiscoverResult(r);
    auto recovered = DeserializeDiscoverResult(jv);

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

    auto jv = SerializeInitializeResult(r);
    auto recovered = DeserializeInitializeResult(jv);

    EXPECT_EQ(recovered.protocol_version, "2026-07-28");
    EXPECT_TRUE(recovered.capabilities.tools.has_value());
}

TEST(Conformance, DiscoverRequestSerialization) {
    DiscoverRequestParams params;
    auto jv = SerializeDiscoverRequestParams(params);
    EXPECT_TRUE(jv.IsObject());
    EXPECT_TRUE(jv.Empty());
}

TEST(Conformance, InitializeRequestSerialization) {
    InitializeRequestParams params;
    params.protocol_version = "2026-07-28";
    params.capabilities = ClientCapabilities{};
    params.client_info = Implementation{"client", "1.0"};

    auto jv = SerializeInitializeRequestParams(params);
    EXPECT_EQ(jv["protocolVersion"].GetString(), "2026-07-28");
    EXPECT_EQ(jv["clientInfo"]["name"].GetString(), "client");
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
    JsonRpcErrorResponse err;
    err.id = RequestId{int64_t(1)};
    err.error = ed;
    auto json_str = SerializeMessage(JsonRpcMessage(err));
    EXPECT_NE(json_str.find("\"code\":-32601"), std::string::npos);
    EXPECT_NE(json_str.find("\"message\":\"method not found\""), std::string::npos);
}

TEST(Conformance, ErrorCodeWithData) {
    ErrorData ed;
    ed.code = McpErrorCode::InvalidParams;
    ed.message = "bad param";
    ed.data = JsonValue(JsonValue::object_tag);
    (*ed.data)["param"] = "x";

    JsonRpcErrorResponse err;
    err.id = RequestId{int64_t(1)};
    err.error = ed;
    auto json_str = SerializeMessage(JsonRpcMessage(err));
    EXPECT_NE(json_str.find("\"code\":-32602"), std::string::npos);
    EXPECT_NE(json_str.find("\"param\":\"x\""), std::string::npos);
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
    t.input_schema = JsonValue(JsonValue::object_tag);
    t.input_schema["type"] = "object";
    t.input_schema["properties"] = JsonValue(JsonValue::object_tag);

    auto jv = SerializeTool(t);
    EXPECT_EQ(jv["name"].GetString(), "echo");
    EXPECT_EQ(jv["description"].GetString(), "Echo input back");
    EXPECT_EQ(jv["inputSchema"]["type"].GetString(), "object");

    auto recovered = DeserializeTool(jv);
    EXPECT_EQ(recovered.name, "echo");
}

TEST(Conformance, ToolWithAnnotations) {
    Tool t;
    t.name = "read-only-tool";
    t.input_schema = JsonValue(JsonValue::object_tag);
    t.annotations = ToolAnnotations{};
    t.annotations->read_only_hint = true;
    t.annotations->idempotent_hint = true;

    auto jv = SerializeTool(t);
    EXPECT_EQ(jv["annotations"]["readOnlyHint"].GetBool(), true);
    EXPECT_EQ(jv["annotations"]["idempotentHint"].GetBool(), true);
    EXPECT_FALSE(jv["annotations"].Contains("openWorldHint"));

    auto recovered = DeserializeTool(jv);
    ASSERT_TRUE(recovered.annotations.has_value());
    EXPECT_TRUE(recovered.annotations->read_only_hint.value_or(false));
}

TEST(Conformance, ToolWithIcons) {
    Tool t;
    t.name = "icon-tool";
    t.input_schema = JsonValue(JsonValue::object_tag);
    t.icons = {Icon{"https://example.com/icon.svg", "image/svg+xml"}};

    auto jv = SerializeTool(t);
    ASSERT_TRUE(jv.Contains("icons"));
    EXPECT_EQ(jv["icons"][0]["src"].GetString(), "https://example.com/icon.svg");
}

TEST(Conformance, ResourceSerialization) {
    Resource r;
    r.uri = "file:///data.txt";
    r.name = "Data";
    r.mime_type = "text/plain";

    auto jv = SerializeResource(r);
    EXPECT_EQ(jv["uri"].GetString(), "file:///data.txt");
    EXPECT_EQ(jv["name"].GetString(), "Data");
    EXPECT_EQ(jv["mimeType"].GetString(), "text/plain");

    auto recovered = DeserializeResource(jv);
    EXPECT_EQ(recovered.uri, "file:///data.txt");
    EXPECT_EQ(recovered.name, "Data");
}

TEST(Conformance, ResourceTemplateSerialization) {
    ResourceTemplate rt;
    rt.uri_template = "file:///{path}";
    rt.name = "Dynamic Files";

    auto jv = SerializeResourceTemplate(rt);
    EXPECT_EQ(jv["uriTemplate"].GetString(), "file:///{path}");

    auto recovered = DeserializeResourceTemplate(jv);
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

    auto jv = SerializePrompt(p);
    EXPECT_EQ(jv["name"].GetString(), "greet");
    ASSERT_TRUE(jv.Contains("arguments"));
    EXPECT_EQ(jv["arguments"][0]["name"].GetString(), "name");
    EXPECT_EQ(jv["arguments"][0]["required"].GetBool(), true);

    auto recovered = DeserializePrompt(jv);
    EXPECT_EQ(recovered.name, "greet");
    ASSERT_TRUE(recovered.arguments.has_value());
    EXPECT_TRUE(recovered.arguments->at(0).required.value_or(false));
}

TEST(Conformance, PromptMessageSerialization) {
    PromptMessage pm;
    pm.role = "user";
    pm.content = TextContent{"text", "Hello"};

    auto jv = SerializePromptMessage(pm);
    EXPECT_EQ(jv["role"].GetString(), "user");
    EXPECT_EQ(jv["content"]["type"].GetString(), "text");
    EXPECT_EQ(jv["content"]["text"].GetString(), "Hello");

    auto recovered = DeserializePromptMessage(jv);
    EXPECT_EQ(recovered.role, "user");
    EXPECT_TRUE(std::holds_alternative<TextContent>(recovered.content));
}

// ====================================================================
// Content variant serialization
// ====================================================================
TEST(Conformance, ContentVariantText) {
    ContentVariant cv = TextContent{"text", "hello"};
    auto jv = SerializeContentVariant(cv);
    EXPECT_EQ(jv["type"].GetString(), "text");
    EXPECT_EQ(jv["text"].GetString(), "hello");
}

TEST(Conformance, ContentVariantImage) {
    ContentVariant cv = ImageContent{"image", "base64data", "image/png"};
    auto jv = SerializeContentVariant(cv);
    EXPECT_EQ(jv["type"].GetString(), "image");
}

TEST(Conformance, ContentVariantAudio) {
    ContentVariant cv = AudioContent{"audio", "base64data", "audio/wav"};
    auto jv = SerializeContentVariant(cv);
    EXPECT_EQ(jv["type"].GetString(), "audio");
}

TEST(Conformance, ContentVariantEmbeddedResource) {
    TextResourceContents trc;
    trc.uri = "file:///doc.txt";
    trc.text = "embedded";
    EmbeddedResource er;
    er.resource = ResourceContents(trc);

    ContentVariant cv = er;
    auto jv = SerializeContentVariant(cv);
    EXPECT_EQ(jv["type"].GetString(), "resource");
    EXPECT_EQ(jv["resource"]["text"].GetString(), "embedded");
}

// ====================================================================
// Implementation / ServerInfo
// ====================================================================
TEST(Conformance, ImplementationRoundTrip) {
    Implementation impl;
    impl.name = "test-server";
    impl.version = "1.0.0";

    auto jv = SerializeImplementation(impl);
    auto recovered = DeserializeImplementation(jv);
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
    r.tools[0].input_schema = JsonValue(JsonValue::object_tag);
    r.next_cursor = "next-page";

    auto jv = SerializeListToolsResult(r);
    EXPECT_EQ(jv["nextCursor"].GetString(), "next-page");
}

TEST(Conformance, ListResourcesResultRoundTrip) {
    ListResourcesResult r;
    Resource res;
    res.uri = "file:///a.txt";
    res.name = "A";
    r.resources = {res};

    auto jv = SerializeListResourcesResult(r);
    auto recovered = DeserializeListResourcesResult(jv);
    ASSERT_EQ(recovered.resources.size(), 1);
    EXPECT_EQ(recovered.resources[0].name, "A");
}

TEST(Conformance, ListPromptsResultRoundTrip) {
    ListPromptsResult r;
    Prompt p;
    p.name = "test-prompt";
    r.prompts = {p};

    auto jv = SerializeListPromptsResult(r);
    auto recovered = DeserializeListPromptsResult(jv);
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

    auto jv = SerializeServerCapabilities(caps);
    EXPECT_TRUE(jv["tools"]["listChanged"].GetBool());
    EXPECT_TRUE(jv["resources"]["subscribe"].GetBool());
    EXPECT_FALSE(jv["resources"]["listChanged"].GetBool());
    EXPECT_TRUE(jv["prompts"].IsObject());

    auto recovered = DeserializeServerCapabilities(jv);
    EXPECT_TRUE(recovered.tools.has_value());
    EXPECT_TRUE(recovered.resources.has_value());
}

TEST(Conformance, ClientCapabilitiesRoundTrip) {
    ClientCapabilities caps;
    caps.roots = RootsCapability{};
    caps.elicitation = ElicitationCapability{};
    caps.elicitation->form = JsonValue(JsonValue::object_tag);

    auto jv = SerializeClientCapabilities(caps);
    EXPECT_TRUE(jv.Contains("roots"));
    EXPECT_TRUE(jv.Contains("elicitation"));

    auto recovered = DeserializeClientCapabilities(jv);
    EXPECT_TRUE(recovered.roots.has_value());
    EXPECT_TRUE(recovered.elicitation.has_value());
}

// ====================================================================
// CallToolResult with structured content
// ====================================================================
TEST(Conformance, CallToolResultWithStructuredContent) {
    CallToolResult r;
    r.content = {TextContent{"text", "result"}};
    r.structured_content = JsonValue(JsonValue::object_tag);
    (*r.structured_content)["key"] = "value";
    r.is_error = false;

    auto jv = SerializeCallToolResult(r);
    EXPECT_EQ(jv["structuredContent"]["key"].GetString(), "value");
    EXPECT_FALSE(jv["isError"].GetBool());

    auto recovered = DeserializeCallToolResult(jv);
    EXPECT_TRUE(recovered.structured_content.has_value());
    EXPECT_EQ((*recovered.structured_content)["key"].GetString(), "value");
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

    auto jv = SerializeRequestMeta(m);
    EXPECT_EQ(jv["io.modelcontextprotocol/protocolVersion"].GetString(), "2026-07-28");
    EXPECT_EQ(jv["io.modelcontextprotocol/clientInfo"]["name"].GetString(), "client");

    auto recovered = DeserializeRequestMeta(jv);
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

    auto jv = SerializeElicitRequestParams(p);
    EXPECT_EQ(jv["message"].GetString(), "What is your name?");
    EXPECT_FALSE(jv.Contains("requestedSchema"));

    auto recovered = DeserializeElicitRequestParams(jv);
    EXPECT_EQ(recovered.message, "What is your name?");
    EXPECT_FALSE(recovered.requested_schema.has_value());
}

TEST(Conformance, ElicitRequestParamsWithSchemaRoundTrip) {
    ElicitRequestParams p;
    p.message = "Enter your age";
    p.requested_schema = JsonValue(JsonValue::object_tag);
    (*p.requested_schema)["type"] = "object";
    (*p.requested_schema)["properties"] = JsonValue(JsonValue::object_tag);
    (*p.requested_schema)["properties"]["age"] = JsonValue(JsonValue::object_tag);
    (*p.requested_schema)["properties"]["age"]["type"] = "number";

    auto jv = SerializeElicitRequestParams(p);
    EXPECT_EQ(jv["message"].GetString(), "Enter your age");
    EXPECT_EQ(jv["requestedSchema"]["type"].GetString(), "object");
    EXPECT_EQ(jv["requestedSchema"]["properties"]["age"]["type"].GetString(), "number");

    auto recovered = DeserializeElicitRequestParams(jv);
    EXPECT_TRUE(recovered.requested_schema.has_value());
    EXPECT_EQ((*recovered.requested_schema)["properties"]["age"]["type"].GetString(), "number");
}

TEST(Conformance, ElicitResultAcceptRoundTrip) {
    ElicitResult r;
    r.values = JsonValue(JsonValue::object_tag);
    (*r.values)["name"] = "Alice";
    r.result_type = ResultType::Complete;

    auto jv = SerializeElicitResult(r);
    EXPECT_EQ(jv["values"]["name"].GetString(), "Alice");
    EXPECT_EQ(jv["resultType"].GetString(), "complete");

    auto recovered = DeserializeElicitResult(jv);
    EXPECT_TRUE(recovered.values.has_value());
    EXPECT_EQ((*recovered.values)["name"].GetString(), "Alice");
    EXPECT_EQ(recovered.result_type, ResultType::Complete);
}

TEST(Conformance, ElicitResultDeclineRoundTrip) {
    ElicitResult r;
    r.result_type = ResultType::InputRequired;

    auto jv = SerializeElicitResult(r);
    EXPECT_EQ(jv["resultType"].GetString(), "input_required");

    auto recovered = DeserializeElicitResult(jv);
    EXPECT_FALSE(recovered.values.has_value());
    EXPECT_EQ(recovered.result_type, ResultType::InputRequired);
}

TEST(Conformance, ElicitResultWithMeta) {
    ElicitResult r;
    r.values = JsonValue(JsonValue::object_tag);
    (*r.values)["ok"] = true;
    r.meta = JsonValue(JsonValue::object_tag);
    (*r.meta)["trace"] = "abc";

    auto jv = SerializeElicitResult(r);
    EXPECT_EQ(jv["_meta"]["trace"].GetString(), "abc");

    auto recovered = DeserializeElicitResult(jv);
    ASSERT_TRUE(recovered.meta.has_value());
    EXPECT_EQ((*recovered.meta)["trace"].GetString(), "abc");
}

TEST(Conformance, ElicitResultTypedAccept) {
    ElicitResultTyped<std::string> typed;
    typed.action = "accept";
    typed.content = "Alice";

    EXPECT_TRUE(typed.is_accepted());
    ASSERT_TRUE(typed.content.has_value());
    EXPECT_EQ(*typed.content, "Alice");
}

TEST(Conformance, ElicitResultTypedDecline) {
    ElicitResultTyped<int> typed;
    typed.action = "decline";

    EXPECT_FALSE(typed.is_accepted());
    EXPECT_FALSE(typed.content.has_value());
}

TEST(Conformance, ElicitResultTypedCancel) {
    ElicitResultTyped<JsonValue> typed;
    EXPECT_EQ(typed.action, "cancel");
}

TEST(Conformance, ElicitMethodConstant) {
    EXPECT_EQ(methods::kElicit, "elicitation/create");
}

TEST(Conformance, MakeInputRequestForElicitation) {
    ElicitRequestParams params;
    params.message = "confirm?";
    auto jv = MakeInputRequestForElicitation(params);

    EXPECT_EQ(jv["method"].GetString(), "elicitation/create");
    EXPECT_EQ(jv["params"]["message"].GetString(), "confirm?");
}

TEST(Conformance, MakeInputResponseFromElicitResult) {
    ElicitResult result;
    result.values = JsonValue(JsonValue::object_tag);
    (*result.values)["ok"] = true;
    result.result_type = ResultType::Complete;

    auto jv = MakeInputResponseFromElicitResult(result);
    EXPECT_EQ(jv["values"]["ok"].GetBool(), true);
    EXPECT_EQ(jv["resultType"].GetString(), "complete");
}

TEST(Conformance, ElicitRequestParamsEmptyMessage) {
    ElicitRequestParams p;
    p.message = "";

    auto jv = SerializeElicitRequestParams(p);
    EXPECT_EQ(jv["message"].GetString(), "");

    auto recovered = DeserializeElicitRequestParams(jv);
    EXPECT_TRUE(recovered.message.empty());
}

TEST(Conformance, ElicitResultTypedJsonValue) {
    ElicitResultTyped<JsonValue> typed;
    typed.action = "accept";
    typed.content = JsonValue(JsonValue::object_tag);
    (*typed.content)["nested"] = JsonValue(JsonValue::object_tag);
    (*typed.content)["nested"]["value"] = 42;

    ASSERT_TRUE(typed.content.has_value());
    EXPECT_EQ((*typed.content)["nested"]["value"], JsonValue(42));
}

// ====================================================================
// Group 2: MRTR / InputRequired (10-15 tests)
// ====================================================================
TEST(Conformance, InputRequiredResultRoundTrip) {
    InputRequiredResult ir;
    ir.input_requests.elicit = InputRequestElicit{"provide value"};
    ir.request_state = "state-abc";

    auto jv = SerializeInputRequiredResult(ir);
    EXPECT_EQ(jv["resultType"].GetString(), "input_required");
    EXPECT_EQ(jv["inputRequests"]["elicit"]["message"].GetString(), "provide value");
    EXPECT_EQ(jv["requestState"].GetString(), "state-abc");

    auto recovered = DeserializeInputRequiredResult(jv);
    EXPECT_TRUE(recovered.input_requests.elicit.has_value());
    EXPECT_EQ(recovered.input_requests.elicit->message, "provide value");
    EXPECT_TRUE(recovered.request_state.has_value());
    EXPECT_EQ(*recovered.request_state, "state-abc");
}

TEST(Conformance, InputRequiredResultWithConfirm) {
    InputRequiredResult ir;
    ir.input_requests.confirm = InputRequestElicit{"Are you sure?"};

    auto jv = SerializeInputRequiredResult(ir);
    EXPECT_EQ(jv["inputRequests"]["confirm"]["message"].GetString(), "Are you sure?");

    auto recovered = DeserializeInputRequiredResult(jv);
    EXPECT_TRUE(recovered.input_requests.confirm.has_value());
    EXPECT_EQ(recovered.input_requests.confirm->message, "Are you sure?");
}

TEST(Conformance, InputRequiredResultBothRequests) {
    InputRequiredResult ir;
    ir.input_requests.confirm = InputRequestElicit{"Confirm?"};
    ir.input_requests.elicit = InputRequestElicit{"Provide details"};

    auto jv = SerializeInputRequiredResult(ir);
    EXPECT_TRUE(jv["inputRequests"].Contains("confirm"));
    EXPECT_TRUE(jv["inputRequests"].Contains("elicit"));

    auto recovered = DeserializeInputRequiredResult(jv);
    EXPECT_TRUE(recovered.input_requests.confirm.has_value());
    EXPECT_TRUE(recovered.input_requests.elicit.has_value());
}

TEST(Conformance, InputRequestElicitWithSchema) {
    InputRequestElicit elicit;
    elicit.message = "pick one";
    elicit.requested_schema = JsonValue(JsonValue::object_tag);
    (*elicit.requested_schema)["enum"] = JsonValue(JsonValue::array_tag);
    (*elicit.requested_schema)["enum"].PushBack("a");
    (*elicit.requested_schema)["enum"].PushBack("b");
    (*elicit.requested_schema)["enum"].PushBack("c");

    auto jv = SerializeInputRequestElicit(elicit);
    EXPECT_EQ(jv["message"].GetString(), "pick one");
    EXPECT_EQ(jv["requestedSchema"]["enum"][0].GetString(), "a");

    auto recovered = DeserializeInputRequestElicit(jv);
    EXPECT_TRUE(recovered.requested_schema.has_value());
}

TEST(Conformance, IsInputRequiredResultTrue) {
    JsonValue jv(JsonValue::object_tag);
    jv["resultType"] = "input_required";
    jv["inputRequests"] = JsonValue(JsonValue::object_tag);
    EXPECT_TRUE(IsInputRequiredResult(jv));
}

TEST(Conformance, IsInputRequiredResultFalse) {
    JsonValue jv(JsonValue::object_tag);
    jv["resultType"] = "complete";
    EXPECT_FALSE(IsInputRequiredResult(jv));
}

TEST(Conformance, IsInputRequiredResultMissingKey) {
    JsonValue jv(JsonValue::object_tag);
    jv["ok"] = true;
    EXPECT_FALSE(IsInputRequiredResult(jv));
}

TEST(Conformance, ExtractInputRequestsFound) {
    JsonValue jv(JsonValue::object_tag);
    jv["inputRequests"] = JsonValue(JsonValue::object_tag);
    jv["inputRequests"]["elicit"] = JsonValue(JsonValue::object_tag);
    jv["inputRequests"]["elicit"]["message"] = "enter value";
    auto extracted = ExtractInputRequests(jv);
    ASSERT_TRUE(extracted.has_value());
    ASSERT_TRUE(extracted->elicit.has_value());
    EXPECT_EQ(extracted->elicit->message, "enter value");
}

TEST(Conformance, ExtractInputRequestsNotFound) {
    JsonValue jv(JsonValue::object_tag);
    jv["result"] = "ok";
    auto extracted = ExtractInputRequests(jv);
    EXPECT_FALSE(extracted.has_value());
}

TEST(Conformance, InputRequestsEmpty) {
    InputRequests ir;
    auto jv = SerializeInputRequests(ir);
    EXPECT_TRUE(jv.Empty());

    auto recovered = DeserializeInputRequests(jv);
    EXPECT_FALSE(recovered.confirm.has_value());
    EXPECT_FALSE(recovered.elicit.has_value());
}

TEST(Conformance, CallToolRequestWithInputResponses) {
    CallToolRequestParams p;
    p.name = "test";
    p.input_responses = JsonValue(JsonValue::object_tag);
    (*p.input_responses)["values"] = JsonValue(JsonValue::object_tag);
    (*p.input_responses)["values"]["key"] = "val";
    p.request_state = "state-xyz";

    auto jv = SerializeCallToolRequestParams(p);
    EXPECT_EQ(jv["inputResponses"]["values"]["key"].GetString(), "val");
    EXPECT_EQ(jv["requestState"].GetString(), "state-xyz");

    auto recovered = DeserializeCallToolRequestParams(jv);
    ASSERT_TRUE(recovered.input_responses.has_value());
    ASSERT_TRUE(recovered.request_state.has_value());
    EXPECT_EQ(*recovered.request_state, "state-xyz");
}

// ====================================================================
// Group 3: Elicitation Schema types as JSON (8-10 tests)
// ====================================================================
TEST(Conformance, SchemaStringTypeRoundTrip) {
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = "string";
    schema["minLength"] = int64_t(1);
    schema["maxLength"] = int64_t(100);
    schema["format"] = "email";

    EXPECT_EQ(schema["type"].GetString(), "string");
    EXPECT_EQ(schema["minLength"].GetInt(), 1);
    EXPECT_EQ(schema["maxLength"].GetInt(), 100);
    EXPECT_EQ(schema["format"].GetString(), "email");

    ElicitRequestParams p;
    p.message = "Enter email";
    p.requested_schema = schema;

    auto jv = SerializeElicitRequestParams(p);
    EXPECT_EQ(jv["requestedSchema"]["minLength"].GetInt(), 1);
    EXPECT_EQ(jv["requestedSchema"]["format"].GetString(), "email");

    auto recovered = DeserializeElicitRequestParams(jv);
    ASSERT_TRUE(recovered.requested_schema.has_value());
    EXPECT_EQ((*recovered.requested_schema)["format"].GetString(), "email");
}

TEST(Conformance, SchemaNumberTypeRoundTrip) {
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = "number";
    schema["minimum"] = int64_t(0);
    schema["maximum"] = int64_t(150);

    EXPECT_EQ(schema["minimum"].GetInt(), 0);
    EXPECT_EQ(schema["maximum"].GetInt(), 150);

    ElicitRequestParams p;
    p.message = "Enter age";
    p.requested_schema = schema;

    auto recovered = p.requested_schema;
    EXPECT_EQ((*recovered)["minimum"].GetInt(), 0);
}

TEST(Conformance, SchemaBooleanTypeRoundTrip) {
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = "boolean";
    schema["default"] = true;

    EXPECT_EQ(schema["type"].GetString(), "boolean");
    EXPECT_EQ(schema["default"].GetBool(), true);
}

TEST(Conformance, SchemaEnumTypeRoundTrip) {
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = "string";
    schema["enum"] = JsonValue(JsonValue::array_tag);
    schema["enum"].PushBack("red");
    schema["enum"].PushBack("green");
    schema["enum"].PushBack("blue");

    EXPECT_EQ(schema["enum"].Size(), 3);
    EXPECT_EQ(schema["enum"][1].GetString(), "green");
}

TEST(Conformance, SchemaSingleSelectEnumUntitled) {
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = "string";
    schema["enum"] = JsonValue(JsonValue::array_tag);
    schema["enum"].PushBack("option1");
    schema["enum"].PushBack("option2");

    ElicitRequestParams p;
    p.message = "Choose one";
    p.requested_schema = schema;

    auto jv = SerializeElicitRequestParams(p);
    EXPECT_EQ(jv["requestedSchema"]["enum"].Size(), 2);
    EXPECT_FALSE(jv["requestedSchema"].Contains("title"));
}

TEST(Conformance, SchemaSingleSelectEnumTitled) {
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = "string";
    schema["enum"] = JsonValue(JsonValue::array_tag);
    schema["enum"].PushBack("yes");
    schema["enum"].PushBack("no");
    schema["title"] = "Confirmation";

    ElicitRequestParams p;
    p.message = "Confirm?";
    p.requested_schema = schema;

    auto jv = SerializeElicitRequestParams(p);
    EXPECT_EQ(jv["requestedSchema"]["title"].GetString(), "Confirmation");
    EXPECT_EQ(jv["requestedSchema"]["enum"][0].GetString(), "yes");
}

TEST(Conformance, SchemaMultiSelectUntitled) {
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = "array";
    schema["items"] = JsonValue(JsonValue::object_tag);
    schema["items"]["type"] = "string";
    schema["items"]["enum"] = JsonValue(JsonValue::array_tag);
    schema["items"]["enum"].PushBack("a");
    schema["items"]["enum"].PushBack("b");
    schema["items"]["enum"].PushBack("c");

    EXPECT_EQ(schema["items"]["enum"].Size(), 3);
}

TEST(Conformance, SchemaMultiSelectTitled) {
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = "array";
    schema["items"] = JsonValue(JsonValue::object_tag);
    schema["items"]["type"] = "string";
    schema["items"]["enum"] = JsonValue(JsonValue::array_tag);
    schema["items"]["enum"].PushBack("x");
    schema["items"]["enum"].PushBack("y");
    schema["title"] = "Pick multiple";

    ElicitRequestParams p;
    p.message = "Select";
    p.requested_schema = schema;

    auto jv = SerializeElicitRequestParams(p);
    EXPECT_EQ(jv["requestedSchema"]["title"].GetString(), "Pick multiple");
}

// ====================================================================
// Group 4: Extensions Capability (5-8 tests)
// ====================================================================
TEST(Conformance, ExtensionsCapabilityServerRoundTrip) {
    ServerCapabilities caps;
    caps.extensions = std::map<std::string, JsonValue>{};

    auto jv = SerializeServerCapabilities(caps);
    EXPECT_TRUE(jv.Contains("extensions"));
    EXPECT_TRUE(jv["extensions"].IsObject());

    auto recovered = DeserializeServerCapabilities(jv);
    EXPECT_TRUE(recovered.extensions.has_value());
}

TEST(Conformance, ExtensionsCapabilityClientRoundTrip) {
    ClientCapabilities caps;
    caps.extensions = std::map<std::string, JsonValue>{};

    auto jv = SerializeClientCapabilities(caps);
    EXPECT_TRUE(jv.Contains("extensions"));

    auto recovered = DeserializeClientCapabilities(jv);
    EXPECT_TRUE(recovered.extensions.has_value());
}

TEST(Conformance, ServerCapabilitiesNoExtensions) {
    ServerCapabilities caps;
    caps.tools = ToolsCapability{};

    auto jv = SerializeServerCapabilities(caps);
    EXPECT_FALSE(jv.Contains("extensions"));

    auto recovered = DeserializeServerCapabilities(jv);
    EXPECT_FALSE(recovered.extensions.has_value());
}

TEST(Conformance, ClientCapabilitiesNoExtensions) {
    ClientCapabilities caps;
    auto jv = SerializeClientCapabilities(caps);
    EXPECT_FALSE(jv.Contains("extensions"));
}

TEST(Conformance, ExtensionsCapabilityWithOtherCaps) {
    ServerCapabilities caps;
    caps.tools = ToolsCapability{};
    caps.extensions = std::map<std::string, JsonValue>{};
    caps.subscriptions = SubscriptionsCapability{};

    auto jv = SerializeServerCapabilities(caps);
    EXPECT_TRUE(jv.Contains("extensions"));
    EXPECT_TRUE(jv.Contains("subscriptions"));
    EXPECT_TRUE(jv.Contains("tools"));
}

// ====================================================================
// Group 6: ResultType enum (8-10 tests)
// ====================================================================
TEST(Conformance, CallToolResultHasResultType) {
    CallToolResult r;
    r.content = {TextContent{"text", "hello"}};

    auto jv = SerializeCallToolResult(r);
    EXPECT_EQ(jv["resultType"].GetString(), "complete");

    auto recovered = DeserializeCallToolResult(jv);
    EXPECT_EQ(recovered.result_type, ResultType::Complete);
}

TEST(Conformance, ListToolsResultHasResultType) {
    ListToolsResult r;
    r.tools = {};

    auto jv = SerializeListToolsResult(r);
    EXPECT_EQ(jv["resultType"].GetString(), "complete");

    auto recovered = DeserializeListToolsResult(jv);
    EXPECT_EQ(recovered.result_type, ResultType::Complete);
}

TEST(Conformance, ListResourcesResultHasResultType) {
    ListResourcesResult r;
    r.resources = {};

    auto jv = SerializeListResourcesResult(r);
    EXPECT_EQ(jv["resultType"].GetString(), "complete");

    auto recovered = DeserializeListResourcesResult(jv);
    EXPECT_EQ(recovered.result_type, ResultType::Complete);
}

TEST(Conformance, ReadResourceResultHasResultType) {
    ReadResourceResult r;
    TextResourceContents trc;
    trc.uri = "file:///a.txt";
    trc.text = "data";
    r.contents = {ResourceContents(trc)};

    auto jv = SerializeReadResourceResult(r);
    EXPECT_EQ(jv["resultType"].GetString(), "complete");

    auto recovered = DeserializeReadResourceResult(jv);
    EXPECT_EQ(recovered.result_type, ResultType::Complete);
}

TEST(Conformance, ListPromptsResultHasResultType) {
    ListPromptsResult r;
    r.prompts = {};

    auto jv = SerializeListPromptsResult(r);
    EXPECT_EQ(jv["resultType"].GetString(), "complete");
}

TEST(Conformance, GetPromptResultHasResultType) {
    GetPromptResult r;
    r.messages = {};

    auto jv = SerializeGetPromptResult(r);
    EXPECT_EQ(jv["resultType"].GetString(), "complete");
}

TEST(Conformance, InitializeResultHasResultType) {
    InitializeResult r;
    r.protocol_version = "2026-07-28";
    r.server_info = Implementation{"s", "1"};
    r.capabilities = ServerCapabilities{};

    auto jv = SerializeInitializeResult(r);
    EXPECT_EQ(jv["resultType"].GetString(), "complete");
}

TEST(Conformance, DiscoverResultHasResultType) {
    DiscoverResult r;
    r.supported_versions = {"2026-07-28"};
    r.server_info = Implementation{"s", "1"};
    r.capabilities = ServerCapabilities{};

    auto jv = SerializeDiscoverResult(r);
    EXPECT_EQ(jv["resultType"].GetString(), "complete");

    auto recovered = DeserializeDiscoverResult(jv);
    EXPECT_EQ(recovered.result_type, ResultType::Complete);
}

TEST(Conformance, CompleteResultHasResultType) {
    CompleteResult r;
    r.completion = JsonValue(JsonValue::object_tag);
    r.completion["values"] = JsonValue(JsonValue::array_tag);
    r.completion["values"].PushBack("a");
    r.completion["values"].PushBack("b");

    auto jv = SerializeCompleteResult(r);
    EXPECT_EQ(jv["resultType"].GetString(), "complete");
}

TEST(Conformance, ResultTypeDefaultWhenMissing) {
    JsonValue jv(JsonValue::object_tag);
    jv["tools"] = JsonValue(JsonValue::array_tag);
    auto recovered = DeserializeListToolsResult(jv);
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

    auto jv = SerializeSubscriptionFilter(f);
    EXPECT_EQ(jv["toolsListChanged"].GetBool(), true);
    EXPECT_EQ(jv["promptsListChanged"].GetBool(), false);
    EXPECT_EQ(jv["resourcesListChanged"].GetBool(), true);

    auto recovered = DeserializeSubscriptionFilter(jv);
    EXPECT_TRUE(recovered.tools_list_changed.value_or(false));
    EXPECT_FALSE(recovered.prompts_list_changed.value_or(true));
    EXPECT_TRUE(recovered.resources_list_changed.value_or(false));
}

TEST(Conformance, SubscriptionFilterWithResourceSubscriptions) {
    SubscriptionFilter f;
    f.resource_subscriptions = {"file:///a.txt", "file:///b.txt"};

    auto jv = SerializeSubscriptionFilter(f);
    ASSERT_TRUE(jv.Contains("resourceSubscriptions"));
    EXPECT_EQ(jv["resourceSubscriptions"].Size(), 2);
    EXPECT_EQ(jv["resourceSubscriptions"][1].GetString(), "file:///b.txt");

    auto recovered = DeserializeSubscriptionFilter(jv);
    EXPECT_EQ(recovered.resource_subscriptions.size(), 2);
}

TEST(Conformance, SubscriptionFilterEmpty) {
    SubscriptionFilter f;
    auto jv = SerializeSubscriptionFilter(f);
    EXPECT_TRUE(jv.Empty());
}

TEST(Conformance, SubscriptionsListenRequestParamsRoundTrip) {
    SubscriptionsListenRequestParams p;
    p.notifications.tools_list_changed = true;
    p.notifications.resource_subscriptions = {"file:///data.txt"};

    auto jv = SerializeSubscriptionsListenRequestParams(p);
    EXPECT_TRUE(jv["notifications"]["toolsListChanged"].GetBool());
    EXPECT_EQ(jv["notifications"]["resourceSubscriptions"][0].GetString(), "file:///data.txt");

    auto recovered = DeserializeSubscriptionsListenRequestParams(jv);
    EXPECT_TRUE(recovered.notifications.tools_list_changed.value_or(false));
}

TEST(Conformance, SubscriptionsAcknowledgedRoundTrip) {
    SubscriptionsAcknowledgedNotificationParams n;
    n.notifications.prompts_list_changed = true;

    auto jv = SerializeSubscriptionsAcknowledgedNotificationParams(n);
    EXPECT_TRUE(jv["notifications"]["promptsListChanged"].GetBool());

    auto recovered = DeserializeSubscriptionsAcknowledgedNotificationParams(jv);
    EXPECT_TRUE(recovered.notifications.prompts_list_changed.value_or(false));
}

TEST(Conformance, SubscriptionFilterPartialFlags) {
    SubscriptionFilter f;
    f.resources_list_changed = true;

    auto jv = SerializeSubscriptionFilter(f);
    EXPECT_FALSE(jv.Contains("toolsListChanged"));
    EXPECT_FALSE(jv.Contains("promptsListChanged"));
    EXPECT_TRUE(jv["resourcesListChanged"].GetBool());

    auto recovered = DeserializeSubscriptionFilter(jv);
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

    auto jv = SerializeGetTaskResult(r);
    EXPECT_EQ(jv["taskId"].GetString(), "task-1");
    EXPECT_EQ(jv["status"].GetString(), "working");
    EXPECT_EQ(jv["resultType"].GetString(), "task");

    auto recovered = DeserializeGetTaskResult(jv);
    EXPECT_EQ(recovered.task_id, "task-1");
    EXPECT_EQ(recovered.status, "working");
}

TEST(Conformance, GetTaskResultCompleted) {
    GetTaskResult r;
    r.task_id = "task-2";
    r.status = "completed";
    r.result = JsonValue(JsonValue::object_tag);
    (*r.result)["output"] = "done";

    auto jv = SerializeGetTaskResult(r);
    EXPECT_EQ(jv["status"].GetString(), "completed");
    EXPECT_EQ(jv["result"]["output"].GetString(), "done");

    auto recovered = DeserializeGetTaskResult(jv);
    EXPECT_EQ(recovered.status, "completed");
    ASSERT_TRUE(recovered.result.has_value());
}

TEST(Conformance, GetTaskResultFailed) {
    GetTaskResult r;
    r.task_id = "task-3";
    r.status = "failed";
    r.error_message = "something went wrong";

    auto jv = SerializeGetTaskResult(r);
    EXPECT_EQ(jv["status"].GetString(), "failed");
    EXPECT_EQ(jv["errorMessage"].GetString(), "something went wrong");

    auto recovered = DeserializeGetTaskResult(jv);
    EXPECT_EQ(recovered.status, "failed");
    ASSERT_TRUE(recovered.error_message.has_value());
    EXPECT_EQ(*recovered.error_message, "something went wrong");
}

TEST(Conformance, GetTaskResultCancelled) {
    GetTaskResult r;
    r.task_id = "task-4";
    r.status = "cancelled";

    auto jv = SerializeGetTaskResult(r);
    EXPECT_EQ(jv["status"].GetString(), "cancelled");
}

TEST(Conformance, GetTaskResultInputRequired) {
    GetTaskResult r;
    r.task_id = "task-5";
    r.status = "working";
    r.input_required = JsonValue(JsonValue::object_tag);
    (*r.input_required)["fields"] = JsonValue(JsonValue::array_tag);
    (*r.input_required)["fields"].PushBack("name");

    auto jv = SerializeGetTaskResult(r);
    ASSERT_TRUE(jv.Contains("inputRequired"));
    EXPECT_EQ(jv["inputRequired"]["fields"][0].GetString(), "name");
}

TEST(Conformance, UpdateTaskResultRoundTrip) {
    UpdateTaskResult r;
    auto jv = SerializeEmptyResult(r);
    EXPECT_TRUE(jv.Contains("resultType"));

    auto recovered = DeserializeEmptyResult(jv);
    EXPECT_EQ(recovered.result_type, ResultType::Complete);
}

TEST(Conformance, CancelTaskResultRoundTrip) {
    CancelTaskResult r;
    auto jv = SerializeEmptyResult(r);
    EXPECT_TRUE(jv.Contains("resultType"));

    auto recovered = DeserializeEmptyResult(jv);
    EXPECT_EQ(recovered.result_type, ResultType::Complete);
}

TEST(Conformance, GetTaskRequestParamsRoundTrip) {
    GetTaskRequestParams p;
    p.task_id = "task-1";

    auto jv = SerializeGetTaskRequestParams(p);
    EXPECT_EQ(jv["taskId"].GetString(), "task-1");

    auto recovered = DeserializeGetTaskRequestParams(jv);
    EXPECT_EQ(recovered.task_id, "task-1");
}

TEST(Conformance, UpdateTaskRequestParamsRoundTrip) {
    UpdateTaskRequestParams p;
    p.task_id = "task-2";
    p.result = JsonValue(JsonValue::object_tag);
    (*p.result)["progress"] = 0.5;

    auto jv = SerializeUpdateTaskRequestParams(p);
    EXPECT_EQ(jv["taskId"].GetString(), "task-2");
    EXPECT_EQ(jv["result"]["progress"], JsonValue(0.5));

    auto recovered = DeserializeUpdateTaskRequestParams(jv);
    EXPECT_EQ(recovered.task_id, "task-2");
}

TEST(Conformance, CancelTaskRequestParamsRoundTrip) {
    CancelTaskRequestParams p;
    p.task_id = "task-3";
    p.reason = "no longer needed";

    auto jv = SerializeCancelTaskRequestParams(p);
    EXPECT_EQ(jv["taskId"].GetString(), "task-3");
    EXPECT_EQ(jv["reason"].GetString(), "no longer needed");

    auto recovered = DeserializeCancelTaskRequestParams(jv);
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

    auto jv = SerializeLoggingMessageNotificationParams(p);
    EXPECT_EQ(jv["level"].GetString(), "debug");
    EXPECT_EQ(jv["logger"].GetString(), "test");
    EXPECT_EQ(jv["data"].GetString(), "debug message");

    auto recovered = DeserializeLoggingMessageNotificationParams(jv);
    EXPECT_EQ(recovered.level, LoggingLevel::Debug);
}

TEST(Conformance, LoggingLevelInfo) {
    LoggingMessageNotificationParams p;
    p.level = LoggingLevel::Info;
    p.logger = "app";
    p.data = JsonValue(JsonValue::object_tag);
    p.data["event"] = "started";

    auto jv = SerializeLoggingMessageNotificationParams(p);
    EXPECT_EQ(jv["level"].GetString(), "info");
}

TEST(Conformance, LoggingLevelWarning) {
    LoggingMessageNotificationParams p;
    p.level = LoggingLevel::Warning;
    p.logger = "app";
    p.data = "warning";

    auto jv = SerializeLoggingMessageNotificationParams(p);
    EXPECT_EQ(jv["level"].GetString(), "warning");

    auto recovered = DeserializeLoggingMessageNotificationParams(jv);
    EXPECT_EQ(recovered.level, LoggingLevel::Warning);
}

TEST(Conformance, LoggingLevelErrorCritical) {
    LoggingMessageNotificationParams p;
    p.level = LoggingLevel::Error;
    p.logger = "sys";
    p.data = "error";

    auto jv = SerializeLoggingMessageNotificationParams(p);
    EXPECT_EQ(jv["level"].GetString(), "error");

    p.level = LoggingLevel::Critical;
    jv = SerializeLoggingMessageNotificationParams(p);
    EXPECT_EQ(jv["level"].GetString(), "critical");

    auto recovered = DeserializeLoggingMessageNotificationParams(jv);
    EXPECT_EQ(recovered.level, LoggingLevel::Critical);
}

TEST(Conformance, LoggingLevelAllValues) {
    auto check_level = [](LoggingLevel level, const char* expected) {
        LoggingMessageNotificationParams p;
        p.level = level;
        p.logger = "t";
        p.data = "x";
        auto jv = SerializeLoggingMessageNotificationParams(p);
        EXPECT_STREQ(jv["level"].GetString().c_str(), expected);
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

    auto jv = SerializeSetLevelRequestParams(p);
    EXPECT_EQ(jv["level"].GetString(), "warning");

    auto recovered = DeserializeSetLevelRequestParams(jv);
    EXPECT_EQ(recovered.level, LoggingLevel::Warning);
}

TEST(Conformance, SetLevelRequestParamsAllLevels) {
    auto check = [](LoggingLevel level) {
        SetLevelRequestParams p;
        p.level = level;
        auto jv = SerializeSetLevelRequestParams(p);
        auto recovered = DeserializeSetLevelRequestParams(jv);
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
