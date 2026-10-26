#include <mcp/McpTypes.hpp>

#include <detail/JsonSchemaValidator.hpp>

#include <gtest/gtest.h>

using namespace mcp;

TEST(McpTypesTest, ToolRoundTrip) {
    Tool t;
    t.name = "test_tool";
    t.title = "Test Tool";
    t.description = "A test tool";
    t.input_schema = JsonValue(JsonValue::object_tag);
    t.input_schema["type"] = "object";
    {
        auto props = JsonValue(JsonValue::object_tag);
        auto text_prop = JsonValue(JsonValue::object_tag);
        text_prop["type"] = "string";
        props["text"] = text_prop;
        t.input_schema["properties"] = props;
    }
    t.annotations = ToolAnnotations{};
    t.annotations->read_only_hint = true;

    auto jv = SerializeTool(t);
    auto t2 = DeserializeTool(jv);

    EXPECT_EQ(t.name, t2.name);
    EXPECT_EQ(t.title, t2.title);
    EXPECT_EQ(t.description, t2.description);
    EXPECT_EQ(t.input_schema, t2.input_schema);
    EXPECT_EQ(t.annotations->read_only_hint, t2.annotations->read_only_hint);
}

TEST(McpTypesTest, ToolExecutionTaskSupportRoundTrip) {
    ToolExecution required;
    required.mode = ToolExecutionMode::Task;
    required.task_support = TaskSupport::Required;
    auto required_wire = SerializeToolExecution(required);
    EXPECT_EQ(required_wire["taskSupport"].GetString(), "required");
    auto required_back = DeserializeToolExecution(required_wire);
    ASSERT_TRUE(required_back.task_support.has_value());
    EXPECT_TRUE(required_back.task_support == TaskSupport::Required);

    ToolExecution optional;
    optional.mode = ToolExecutionMode::Task;
    optional.task_support = TaskSupport::Optional;
    auto optional_wire = SerializeToolExecution(optional);
    EXPECT_EQ(optional_wire["taskSupport"].GetString(), "optional");

    ToolExecution unset;
    unset.mode = ToolExecutionMode::Task;
    auto unset_wire = SerializeToolExecution(unset);
    EXPECT_FALSE(unset_wire.Contains("taskSupport"));
    auto unset_back = DeserializeToolExecution(unset_wire);
    EXPECT_FALSE(unset_back.task_support.has_value());
}

TEST(McpTypesTest, ResourceRoundTrip) {
    Resource r;
    r.uri = "file:///data";
    r.name = "Data File";

    auto jv = SerializeResource(r);
    auto r2 = DeserializeResource(jv);

    EXPECT_EQ(r.uri, r2.uri);
    EXPECT_EQ(r.name, r2.name);
}

TEST(McpTypesTest, PromptRoundTrip) {
    Prompt p;
    p.name = "greet";

    PromptArgument arg;
    arg.name = "name";
    arg.description = "The name to greet";
    arg.required = true;
    p.arguments = {arg};

    auto jv = SerializePrompt(p);
    auto p2 = DeserializePrompt(jv);

    EXPECT_EQ(p.name, p2.name);
    ASSERT_TRUE(p2.arguments.has_value());
    EXPECT_EQ(p2.arguments->at(0).name, "name");
    EXPECT_TRUE(p2.arguments->at(0).required.value_or(false));
}

TEST(McpTypesTest, ImplementationRoundTrip) {
    Implementation impl;
    impl.name = "test-server";
    impl.version = "1.0.0";

    auto jv = SerializeImplementation(impl);
    auto impl2 = DeserializeImplementation(jv);

    EXPECT_EQ(impl.name, impl2.name);
    EXPECT_EQ(impl.version, impl2.version);
}

TEST(McpTypesTest, CallToolResultRoundTrip) {
    CallToolResult r;
    r.content = {TextContent{"text", "Hello!"}};
    r.is_error = false;

    auto jv = SerializeCallToolResult(r);
    auto r2 = DeserializeCallToolResult(jv);

    ASSERT_EQ(r2.content.size(), 1);
    auto* text = std::get_if<TextContent>(&r2.content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "Hello!");
    EXPECT_FALSE(r2.is_error);
}

TEST(McpTypesTest, CallToolResultWithError) {
    CallToolResult r;
    r.is_error = true;

    auto jv = SerializeCallToolResult(r);
    auto r2 = DeserializeCallToolResult(jv);

    EXPECT_TRUE(r2.is_error);
}

TEST(McpTypesTest, CallToolResultInputRequiredRoundTrip) {
    CallToolResult r;
    r.content = {TextContent{"text", "need input"}};
    r.result_type = ResultType::InputRequired;

    InputRequest name_request;
    name_request.method = "elicitation/create";
    name_request.params = JsonValue::Parse(R"({"message":"Give me a name"})");
    InputRequest roots_request;
    roots_request.method = "roots/list";
    roots_request.params = JsonValue(JsonValue::object_tag);
    r.input_required = InputRequiredResult{};
    r.input_required->input_requests["name"] = name_request;
    r.input_required->input_requests["roots"] = roots_request;
    r.input_required->request_state = "state-123";

    auto r2 = DeserializeCallToolResult(SerializeCallToolResult(r));

    ASSERT_TRUE(r2.input_required.has_value());
    ASSERT_EQ(r2.input_required->input_requests.size(), size_t{2});
    ASSERT_EQ(r2.input_required->input_requests.count("name"), size_t{1});
    EXPECT_EQ(r2.input_required->input_requests.at("name").method, "elicitation/create");
    EXPECT_EQ(r2.input_required->input_requests.at("name").params, name_request.params);
    ASSERT_EQ(r2.input_required->input_requests.count("roots"), size_t{1});
    EXPECT_EQ(r2.input_required->input_requests.at("roots").method, "roots/list");
    EXPECT_TRUE(r2.input_required->input_requests.at("roots").params.Empty());
    EXPECT_EQ(r2.input_required->request_state, "state-123");
    EXPECT_EQ(r2.result_type, ResultType::InputRequired);
}

TEST(McpTypesTest, CallToolResultWithoutMrtrKeysHasNoInputRequired) {
    CallToolResult r;
    r.content = {TextContent{"text", "ok"}};
    auto jv = SerializeCallToolResult(r);
    EXPECT_FALSE(jv.Find("inputRequests"));
    auto r2 = DeserializeCallToolResult(jv);
    EXPECT_FALSE(r2.input_required.has_value());
    EXPECT_EQ(r2.result_type, ResultType::Complete);

    auto r3 = DeserializeCallToolResult(
        JsonValue::Parse(R"({"content":[],"isError":false,"resultType":"input_required"})"));
    ASSERT_TRUE(r3.input_required.has_value());
    EXPECT_TRUE(r3.input_required->input_requests.empty());
    EXPECT_FALSE(r3.input_required->request_state.has_value());
}

