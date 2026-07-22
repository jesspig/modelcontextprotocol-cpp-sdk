#include <mcp/McpTypes.hpp>

#include <gtest/gtest.h>

using namespace mcp;

// ── Tool ──
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

// ── Resource ──
TEST(McpTypesTest, ResourceRoundTrip) {
    Resource r;
    r.uri = "file:///data";
    r.name = "Data File";

    auto jv = SerializeResource(r);
    auto r2 = DeserializeResource(jv);

    EXPECT_EQ(r.uri, r2.uri);
    EXPECT_EQ(r.name, r2.name);
}

// ── Prompt ──
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

// ── Implementation ──
TEST(McpTypesTest, ImplementationRoundTrip) {
    Implementation impl;
    impl.name = "test-server";
    impl.version = "1.0.0";

    auto jv = SerializeImplementation(impl);
    auto impl2 = DeserializeImplementation(jv);

    EXPECT_EQ(impl.name, impl2.name);
    EXPECT_EQ(impl.version, impl2.version);
}

// ── CallToolResult ──
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

// ── CallToolResult with isError ──
TEST(McpTypesTest, CallToolResultWithError) {
    CallToolResult r;
    r.is_error = true;

    auto jv = SerializeCallToolResult(r);
    auto r2 = DeserializeCallToolResult(jv);

    EXPECT_TRUE(r2.is_error);
}

// ── ListToolsResult ──
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

// ── Content variants ──
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

// ── ServerCapabilities ──
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

// ── ClientCapabilities ──
TEST(McpTypesTest, ClientCapabilitiesRoundTrip) {
    ClientCapabilities caps;
    caps.elicitation = ElicitationCapability{};
    caps.elicitation->form = JsonValue(JsonValue::object_tag);

    auto jv = SerializeClientCapabilities(caps);
    auto caps2 = DeserializeClientCapabilities(jv);

    EXPECT_TRUE(caps2.elicitation.has_value());
}

// ── DiscoverResult ──
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

// ── InitializeResult ──
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

// ── GetPromptResult with PromptMessage ──
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

// ── ResourceContents ──
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

// ── ReadResourceResult ──
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

// ── LoggingLevel ──
TEST(McpTypesTest, LoggingLevelRoundTrip) {
    auto jv = SerializeLoggingLevel(LoggingLevel::Warning);
    EXPECT_EQ(jv.GetString(), "warning");

    auto l = DeserializeLoggingLevel(jv);
    EXPECT_EQ(l, LoggingLevel::Warning);
}

// ── RequestMeta ──
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

// ── CallToolRequestParams ──
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

// ── EmbeddedResource ──
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

// ── EmptyResult ──
TEST(McpTypesTest, EmptyResultSerializes) {
    EmptyResult r;
    auto jv = SerializeEmptyResult(r);
    auto r2 = DeserializeEmptyResult(jv);
    SUCCEED();
}

// ── CompleteResult ──
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

// ── ResourceTemplate ──
TEST(McpTypesTest, ResourceTemplateRoundTrip) {
    ResourceTemplate rt;
    rt.uri_template = "file:///{path}";
    rt.name = "Dynamic Files";
    auto jv = SerializeResourceTemplate(rt);
    auto rt2 = DeserializeResourceTemplate(jv);
    EXPECT_EQ(rt2.uri_template, "file:///{path}");
}

// ── Icon ──
TEST(McpTypesTest, IconRoundTrip) {
    Icon ic{"https://example.com/icon.svg", "image/svg+xml", std::vector<std::string>{"any"}};
    auto jv = SerializeIcon(ic);
    auto ic2 = DeserializeIcon(jv);
    EXPECT_EQ(ic2.src, "https://example.com/icon.svg");
}
