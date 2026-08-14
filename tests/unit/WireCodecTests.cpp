// WireCodecTests — unit tests for WireCodec factory, era-gating, and _meta handling

#include <mcp/protocol/WireCodec.hpp>
#include <mcp/McpCore.hpp>

#include <mcp/test/McpTest.hpp>

using namespace mcp;

// ── WireCodec factory ──
TEST(WireCodecTest, FactoryReturns2025ForLegacy) {
    auto codec = MakeWireCodec("2025-11-25");
    ASSERT_NE(codec, nullptr);
    EXPECT_EQ(codec->Era(), "2025-11-25");
}

TEST(WireCodecTest, FactoryReturns2026ForModern) {
    auto codec = MakeWireCodec("2026-07-28");
    ASSERT_NE(codec, nullptr);
    EXPECT_EQ(codec->Era(), "2026-07-28");
}

// ── Factory version comparison boundaries ──
TEST(WireCodecTest, FactoryBoundaryJustBeforeLatest) {
    auto codec = MakeWireCodec("2026-07-27");
    ASSERT_NE(codec, nullptr);
    EXPECT_EQ(codec->Era(), "2025-11-25");
}

TEST(WireCodecTest, FactoryBoundaryPrefixGreaterThanLatest) {
    auto codec = MakeWireCodec("2026-08");
    ASSERT_NE(codec, nullptr);
    EXPECT_EQ(codec->Era(), "2026-07-28");
}

TEST(WireCodecTest, FactoryBoundaryExactLatest) {
    auto codec = MakeWireCodec("2026-07-28");
    ASSERT_NE(codec, nullptr);
    EXPECT_EQ(codec->Era(), "2026-07-28");
}

// ── 2025-era codec ──
TEST(WireCodecTest, Rev2025HasRequestMethod) {
    auto codec = MakeWireCodec("2025-11-25");
    EXPECT_TRUE(codec->HasRequestMethod("tools/list"));
    EXPECT_TRUE(codec->HasRequestMethod("tools/call"));
    EXPECT_TRUE(codec->HasRequestMethod("initialize"));
    EXPECT_TRUE(codec->HasRequestMethod("ping"));
    EXPECT_FALSE(codec->HasRequestMethod("server/discover"));
}

TEST(WireCodecTest, Rev2025HasNotificationMethod) {
    auto codec = MakeWireCodec("2025-11-25");
    EXPECT_TRUE(codec->HasNotificationMethod("notifications/initialized"));
    EXPECT_TRUE(codec->HasNotificationMethod("notifications/cancelled"));
    EXPECT_TRUE(codec->HasNotificationMethod("notifications/progress"));
}

// ── 2025-era initialize request validation ──
TEST(WireCodecTest, Rev2025ValidateInitializeRequest) {
    auto codec = MakeWireCodec("2025-11-25");

    JsonValue raw(JsonValue::object_tag);
    raw["jsonrpc"] = JsonValue("2.0");
    raw["id"] = JsonValue(int64_t(1));
    raw["method"] = JsonValue("initialize");
    JsonValue params(JsonValue::object_tag);
    params["protocolVersion"] = JsonValue("2025-11-25");
    params["capabilities"] = JsonValue(JsonValue::object_tag);
    params["clientInfo"] =
        SerializeImplementation(Implementation{"test-client", "1.0.0"});
    raw["params"] = std::move(params);

    EXPECT_EQ(codec->ValidateRequest("initialize", raw), WireValidation::Ok);
}

TEST(WireCodecTest, Rev2025ValidateInitializeRequestMissingProtocolVersion) {
    auto codec = MakeWireCodec("2025-11-25");

    JsonValue raw(JsonValue::object_tag);
    raw["jsonrpc"] = JsonValue("2.0");
    raw["id"] = JsonValue(int64_t(1));
    raw["method"] = JsonValue("initialize");
    JsonValue params(JsonValue::object_tag);
    params["capabilities"] = JsonValue(JsonValue::object_tag);
    params["clientInfo"] = JsonValue(JsonValue::object_tag);
    raw["params"] = std::move(params);

    EXPECT_EQ(codec->ValidateRequest("initialize", raw), WireValidation::Invalid);
}

// ── 2026-era codec ──
TEST(WireCodecTest, Rev2026StampAddsMeta) {
    auto codec = MakeWireCodec("2026-07-28");
    JsonValue body(JsonValue::object_tag);

    RequestMeta meta;
    meta.protocol_version = "2026-07-28";
    meta.client_info = Implementation{"test-client", "1.0.0"};
    meta.client_capabilities = ClientCapabilities{};

    codec->StampOutgoingRequest(body, meta);

    ASSERT_TRUE(body.Contains("_meta"));
    EXPECT_EQ(body["_meta"]["io.modelcontextprotocol/protocolVersion"],
              "2026-07-28");
    EXPECT_EQ(
        body["_meta"]["io.modelcontextprotocol/clientInfo"]["name"],
        "test-client");
    EXPECT_TRUE(
        body["_meta"].Contains(
            "io.modelcontextprotocol/clientCapabilities"));
}

TEST(WireCodecTest, Rev2026EncodeResult) {
    auto codec = MakeWireCodec("2026-07-28");
    JsonValue result(JsonValue::object_tag);
    result["content"] = JsonValue(JsonValue::array_tag);
    auto encoded = codec->EncodeResult("tools/call", result);
    EXPECT_EQ(encoded["resultType"], "complete");
    EXPECT_TRUE(encoded.Contains("content"));
}