TEST(McpTypesTest, ListToolsResultRoundTrip) {
    ListToolsResult r;
    Tool t;
    t.name = "tool1";
    r.tools = {t};
    r.next_cursor = "cursor123";

    auto jv = SerializeListToolsResult(r);
    auto r2 = DeserializeListToolsResult(jv);

    ASSERT_EQ(r2.tools.size(), 1);
    EXPECT_EQ(r2.tools[0].name, "tool1");
    EXPECT_EQ(r2.next_cursor, "cursor123");
}

TEST(McpTypesTest, TextContentRoundTrip) {
    TextContent tc{"text", "Hello world", "text/plain"};
    auto jv = SerializeTextContent(tc);
    auto tc2 = DeserializeTextContent(jv);
    EXPECT_EQ(tc.text, tc2.text);
}

TEST(McpTypesTest, ContentVariantTextRoundTrip) {
    ContentVariant cv = TextContent{"text", "Hello"};
    auto jv = SerializeContentVariant(cv);
    auto cv2 = DeserializeContentVariant(jv);
    EXPECT_TRUE(std::holds_alternative<TextContent>(cv2));
    EXPECT_EQ(std::get<TextContent>(cv2).text, "Hello");
}

TEST(McpTypesTest, ContentVariantImageRoundTrip) {
    ContentVariant cv = ImageContent{"image", "base64data", "image/png"};
    auto jv = SerializeContentVariant(cv);
    auto cv2 = DeserializeContentVariant(jv);
    EXPECT_TRUE(std::holds_alternative<ImageContent>(cv2));
    EXPECT_EQ(std::get<ImageContent>(cv2).mime_type, "image/png");
}

TEST(McpTypesTest, ContentVariantAudioRoundTrip) {
    ContentVariant cv = AudioContent{"audio", "base64data", "audio/wav"};
    auto jv = SerializeContentVariant(cv);
    auto cv2 = DeserializeContentVariant(jv);
    EXPECT_TRUE(std::holds_alternative<AudioContent>(cv2));
}

TEST(McpTypesTest, ServerCapabilitiesRoundTrip) {
    ServerCapabilities caps;
    caps.tools = ToolsCapability{true};
    caps.resources = ResourcesCapability{true, true};

    auto jv = SerializeServerCapabilities(caps);
    auto caps2 = DeserializeServerCapabilities(jv);

    EXPECT_TRUE(caps2.tools.has_value());
    EXPECT_TRUE(caps2.tools->list_changed.value_or(false));
    EXPECT_TRUE(caps2.resources.has_value());
}

TEST(McpTypesTest, ClientCapabilitiesRoundTrip) {
    ClientCapabilities caps;
    caps.elicitation = ElicitationCapability{};
    caps.elicitation->form = JsonValue(JsonValue::object_tag);

    auto jv = SerializeClientCapabilities(caps);
    auto caps2 = DeserializeClientCapabilities(jv);

    EXPECT_TRUE(caps2.elicitation.has_value());
}

TEST(McpTypesTest, DiscoverResultRoundTrip) {
    DiscoverResult r;
    r.supported_versions = {"2025-11-25", "2026-07-28"};
    r.capabilities = ServerCapabilities{};
    r.capabilities.tools = ToolsCapability{};
    r.server_info = Implementation{"my-server", "1.0.0"};

    auto jv = SerializeDiscoverResult(r);
    auto r2 = DeserializeDiscoverResult(jv);

    ASSERT_EQ(r2.supported_versions.size(), 2);
    EXPECT_EQ(r2.supported_versions[1], "2026-07-28");
    EXPECT_EQ(r2.server_info.name, "my-server");
}

TEST(McpTypesTest, InitializeResultRoundTrip) {
    InitializeResult r;
    r.protocol_version = "2026-07-28";
    r.capabilities = ServerCapabilities{};
    r.capabilities.tools = ToolsCapability{};
    r.server_info = Implementation{"server", "1.0.0"};

    auto jv = SerializeInitializeResult(r);
    auto r2 = DeserializeInitializeResult(jv);

    EXPECT_EQ(r2.protocol_version, "2026-07-28");
}

TEST(McpTypesTest, GetPromptResultRoundTrip) {
    GetPromptResult r;
    PromptMessage pm;
    pm.role = "user";
    pm.content = TextContent{"text", "Hello"};
    r.messages = {pm};

    auto jv = SerializeGetPromptResult(r);
    auto r2 = DeserializeGetPromptResult(jv);

    ASSERT_EQ(r2.messages.size(), 1);
    EXPECT_EQ(r2.messages[0].role, "user");
    EXPECT_TRUE(std::holds_alternative<TextContent>(r2.messages[0].content));
}

TEST(McpTypesTest, GetPromptResultInputRequiredRoundTrip) {
    GetPromptResult r;
    r.description = "daily report";
    InputRequest elicit;
    elicit.method = "elicitation/create";
    elicit.params = JsonValue::Parse(R"({"message":"Which day?"})");
    r.input_required = InputRequiredResult{};
    r.input_required->input_requests["day"] = elicit;
    r.input_required->request_state = "prompt-state";

    auto r2 = DeserializeGetPromptResult(SerializeGetPromptResult(r));

    ASSERT_TRUE(r2.input_required.has_value());
    ASSERT_EQ(r2.input_required->input_requests.count("day"), size_t{1});
    EXPECT_EQ(r2.input_required->input_requests.at("day").method, "elicitation/create");
    EXPECT_EQ(r2.input_required->input_requests.at("day").params, elicit.params);
    EXPECT_EQ(r2.input_required->request_state, "prompt-state");
    EXPECT_EQ(r2.result_type, ResultType::InputRequired);
    EXPECT_EQ(r2.description, "daily report");

    r.input_required = std::nullopt;
    auto r3 = DeserializeGetPromptResult(SerializeGetPromptResult(r));
    EXPECT_FALSE(r3.input_required.has_value());
    EXPECT_EQ(r3.result_type, ResultType::Complete);
}

TEST(McpTypesTest, TextResourceContentsRoundTrip) {
    TextResourceContents r;
    r.uri = "file:///doc.txt";
    r.text = "content";
    auto rc = ResourceContents(r);

    auto jv = SerializeResourceContents(rc);
    auto rc2 = DeserializeResourceContents(jv);
    EXPECT_TRUE(std::holds_alternative<TextResourceContents>(rc2));
    EXPECT_EQ(std::get<TextResourceContents>(rc2).text, "content");
}

