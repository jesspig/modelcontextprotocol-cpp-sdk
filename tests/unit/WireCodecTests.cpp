#include <mcp/protocol/WireCodec.hpp>
#include <mcp/McpCore.hpp>

#include <mcp/test/McpTest.hpp>

using namespace mcp;

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

TEST(WireCodecTest, Rev2026StampAddsMetaInsideParams) {
    auto codec = MakeWireCodec("2026-07-28");
    JsonValue body(JsonValue::object_tag);

    RequestMeta meta;
    meta.protocol_version = "2026-07-28";
    meta.client_info = Implementation{"test-client", "1.0.0"};
    meta.client_capabilities = ClientCapabilities{};

    codec->StampOutgoingRequest(body, meta);

    ASSERT_TRUE(body.Contains("params"));
    ASSERT_TRUE(body["params"].Contains("_meta"));
    EXPECT_FALSE(body.Contains("_meta"));
    EXPECT_EQ(body["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"],
              "2026-07-28");
    EXPECT_EQ(
        body["params"]["_meta"]["io.modelcontextprotocol/clientInfo"]["name"],
        "test-client");
    EXPECT_TRUE(
        body["params"]["_meta"].Contains(
            "io.modelcontextprotocol/clientCapabilities"));
}

TEST(WireCodecTest, Rev2026ValidateRequestRequiresMetaInsideParams) {
    auto codec = MakeWireCodec("2026-07-28");

    JsonValue ok(JsonValue::object_tag);
    ok["jsonrpc"] = JsonValue("2.0");
    ok["id"] = JsonValue(int64_t(1));
    ok["method"] = JsonValue("tools/list");
    JsonValue params(JsonValue::object_tag);
    params["_meta"] = JsonValue(JsonValue::object_tag);
    ok["params"] = std::move(params);
    EXPECT_EQ(codec->ValidateRequest("tools/list", ok), WireValidation::Ok);

    JsonValue top_level_meta(JsonValue::object_tag);
    top_level_meta["jsonrpc"] = JsonValue("2.0");
    top_level_meta["id"] = JsonValue(int64_t(1));
    top_level_meta["method"] = JsonValue("tools/list");
    top_level_meta["params"] = JsonValue(JsonValue::object_tag);
    top_level_meta["_meta"] = JsonValue(JsonValue::object_tag);
    EXPECT_EQ(codec->ValidateRequest("tools/list", top_level_meta),
              WireValidation::Invalid);

    JsonValue missing_params(JsonValue::object_tag);
    missing_params["jsonrpc"] = JsonValue("2.0");
    missing_params["id"] = JsonValue(int64_t(1));
    missing_params["method"] = JsonValue("tools/list");
    missing_params["_meta"] = JsonValue(JsonValue::object_tag);
    EXPECT_EQ(codec->ValidateRequest("tools/list", missing_params),
              WireValidation::Invalid);

    JsonValue discover(JsonValue::object_tag);
    discover["jsonrpc"] = JsonValue("2.0");
    discover["id"] = JsonValue(int64_t(1));
    discover["method"] = JsonValue("server/discover");
    EXPECT_EQ(codec->ValidateRequest("server/discover", discover),
              WireValidation::Ok);
}

TEST(WireCodecTest, Rev2026EncodeResult) {
    auto codec = MakeWireCodec("2026-07-28");
    JsonValue result(JsonValue::object_tag);
    result["content"] = JsonValue(JsonValue::array_tag);
    auto encoded = codec->EncodeResult("tools/call", result);
    EXPECT_EQ(encoded["resultType"], "complete");
    EXPECT_TRUE(encoded.Contains("content"));
}

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

TEST(WireCodecTest, Rev2025HasTaskStatusNotification) {
    auto codec = MakeWireCodec("2025-11-25");
    EXPECT_TRUE(codec->HasNotificationMethod("notifications/tasks/status"));
    EXPECT_TRUE(codec->HasNotificationMethod("notifications/message"));
    EXPECT_FALSE(codec->HasNotificationMethod("notifications/subscriptions/acknowledged"));
}

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

TEST(WireCodecTest, JsonRpcRequestWithMetaRoundTrip) {
    JsonRpcRequest req;
    req.id = RequestId{int64_t(1)};
    req.method = "tools/call";
    req.params = JsonValue(JsonValue::object_tag);
    (*req.params)["name"] = "echo";
    req.meta = JsonValue(JsonValue::object_tag);
    (*req.meta)["io.modelcontextprotocol/protocolVersion"] = "2026-07-28";

    auto json_str = SerializeMessage(JsonRpcMessage(req));

    auto wire = JsonValue::Parse(json_str);
    EXPECT_FALSE(wire.Contains("_meta"));
    ASSERT_TRUE(wire["params"].Contains("_meta"));
    EXPECT_EQ(wire["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"],
              "2026-07-28");
    EXPECT_TRUE(wire["params"].Contains("name"));

    auto parsed = DeserializeMessage(json_str);
    const auto& req2 = std::get<JsonRpcRequest>(parsed);
    ASSERT_TRUE(req2.meta.has_value());
    EXPECT_EQ((*req2.meta)["io.modelcontextprotocol/protocolVersion"],
              "2026-07-28");
    ASSERT_TRUE(req2.params);
    EXPECT_FALSE(req2.params->Contains("_meta"));
    EXPECT_EQ((*req2.params)["name"], "echo");
}

TEST(WireCodecTest, JsonRpcRequestWithoutParamsSynthesizesParamsForMeta) {
    JsonRpcRequest req;
    req.id = RequestId{int64_t(1)};
    req.method = "tools/list";
    req.meta = JsonValue(JsonValue::object_tag);
    (*req.meta)["io.modelcontextprotocol/protocolVersion"] = "2026-07-28";

    auto json_str = SerializeMessage(JsonRpcMessage(req));

    auto wire = JsonValue::Parse(json_str);
    ASSERT_TRUE(wire["params"].Contains("_meta"));
    EXPECT_EQ(wire["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"],
              "2026-07-28");

    auto parsed = DeserializeMessage(json_str);
    const auto& req2 = std::get<JsonRpcRequest>(parsed);
    ASSERT_TRUE(req2.meta.has_value());
    ASSERT_TRUE(req2.params);
    EXPECT_TRUE(req2.params->GetObject().empty());
}

TEST(WireCodecTest, JsonRpcNotificationWithMetaRoundTrip) {
    JsonRpcNotification notif;
    notif.method = "notifications/message";
    notif.params = JsonValue(JsonValue::object_tag);
    (*notif.params)["level"] = "info";
    notif.meta = JsonValue(JsonValue::object_tag);
    (*notif.meta)["io.modelcontextprotocol/protocolVersion"] = "2026-07-28";

    auto json_str = SerializeMessage(JsonRpcMessage(notif));

    auto wire = JsonValue::Parse(json_str);
    EXPECT_FALSE(wire.Contains("_meta"));
    ASSERT_TRUE(wire["params"].Contains("_meta"));

    auto parsed = DeserializeMessage(json_str);
    const auto& notif2 = std::get<JsonRpcNotification>(parsed);
    ASSERT_TRUE(notif2.meta.has_value());
    EXPECT_EQ((*notif2.meta)["io.modelcontextprotocol/protocolVersion"],
              "2026-07-28");
    ASSERT_TRUE(notif2.params);
    EXPECT_FALSE(notif2.params->Contains("_meta"));
    EXPECT_EQ((*notif2.params)["level"], "info");
}