// ── 2026 flattens nested cacheHint onto the result top level ──
TEST(WireCodecTest, Rev2026EncodeResultFlattensCacheHint) {
    auto codec = MakeWireCodec("2026-07-28");
    JsonValue result(JsonValue::object_tag);
    result["content"] = JsonValue(JsonValue::array_tag);
    JsonValue hint(JsonValue::object_tag);
    hint["ttlMs"] = JsonValue(int64_t(60000));
    hint["cacheScope"] = JsonValue("public");
    result["cacheHint"] = std::move(hint);
    auto encoded = codec->EncodeResult("tools/list", result);
    EXPECT_EQ(encoded["resultType"], "complete");
    EXPECT_EQ(encoded["ttlMs"], JsonValue(int64_t(60000)));
    EXPECT_EQ(encoded["cacheScope"], "public");
    EXPECT_FALSE(encoded.Contains("cacheHint"));
    EXPECT_TRUE(encoded.Contains("content"));
}

// ── 2025 keeps the nested cacheHint shape untouched ──
TEST(WireCodecTest, Rev2025EncodeResultKeepsCacheHintNested) {
    auto codec = MakeWireCodec("2025-11-25");
    JsonValue result(JsonValue::object_tag);
    JsonValue hint(JsonValue::object_tag);
    hint["ttlMs"] = JsonValue(int64_t(60000));
    result["cacheHint"] = std::move(hint);
    auto encoded = codec->EncodeResult("tools/list", result);
    EXPECT_FALSE(encoded.Contains("ttlMs"));
    ASSERT_TRUE(encoded.Contains("cacheHint"));
    EXPECT_EQ(encoded["cacheHint"]["ttlMs"], JsonValue(int64_t(60000)));
}

// ── 2026 notification method membership ──
TEST(WireCodecTest, Rev2026HasMessageAndSubscriptionNotificationsNoTasks) {
    auto codec = MakeWireCodec("2026-07-28");
    EXPECT_TRUE(codec->HasNotificationMethod("notifications/message"));
    EXPECT_TRUE(codec->HasNotificationMethod("notifications/subscriptions/acknowledged"));
    EXPECT_FALSE(codec->HasNotificationMethod("notifications/tasks/status"));
    EXPECT_FALSE(codec->HasNotificationMethod("notifications/tasks/working"));
    EXPECT_FALSE(codec->HasNotificationMethod("notifications/tasks/completed"));
    EXPECT_FALSE(codec->HasNotificationMethod("notifications/tasks/failed"));
    EXPECT_FALSE(codec->HasNotificationMethod("notifications/tasks/cancelled"));
    EXPECT_FALSE(codec->HasNotificationMethod("notifications/tasks/input_required"));
    EXPECT_FALSE(codec->HasNotificationMethod("notifications/initialized"));
}

// ── 2025-era notification method membership ──
TEST(WireCodecTest, Rev2025HasTaskStatusNotification) {
    auto codec = MakeWireCodec("2025-11-25");
    EXPECT_TRUE(codec->HasNotificationMethod("notifications/tasks/status"));
    EXPECT_TRUE(codec->HasNotificationMethod("notifications/message"));
    EXPECT_FALSE(codec->HasNotificationMethod("notifications/subscriptions/acknowledged"));
}

// ── 2026 response validation ──
TEST(WireCodecTest, Rev2026ValidateResponse) {
    auto codec = MakeWireCodec("2026-07-28");

    JsonValue bare(JsonValue::object_tag);
    EXPECT_EQ(codec->ValidateResponse("tools/list", bare),
              WireValidation::Invalid);

    JsonValue complete(JsonValue::object_tag);
    complete["resultType"] = "complete";
    EXPECT_EQ(codec->ValidateResponse("tools/list", complete),
              WireValidation::Ok);
    EXPECT_EQ(codec->ValidateResponse("tools/call", complete),
              WireValidation::Ok);

    JsonValue input_required(JsonValue::object_tag);
    input_required["resultType"] = "input_required";
    EXPECT_EQ(codec->ValidateResponse("tools/list", input_required),
              WireValidation::Invalid);
    EXPECT_EQ(codec->ValidateResponse("tools/call", input_required),
              WireValidation::Ok);
}

// ── 2026 notification validation ──
TEST(WireCodecTest, Rev2026ValidateNotification) {
    auto codec = MakeWireCodec("2026-07-28");

    JsonValue clean(JsonValue::object_tag);
    clean["params"] = JsonValue(JsonValue::object_tag);
    EXPECT_EQ(codec->ValidateNotification("notifications/progress", clean),
              WireValidation::Ok);

    JsonValue with_id(JsonValue::object_tag);
    with_id["id"] = JsonValue(int64_t(1));
    EXPECT_EQ(codec->ValidateNotification("notifications/progress", with_id),
              WireValidation::Invalid);

    JsonValue with_error(JsonValue::object_tag);
    with_error["error"] = JsonValue(JsonValue::object_tag);
    EXPECT_EQ(codec->ValidateNotification("notifications/progress", with_error),
              WireValidation::Invalid);
}

// ── JsonRpcRequest with _meta ──
TEST(WireCodecTest, JsonRpcRequestWithMetaRoundTrip) {
    JsonRpcRequest req;
    req.id = RequestId{int64_t(1)};
    req.method = "tools/call";
    req.params = JsonValue(JsonValue::object_tag);
    (*req.params)["name"] = "echo";
    req.meta = JsonValue(JsonValue::object_tag);
    (*req.meta)["io.modelcontextprotocol/protocolVersion"] = "2026-07-28";

    auto json_str = SerializeMessage(JsonRpcMessage(req));
    auto parsed = DeserializeMessage(json_str);
    const auto& req2 = std::get<JsonRpcRequest>(parsed);
    ASSERT_TRUE(req2.meta.has_value());
    EXPECT_EQ((*req2.meta)["io.modelcontextprotocol/protocolVersion"],
              "2026-07-28");
}