TEST(McpTypesTest, ReadResourceResultRoundTrip) {
    ReadResourceResult r;
    TextResourceContents trc;
    trc.uri = "file:///doc.txt";
    trc.text = "Hello";
    r.contents = {ResourceContents(trc)};

    auto jv = SerializeReadResourceResult(r);
    auto r2 = DeserializeReadResourceResult(jv);

    ASSERT_EQ(r2.contents.size(), 1);
    auto* rc = std::get_if<TextResourceContents>(&r2.contents[0]);
    ASSERT_NE(rc, nullptr);
    EXPECT_EQ(rc->text, "Hello");
}

TEST(McpTypesTest, ReadResourceResultInputRequiredRoundTrip) {
    ReadResourceResult r;
    TextResourceContents trc;
    trc.uri = "file:///doc.txt";
    trc.text = "Hello";
    r.contents = {ResourceContents(trc)};

    InputRequest roots;
    roots.method = "roots/list";
    roots.params = JsonValue(JsonValue::object_tag);
    r.input_required = InputRequiredResult{};
    r.input_required->input_requests["paths"] = roots;
    r.input_required->request_state = "read-state";

    auto jv = SerializeReadResourceResult(r);
    auto r2 = DeserializeReadResourceResult(jv);

    ASSERT_TRUE(r2.input_required.has_value());
    ASSERT_EQ(r2.input_required->input_requests.count("paths"), size_t{1});
    EXPECT_EQ(r2.input_required->input_requests.at("paths").method, "roots/list");
    EXPECT_EQ(r2.input_required->request_state, "read-state");
    EXPECT_EQ(r2.result_type, ResultType::InputRequired);
    ASSERT_EQ(r2.contents.size(), 1);

    r.input_required = std::nullopt;
    auto r3 = DeserializeReadResourceResult(SerializeReadResourceResult(r));
    EXPECT_FALSE(r3.input_required.has_value());
    EXPECT_EQ(r3.result_type, ResultType::Complete);
}

TEST(McpTypesTest, LoggingLevelRoundTrip) {
    auto jv = SerializeLoggingLevel(LoggingLevel::Warning);
    EXPECT_EQ(jv.GetString(), "warning");

    auto l = DeserializeLoggingLevel(jv);
    EXPECT_EQ(l, LoggingLevel::Warning);
}

TEST(McpTypesTest, RequestMetaRoundTrip) {
    RequestMeta m;
    m.protocol_version = "2026-07-28";
    m.client_info = Implementation{"client", "1.0"};

    auto jv = SerializeRequestMeta(m);
    auto m2 = DeserializeRequestMeta(jv);

    EXPECT_EQ(m2.protocol_version, "2026-07-28");
    EXPECT_TRUE(m2.client_info.has_value());
    EXPECT_EQ(m2.client_info->name, "client");
}

TEST(McpTypesTest, CallToolRequestParamsRoundTrip) {
    CallToolRequestParams p;
    p.name = "echo";
    {
        auto args = JsonValue(JsonValue::object_tag);
        args["text"] = "hello";
        p.arguments = args;
    }

    auto jv = SerializeCallToolRequestParams(p);
    auto p2 = DeserializeCallToolRequestParams(jv);

    EXPECT_EQ(p2.name, "echo");
    EXPECT_EQ((*p2.arguments)["text"].GetString(), "hello");
}

TEST(McpTypesTest, EmbeddedResourceRoundTrip) {
    TextResourceContents trc;
    trc.uri = "file:///doc.txt";
    trc.text = "embedded";
    EmbeddedResource er;
    er.resource = ResourceContents(trc);

    auto jv = SerializeEmbeddedResource(er);
    auto er2 = DeserializeEmbeddedResource(jv);

    auto* rc = std::get_if<TextResourceContents>(&er2.resource);
    ASSERT_NE(rc, nullptr);
    EXPECT_EQ(rc->text, "embedded");
}

TEST(McpTypesTest, EmptyResultSerializes) {
    EmptyResult r;
    auto jv = SerializeEmptyResult(r);
    EXPECT_FALSE(jv.Contains("resultType"));

    auto r2 = DeserializeEmptyResult(jv);
    EXPECT_EQ(r2.result_type, ResultType::Complete);
}

TEST(McpTypesTest, CompleteRequestParamsStandardShape) {
    const char* wire =
        "{\"ref\":{\"type\":\"ref/prompt\",\"name\":\"code_review\"},"
        "\"argument\":{\"name\":\"language\",\"value\":\"py\"}}";
    auto jv = JsonValue::Parse(wire);

    auto params = DeserializeCompleteRequestParams(jv);
    EXPECT_EQ(params.argument_name, "language");
    EXPECT_EQ(params.argument_value, "py");
    EXPECT_TRUE(params.ref.Contains("type"));

    auto out = SerializeCompleteRequestParams(params);
    EXPECT_TRUE(out.Contains("ref"));
    EXPECT_TRUE(out.Contains("argument"));
    EXPECT_EQ(out["argument"]["name"], JsonValue("language"));
    EXPECT_EQ(out["argument"]["value"], JsonValue("py"));
    EXPECT_FALSE(out.Contains("argumentName"));
}

TEST(McpTypesTest, CompleteRequestParamsFlatShapeCompat) {
    const char* wire =
        "{\"ref\":{\"type\":\"ref/prompt\",\"name\":\"code_review\"},"
        "\"argumentName\":\"language\",\"argumentValue\":\"py\"}";
    auto jv = JsonValue::Parse(wire);

    auto params = DeserializeCompleteRequestParams(jv);
    EXPECT_EQ(params.argument_name, "language");
    EXPECT_EQ(params.argument_value, "py");
}

TEST(McpTypesTest, CompleteResultRoundTrip) {
    CompleteResult r;
    {
        auto completion = JsonValue(JsonValue::object_tag);
        auto values = JsonValue(JsonValue::array_tag);
        values.PushBack(JsonValue("hello"));
        values.PushBack(JsonValue("help"));
        completion["values"] = values;
        r.completion = completion;
    }

    auto jv = SerializeCompleteResult(r);
    auto r2 = DeserializeCompleteResult(jv);

    EXPECT_EQ(r2.completion["values"].Size(), 2);
}

TEST(McpTypesTest, ResourceTemplateRoundTrip) {
    ResourceTemplate rt;
    rt.uri_template = "file:///{path}";
    rt.name = "Dynamic Files";
    auto jv = SerializeResourceTemplate(rt);
    auto rt2 = DeserializeResourceTemplate(jv);
    EXPECT_EQ(rt2.uri_template, "file:///{path}");
}

TEST(McpTypesTest, IconRoundTrip) {
    Icon ic{"https://example.com/icon.svg", "image/svg+xml", std::vector<std::string>{"any"}};
    auto jv = SerializeIcon(ic);
    auto ic2 = DeserializeIcon(jv);
    EXPECT_EQ(ic2.src, "https://example.com/icon.svg");
}

TEST(McpTypesTest, JsonSchemaValidatorRequiredAndProperties) {
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = "object";
    JsonValue props(JsonValue::object_tag);
    JsonValue name_s(JsonValue::object_tag);
    name_s["type"] = "string";
    props["name"] = std::move(name_s);
    schema["properties"] = std::move(props);
    JsonValue req(JsonValue::array_tag);
    req.PushBack(JsonValue("name"));
    schema["required"] = std::move(req);

    std::string err;
    JsonValue ok(JsonValue::object_tag);
    ok["name"] = JsonValue("x");
    EXPECT_TRUE(detail::ValidateJsonSchema(ok, schema, err));

    JsonValue bad(JsonValue::object_tag);
    EXPECT_FALSE(detail::ValidateJsonSchema(bad, schema, err));
    EXPECT_NE(err.find("required"), std::string::npos);
}

TEST(McpTypesTest, JsonSchemaValidatorTypeAndEnum) {
    std::string err;
    JsonValue enum_schema(JsonValue::object_tag);
    JsonValue enums(JsonValue::array_tag);
    enums.PushBack(JsonValue("red"));
    enums.PushBack(JsonValue("green"));
    enum_schema["enum"] = std::move(enums);
    EXPECT_TRUE(detail::ValidateJsonSchema(JsonValue("green"), enum_schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(JsonValue("blue"), enum_schema, err));

    JsonValue int_schema(JsonValue::object_tag);
    int_schema["type"] = "integer";
    EXPECT_TRUE(detail::ValidateJsonSchema(JsonValue(int64_t(3)), int_schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(JsonValue(3.5), int_schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(JsonValue("3"), int_schema, err));
}

TEST(McpTypesTest, JsonSchemaValidatorNumbersAndArrays) {
    std::string err;
    JsonValue num_schema(JsonValue::object_tag);
    num_schema["type"] = "number";
    num_schema["minimum"] = JsonValue(0.0);
    num_schema["maximum"] = JsonValue(150.0);
    EXPECT_TRUE(detail::ValidateJsonSchema(JsonValue(42.0), num_schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(JsonValue(200.0), num_schema, err));

    JsonValue arr_schema(JsonValue::object_tag);
    JsonValue item_schema(JsonValue::object_tag);
    item_schema["type"] = "string";
    arr_schema["items"] = std::move(item_schema);
    arr_schema["minItems"] = JsonValue(int64_t(1));
    JsonValue arr_ok(JsonValue::array_tag);
    arr_ok.PushBack(JsonValue("a"));
    EXPECT_TRUE(detail::ValidateJsonSchema(arr_ok, arr_schema, err));
    JsonValue arr_bad(JsonValue::array_tag);
    EXPECT_FALSE(detail::ValidateJsonSchema(arr_bad, arr_schema, err));
    JsonValue arr_bad2(JsonValue::array_tag);
    arr_bad2.PushBack(JsonValue(int64_t(1)));
    EXPECT_FALSE(detail::ValidateJsonSchema(arr_bad2, arr_schema, err));
}

TEST(McpTypesTest, ResultTypeTaskRoundTrip) {
    EXPECT_EQ(SerializeResultType(ResultType::Task), JsonValue("task"));
    EXPECT_EQ(DeserializeResultType(JsonValue("task")), ResultType::Task);
    EXPECT_EQ(DeserializeResultType(JsonValue("complete")), ResultType::Complete);
    EXPECT_EQ(DeserializeResultType(JsonValue("input_required")), ResultType::InputRequired);
}

TEST(McpTypesTest, CreateTaskResultFlatAndNestedCompat) {
    CreateTaskResult r;
    r.task_id = "task-1";
    r.status = "working";
    r.created_at = "2026-01-01T00:00:00Z";
    r.ttl_ms = int64_t(60000);
    InputRequests reqs;
    ElicitRequestParams ep;
    ep.message = "name?";
    reqs["q"] = MakeInputRequestForElicitation(ep);
    r.input_requests = reqs;

    auto jv = SerializeCreateTaskResult(r);
    EXPECT_EQ(jv["resultType"].GetString(), "task");
    EXPECT_EQ(jv["taskId"].GetString(), "task-1");
    EXPECT_EQ(jv["status"].GetString(), "working");
    EXPECT_EQ(jv["ttlMs"].GetInt(), int64_t(60000));
    ASSERT_TRUE(jv.Contains("task"));
    EXPECT_EQ(jv["task"]["taskId"].GetString(), "task-1");

    auto r2 = DeserializeCreateTaskResult(jv);
    EXPECT_EQ(r2.task_id, "task-1");
    EXPECT_EQ(r2.status, "working");
    EXPECT_EQ(r2.created_at, "2026-01-01T00:00:00Z");
    ASSERT_TRUE(r2.input_requests.has_value());
    EXPECT_EQ(r2.input_requests->size(), size_t{1});
}

TEST(McpTypesTest, CreateTaskResultReadsLegacyNestedTask) {
    JsonValue task(JsonValue::object_tag);
    task["taskId"] = JsonValue("t-9");
    task["status"] = JsonValue("working");
    task["createdAt"] = JsonValue("2026-01-01T00:00:00Z");
    JsonValue jv(JsonValue::object_tag);
    jv["resultType"] = JsonValue("task");
    jv["task"] = std::move(task);

    auto r = DeserializeCreateTaskResult(jv);
    EXPECT_EQ(r.task_id, "t-9");
    EXPECT_EQ(r.status, "working");
    EXPECT_EQ(r.created_at, "2026-01-01T00:00:00Z");
}

TEST(McpTypesTest, UpdateTaskRequestParamsInputResponses) {
    UpdateTaskRequestParams p;
    p.task_id = "task-1";
    JsonValue responses(JsonValue::object_tag);
    JsonValue elic(JsonValue::object_tag);
    elic["action"] = JsonValue("accept");
    responses["q"] = std::move(elic);
    p.input_responses = responses;

    auto jv = SerializeUpdateTaskRequestParams(p);
    EXPECT_EQ(jv["taskId"].GetString(), "task-1");
    ASSERT_TRUE(jv.Contains("inputResponses"));

    auto p2 = DeserializeUpdateTaskRequestParams(jv);
    EXPECT_EQ(p2.task_id, "task-1");
    ASSERT_TRUE(p2.input_responses.has_value());
    EXPECT_EQ((*p2.input_responses)["q"]["action"].GetString(), "accept");
    EXPECT_FALSE(p2.result.has_value());
}

TEST(McpTypesTest, GetTaskResultExtendedFields) {
    GetTaskResult r;
    r.task_id = "task-2";
    r.status = "input_required";
    r.created_at = "2026-01-01T00:00:00Z";
    r.ttl_ms = int64_t(60000);
    r.poll_interval_ms = int64_t(5000);
    InputRequests reqs;
    ElicitRequestParams ep;
    ep.message = "name?";
    reqs["q"] = MakeInputRequestForElicitation(ep);
    r.input_requests = reqs;

    auto jv = SerializeGetTaskResult(r);
    EXPECT_EQ(jv["resultType"].GetString(), "task");
    EXPECT_EQ(jv["createdAt"].GetString(), "2026-01-01T00:00:00Z");

    auto r2 = DeserializeGetTaskResult(jv);
    EXPECT_EQ(r2.task_id, "task-2");
    ASSERT_TRUE(r2.input_requests.has_value());
    ASSERT_TRUE(r2.ttl_ms.has_value());
    EXPECT_EQ(*r2.ttl_ms, int64_t(60000));
}

TEST(McpTypesTest, GetTaskResultReadsLegacyErrorFields) {
    JsonValue jv(JsonValue::object_tag);
    jv["taskId"] = JsonValue("task-3");
    jv["status"] = JsonValue("failed");
    jv["resultType"] = JsonValue("task");
    jv["errorMessage"] = JsonValue("boom");

    auto r = DeserializeGetTaskResult(jv);
    ASSERT_TRUE(r.error_message.has_value());
    EXPECT_EQ(*r.error_message, "boom");
    EXPECT_FALSE(r.error.has_value());
}

TEST(McpTypesTest, SkillResourcesManifestRoundTrip) {
    Skill s;
    s.uri = "skill://code-review/SKILL.md";
    JsonValue fm(JsonValue::object_tag);
    fm["name"] = JsonValue("code-review");
    fm["description"] = JsonValue("Review code.");
    s.frontmatter = fm;
    SkillResource res{"skill://code-review/SKILL.md", "sha256:abc", int64_t(149)};
    s.resources = SkillResources{std::vector<SkillResource>{res}};

    auto jv = SerializeSkill(s);
    EXPECT_EQ(jv["uri"].GetString(), "skill://code-review/SKILL.md");
    EXPECT_EQ(jv["frontmatter"]["name"].GetString(), "code-review");
    ASSERT_TRUE(jv["resources"].IsArray());
    EXPECT_EQ(jv["resources"].Size(), size_t{1});

    auto s2 = DeserializeSkill(jv);
    EXPECT_EQ(s2.uri, s.uri);
    ASSERT_TRUE(std::holds_alternative<std::vector<SkillResource>>(s2.resources));
    EXPECT_EQ(std::get<std::vector<SkillResource>>(s2.resources)[0].digest, "sha256:abc");
}

TEST(McpTypesTest, SkillResourcesDynamicRoundTrip) {
    Skill s;
    s.uri = "skill://gen/SKILL.md";
    s.frontmatter = JsonValue(JsonValue::object_tag);
    s.resources = SkillResources{std::string("dynamic")};

    auto jv = SerializeSkill(s);
    EXPECT_EQ(jv["resources"].GetString(), "dynamic");

    auto s2 = DeserializeSkill(jv);
    ASSERT_TRUE(std::holds_alternative<std::string>(s2.resources));
    EXPECT_EQ(std::get<std::string>(s2.resources), "dynamic");
}

TEST(McpTypesTest, SkillsListResultRoundTrip) {
    SkillsListResult r;
    Skill s;
    s.uri = "skill://code-review/SKILL.md";
    s.frontmatter = JsonValue(JsonValue::object_tag);
    s.resources = SkillResources{std::string("dynamic")};
    r.skills = {s};
    r.next_cursor = "c1";
    r.ttl_ms = int64_t(300000);
    r.cache_scope = "public";

    auto jv = SerializeSkillsListResult(r);
    EXPECT_EQ(jv["resultType"].GetString(), "complete");
    EXPECT_EQ(jv["ttlMs"].GetInt(), int64_t(300000));
    EXPECT_EQ(jv["cacheScope"].GetString(), "public");
    ASSERT_TRUE(jv["skills"].IsArray());

    auto r2 = DeserializeSkillsListResult(jv);
    ASSERT_EQ(r2.skills.size(), size_t{1});
    EXPECT_EQ(r2.skills[0].uri, "skill://code-review/SKILL.md");
    EXPECT_EQ(r2.next_cursor, "c1");

    JsonValue bare(JsonValue::object_tag);
    auto r3 = DeserializeSkillsListResult(bare);
    EXPECT_EQ(r3.result_type, ResultType::Complete);
    EXPECT_TRUE(r3.skills.empty());
}

TEST(McpTypesTest, SkillsGetResultRoundTrip) {
    SkillsGetResult r;
    r.skill.uri = "skill://code-review/SKILL.md";
    r.skill.frontmatter = JsonValue(JsonValue::object_tag);
    r.skill.resources = SkillResources{std::string("dynamic")};
    r.ttl_ms = int64_t(300000);
    r.cache_scope = "public";

    auto jv = SerializeSkillsGetResult(r);
    EXPECT_EQ(jv["resultType"].GetString(), "complete");
    ASSERT_TRUE(jv.Contains("skill"));

    auto r2 = DeserializeSkillsGetResult(jv);
    EXPECT_EQ(r2.skill.uri, "skill://code-review/SKILL.md");
}

TEST(McpTypesTest, SkillsGetRequestParamsRoundTrip) {
    SkillsGetRequestParams p;
    p.uri = "skill://code-review/SKILL.md";

    auto jv = SerializeSkillsGetRequestParams(p);
    auto p2 = DeserializeSkillsGetRequestParams(jv);
    EXPECT_EQ(p2.uri, "skill://code-review/SKILL.md");
}

TEST(McpTypesTest, ReadDirectoryResultRoundTrip) {
    ReadDirectoryResult r;
    DirectoryChild child;
    child.uri = "skill://code-review/references/checklist.md";
    child.name = "checklist.md";
    child.mime_type = "text/markdown";
    r.resources = {child};
    r.next_cursor = "n1";

    auto jv = SerializeReadDirectoryResult(r);
    EXPECT_EQ(jv["resultType"].GetString(), "complete");
    ASSERT_TRUE(jv["resources"].IsArray());
    EXPECT_EQ(jv["resources"][size_t{0}]["name"].GetString(), "checklist.md");

    auto r2 = DeserializeReadDirectoryResult(jv);
    ASSERT_EQ(r2.resources.size(), size_t{1});
    EXPECT_EQ(r2.resources[0].uri, "skill://code-review/references/checklist.md");
    EXPECT_EQ(r2.next_cursor, "n1");
}

TEST(McpTypesTest, ReadDirectoryRequestParamsRoundTrip) {
    ReadDirectoryRequestParams p;
    p.uri = "skill://code-review/references";
    p.cursor = "c9";

    auto jv = SerializeReadDirectoryRequestParams(p);
    EXPECT_EQ(jv["cursor"].GetString(), "c9");

    auto p2 = DeserializeReadDirectoryRequestParams(jv);
    EXPECT_EQ(p2.uri, "skill://code-review/references");
    EXPECT_EQ(p2.cursor, "c9");
}

TEST(McpTypesTest, ListToolsResultWritesFlatCacheFields) {
    ListToolsResult r;
    Tool t;
    t.name = "tool1";
    r.tools = {t};
    CacheHint hint;
    hint.ttl_ms = int64_t(60000);
    hint.cache_scope = "public";
    r.cache_hint = hint;

    auto jv = SerializeListToolsResult(r);
    ASSERT_TRUE(jv.Contains("cacheHint"));
    EXPECT_EQ(jv["cacheHint"]["ttlMs"].GetInt(), int64_t(60000));
    EXPECT_EQ(jv["ttlMs"].GetInt(), int64_t(60000));
    EXPECT_EQ(jv["cacheScope"].GetString(), "public");

    auto r2 = DeserializeListToolsResult(jv);
    ASSERT_TRUE(r2.cache_hint.has_value());
    EXPECT_EQ(*r2.cache_hint->ttl_ms, int64_t(60000));
}

TEST(McpTypesTest, ListToolsResultReadsNestedCacheHint) {
    JsonValue hint(JsonValue::object_tag);
    hint["ttlMs"] = JsonValue(int64_t(60000));
    JsonValue jv(JsonValue::object_tag);
    jv["tools"] = JsonValue(JsonValue::array_tag);
    jv["cacheHint"] = std::move(hint);

    auto r = DeserializeListToolsResult(jv);
    ASSERT_TRUE(r.cache_hint.has_value());
    EXPECT_EQ(*r.cache_hint->ttl_ms, int64_t(60000));
    EXPECT_EQ(r.result_type, ResultType::Complete);
}

TEST(McpTypesTest, SkillsExtensionSettingsRoundTrip) {
    SkillsExtensionSettings s;
    auto jv = SerializeSkillsExtensionSettings(s);
    EXPECT_TRUE(jv.IsObject());
    EXPECT_TRUE(jv.Empty());
    EXPECT_EQ(std::string(kSkillsExtensionId), "io.modelcontextprotocol/skills");

    auto s2 = DeserializeSkillsExtensionSettings(jv);
    EXPECT_FALSE(s2.directory_read);

    s.directory_read = true;
    auto jv2 = SerializeSkillsExtensionSettings(s);
    EXPECT_TRUE(jv2["directoryRead"].GetBool());

    auto s3 = DeserializeSkillsExtensionSettings(jv2);
    EXPECT_TRUE(s3.directory_read);
}

TEST(McpTypesTest, TasksExtensionSettingsRoundTrip) {
    TasksExtensionSettings s;
    auto jv = SerializeTasksExtensionSettings(s);
    EXPECT_TRUE(jv.IsObject());
    EXPECT_TRUE(jv.Empty());
    EXPECT_EQ(std::string(kTasksExtensionId), "io.modelcontextprotocol/tasks");

    auto s2 = DeserializeTasksExtensionSettings(jv);
    (void)s2;
}

TEST(McpTypesTest, OutputSchemaArbitraryType) {
    Tool t;
    t.name = "t";
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("string");
    t.output_schema = schema;

    auto jv = SerializeTool(t);
    auto t2 = DeserializeTool(jv);
    ASSERT_TRUE(t2.output_schema.has_value());
    EXPECT_EQ((*t2.output_schema)["type"].GetString(), "string");

    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(JsonValue("hello"), *t2.output_schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(JsonValue(int64_t(3)), *t2.output_schema, err));
}

TEST(McpTypesTest, SkillsAndLegacyTaskMethods) {
    EXPECT_EQ(std::string(methods::kListSkills), "skills/list");
    EXPECT_EQ(std::string(methods::kGetSkill), "skills/get");
    EXPECT_EQ(std::string(methods::kReadDirectory), "resources/directory/read");
    EXPECT_EQ(std::string(methods::kLegacyGetTaskPayload), std::string(methods::kGetTaskPayload));
    EXPECT_EQ(std::string(methods::kLegacyListTasks), std::string(methods::kListTasks));
}

TEST(McpTypesTest, NoHintEmitsZeroPrivate) {
    {
        ListToolsResult r;
        auto jv = SerializeListToolsResult(r);
        EXPECT_EQ(jv["ttlMs"].GetInt(), int64_t(0));
        EXPECT_EQ(jv["cacheScope"].GetString(), "private");
    }
    {
        ListResourcesResult r;
        auto jv = SerializeListResourcesResult(r);
        EXPECT_EQ(jv["ttlMs"].GetInt(), int64_t(0));
        EXPECT_EQ(jv["cacheScope"].GetString(), "private");
    }
    {
        ListResourceTemplatesResult r;
        auto jv = SerializeListResourceTemplatesResult(r);
        EXPECT_EQ(jv["ttlMs"].GetInt(), int64_t(0));
        EXPECT_EQ(jv["cacheScope"].GetString(), "private");
    }
    {
        ReadResourceResult r;
        auto jv = SerializeReadResourceResult(r);
        EXPECT_EQ(jv["ttlMs"].GetInt(), int64_t(0));
        EXPECT_EQ(jv["cacheScope"].GetString(), "private");
    }
    {
        ListPromptsResult r;
        auto jv = SerializeListPromptsResult(r);
        EXPECT_EQ(jv["ttlMs"].GetInt(), int64_t(0));
        EXPECT_EQ(jv["cacheScope"].GetString(), "private");
    }
    {
        DiscoverResult r;
        r.server_info = Implementation{"s", "1"};
        auto jv = SerializeDiscoverResult(r);
        EXPECT_EQ(jv["ttlMs"].GetInt(), int64_t(0));
        EXPECT_EQ(jv["cacheScope"].GetString(), "private");
    }
    {
        SkillsListResult r;
        auto jv = SerializeSkillsListResult(r);
        EXPECT_EQ(jv["ttlMs"].GetInt(), int64_t(0));
        EXPECT_EQ(jv["cacheScope"].GetString(), "private");
    }
    {
        SkillsGetResult r;
        r.skill.resources = SkillResources{std::string("dynamic")};
        auto jv = SerializeSkillsGetResult(r);
        EXPECT_EQ(jv["ttlMs"].GetInt(), int64_t(0));
        EXPECT_EQ(jv["cacheScope"].GetString(), "private");
    }
    {
        ReadDirectoryResult r;
        auto jv = SerializeReadDirectoryResult(r);
        EXPECT_EQ(jv["ttlMs"].GetInt(), int64_t(0));
        EXPECT_EQ(jv["cacheScope"].GetString(), "private");
    }
}

TEST(McpTypesTest, ToolSchemaRemoteRefPreservedVerbatim) {
    auto wire = JsonValue::Parse(
        R"({"name":"remote_ref_tool",)"
        R"("inputSchema":{"$ref":"https://example.com/schemas/address.json",)"
        R"("type":"object"}})");
    Tool tool = DeserializeTool(wire);
    ASSERT_TRUE(tool.input_schema.Contains("$ref"));
    EXPECT_EQ(tool.input_schema["$ref"].GetString(),
        "https://example.com/schemas/address.json");
    JsonValue round_tripped = SerializeTool(tool);
    EXPECT_EQ(round_tripped["inputSchema"], wire["inputSchema"]);
}

TEST(McpTypesTest, ToolSchemaLocalRefPreservedVerbatim) {
    auto wire = JsonValue::Parse(
        R"({"name":"local_ref_tool",)"
        R"("inputSchema":{"$defs":{"address":{"type":"object"}},)"
        R"("properties":{"address":{"$ref":"#/$defs/address"}}}})");
    Tool tool = DeserializeTool(wire);
    EXPECT_EQ(tool.input_schema, wire["inputSchema"]);
    JsonValue round_tripped = SerializeTool(tool);
    EXPECT_EQ(round_tripped["inputSchema"], wire["inputSchema"]);
}

TEST(McpTypesTest, ValidatorRejectsRemoteRefWithoutDereferencing) {
    auto schema = JsonValue::Parse(
        R"({"$ref":"https://example.com/schemas/address.json"})");
    std::string err;
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue(JsonValue::object_tag), schema, err));
    EXPECT_NE(err.find("remote"), std::string::npos);
    EXPECT_NE(err.find("$ref"), std::string::npos);
}

TEST(McpTypesTest, ValidatorRejectsNestedRemoteRef) {
    auto schema = JsonValue::Parse(
        R"({"type":"object",)"
        R"("properties":{"address":{"$ref":"https://example.com/a.json"}}})");
    auto instance = JsonValue::Parse(R"({"address":{}})");
    std::string err;
    EXPECT_FALSE(detail::ValidateJsonSchema(instance, schema, err));
    EXPECT_NE(err.find("remote"), std::string::npos);
}

TEST(McpTypesTest, ValidatorRejectsUnresolvableLocalRef) {
    auto schema = JsonValue::Parse(R"({"$ref":"#/$defs/missing"})");
    std::string err;
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue(JsonValue::object_tag), schema, err));
}

TEST(McpTypesTest, ValidatorResolvesLocalRef) {
    auto schema = JsonValue::Parse(
        R"({"$defs":{"address":{"type":"object",)"
        R"("properties":{"city":{"type":"string"}},"required":["city"]}},)"
        R"("properties":{"address":{"$ref":"#/$defs/address"}}})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"address":{"city":"x"}})"), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"address":{}})"), schema, err));
}

TEST(McpTypesTest, ValidatorExclusiveMinMaxNumeric) {
    auto schema = JsonValue::Parse(
        R"({"type":"number","exclusiveMinimum":5,"exclusiveMaximum":10})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(JsonValue(7.0), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(JsonValue(5.0), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(JsonValue(10.0), schema, err));
}

TEST(McpTypesTest, ValidatorExclusiveMinMaxBooleanLegacy) {
    auto schema = JsonValue::Parse(
        R"({"type":"number","minimum":5,"maximum":10,)"
        R"("exclusiveMinimum":true,"exclusiveMaximum":true})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(JsonValue(7.0), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(JsonValue(5.0), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(JsonValue(10.0), schema, err));
}

TEST(McpTypesTest, ValidatorMultipleOf) {
    auto schema = JsonValue::Parse(R"({"multipleOf":3})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(JsonValue(int64_t(9)), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(JsonValue(int64_t(10)), schema, err));
    auto dbl = JsonValue::Parse(R"({"multipleOf":0.5})");
    EXPECT_TRUE(detail::ValidateJsonSchema(JsonValue(1.5), dbl, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(JsonValue(1.6), dbl, err));
}

TEST(McpTypesTest, ValidatorPatternAndFormatIgnored) {
    auto schema = JsonValue::Parse(
        R"({"type":"string","pattern":"^a+$","format":"email"})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(JsonValue("aaa"), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(JsonValue("b"), schema, err));
}

TEST(McpTypesTest, ValidatorInvalidPatternIgnored) {
    auto schema = JsonValue::Parse(R"({"type":"string","pattern":"([invalid"})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(JsonValue("anything"), schema, err));
}

TEST(McpTypesTest, ValidatorMinMaxProperties) {
    auto schema = JsonValue::Parse(R"({"minProperties":1,"maxProperties":2})");
    std::string err;
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue(JsonValue::object_tag), schema, err));
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"a":1})"), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"a":1,"b":2,"c":3})"), schema, err));
}

TEST(McpTypesTest, ValidatorPropertyNames) {
    auto schema = JsonValue::Parse(R"({"propertyNames":{"pattern":"^[a-z]+$"}})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"abc":1})"), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"ABC":1})"), schema, err));
}

TEST(McpTypesTest, ValidatorPatternProperties) {
    auto schema = JsonValue::Parse(
        R"({"patternProperties":{"^x-":{"type":"string"}}})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"x-a":"s"})"), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"x-a":1})"), schema, err));
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"other":1})"), schema, err));
}

TEST(McpTypesTest, ValidatorAdditionalSkipsPatternCovered) {
    auto schema = JsonValue::Parse(
        R"({"patternProperties":{"^x-":{}},"additionalProperties":false})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"x-a":1})"), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"other":1})"), schema, err));
}

TEST(McpTypesTest, ValidatorDependentRequired) {
    auto schema = JsonValue::Parse(
        R"({"dependentRequired":{"credit_card":["billing_address"]}})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"credit_card":1,"billing_address":2})"), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"credit_card":1})"), schema, err));
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"other":1})"), schema, err));
}

TEST(McpTypesTest, ValidatorDependentSchemas) {
    auto schema = JsonValue::Parse(
        R"({"dependentSchemas":{"a":{"required":["b"]}}})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"a":1,"b":2})"), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"a":1})"), schema, err));
}

TEST(McpTypesTest, ValidatorDependenciesCompat) {
    auto schema = JsonValue::Parse(
        R"({"dependencies":{"a":["b"],"c":{"required":["d"]}}})");
    std::string err;
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"a":1})"), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"c":1})"), schema, err));
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"a":1,"b":2,"c":1,"d":2})"), schema, err));
}

TEST(McpTypesTest, ValidatorContainsMinMax) {
    auto schema = JsonValue::Parse(
        R"({"contains":{"type":"integer"},"minContains":2,"maxContains":3})");
    std::string err;
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"([1,"a"])"), schema, err));
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"([1,2,"a"])"), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"([1,2,3,4])"), schema, err));
}

TEST(McpTypesTest, ValidatorUniqueItems) {
    auto schema = JsonValue::Parse(R"({"uniqueItems":true})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"([1,2,3])"), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"([1,2,1])"), schema, err));
}

TEST(McpTypesTest, ValidatorPrefixItems) {
    auto schema = JsonValue::Parse(
        R"({"prefixItems":[{"type":"string"},{"type":"integer"}],)"
        R"("items":false})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"(["a",1])"), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"(["a","b"])"), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"(["a",1,"extra"])"), schema, err));
}

TEST(McpTypesTest, ValidatorUnevaluatedPropertiesIgnored) {
    auto schema = JsonValue::Parse(
        R"({"properties":{"a":{"type":"string"}},"unevaluatedProperties":false})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"a":"x","b":1})"), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"a":1})"), schema, err));
    auto typed = JsonValue::Parse(R"({"unevaluatedProperties":{"type":"string"}})");
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"a":1})"), typed, err));
}

TEST(McpTypesTest, ValidatorUnevaluatedPropertiesIgnoredAcrossInPlaceApplicators) {
    auto schema = JsonValue::Parse(
        R"({"$defs":{"extra":{"type":"string"}},)"
        R"("allOf":[{"properties":{"a":{"type":"string"}},)"
        R"("unevaluatedProperties":false}],)"
        R"("anyOf":[{"properties":{"b":{"$ref":"#/$defs/extra"}}},)"
        R"({"properties":{"b":{"type":"integer"}}}],)"
        R"("dependentSchemas":{"c":{"properties":{"d":{"type":"string"}}}},)"
        R"("unevaluatedProperties":{"type":"integer"}})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"a":"s","b":2,"c":1,"d":"t","e":[1]})"), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"a":"s","b":2,"c":1,"d":5})"), schema, err));
}

TEST(McpTypesTest, ValidatorUnevaluatedItemsIgnored) {
    auto schema = JsonValue::Parse(R"({"unevaluatedItems":false})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"([1,2,3])"), schema, err));
    auto strict = JsonValue::Parse(
        R"({"prefixItems":[{"type":"string"},{"type":"integer"}],)"
        R"("unevaluatedItems":false})");
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"(["a",1])"), strict, err));
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"(["a",1,"extra"])"), strict, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"(["a","b"])"), strict, err));
}

TEST(McpTypesTest, ValidatorIntegerAcceptsIntegralDouble) {
    auto schema = JsonValue::Parse(R"({"type":"integer"})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(JsonValue(3.0), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(JsonValue(3.5), schema, err));
}

TEST(McpTypesTest, ValidatorMinLengthCountsCodePoints) {
    auto schema = JsonValue::Parse(R"({"minLength":2,"maxLength":2})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse("\"h\xC3\xA9\""), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(JsonValue("a"), schema, err));
}

TEST(McpTypesTest, ValidatorDynamicRefLocal) {
    auto schema = JsonValue::Parse(
        R"({"$defs":{"name":{"type":"string"}},)"
        R"("properties":{"n":{"$dynamicRef":"#/$defs/name"}}})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"n":"x"})"), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"n":1})"), schema, err));
}

TEST(McpTypesTest, ValidatorDynamicRefRemoteRejected) {
    auto schema = JsonValue::Parse(
        R"({"$dynamicRef":"https://example.com/s.json"})");
    std::string err;
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue(JsonValue::object_tag), schema, err));
    EXPECT_NE(err.find("remote"), std::string::npos);
}

TEST(McpTypesTest, ValidatorAnchorRef) {
    auto schema = JsonValue::Parse(
        R"({"$defs":{"target":{"$anchor":"tgt","type":"string"}},)"
        R"("properties":{"n":{"$ref":"#tgt"}}})");
    std::string err;
    EXPECT_TRUE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"n":"x"})"), schema, err));
    EXPECT_FALSE(detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"n":1})"), schema, err));
}

TEST(McpTypesTest, CallToolResultStructuredContentArbitraryPassthrough) {
    CallToolResult r;
    JsonValue arr(JsonValue::array_tag);
    arr.PushBack(JsonValue(int64_t(1)));
    arr.PushBack(JsonValue("two"));
    r.structured_content = arr;
    auto jv = SerializeCallToolResult(r);
    ASSERT_TRUE(jv["structuredContent"].IsArray());
    EXPECT_EQ(jv["structuredContent"].Size(), size_t{2});
    auto r2 = DeserializeCallToolResult(jv);
    ASSERT_TRUE(r2.structured_content.has_value());
    EXPECT_EQ(*r2.structured_content, arr);

    CallToolResult rs;
    rs.structured_content = JsonValue("plain");
    auto jvs = SerializeCallToolResult(rs);
    auto rs2 = DeserializeCallToolResult(jvs);
    ASSERT_TRUE(rs2.structured_content.has_value());
    EXPECT_EQ(rs2.structured_content->GetString(), "plain");
}
