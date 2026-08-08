// WireCodecTests — unit tests for WireCodec factory, era-gating, and _meta handling

#include <mcp/protocol/WireCodec.hpp>
#include <mcp/McpCore.hpp>

#include <gtest/gtest.h>

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

TEST(WireCodecTest, Rev2026ExtractMeta) {
    auto codec = MakeWireCodec("2026-07-28");
    JsonValue body(JsonValue::object_tag);
    body["_meta"] = JsonValue(JsonValue::object_tag);
    body["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2026-07-28";
    body["_meta"]["io.modelcontextprotocol/clientInfo"] =
        SerializeImplementation(Implementation{"cli", "2.0"});

    auto meta = codec->ExtractIncomingMeta(body);
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->protocol_version, "2026-07-28");
    ASSERT_TRUE(meta->client_info.has_value());
    EXPECT_EQ(meta->client_info->name, "cli");
}

TEST(WireCodecTest, Rev2026EncodeResult) {
    auto codec = MakeWireCodec("2026-07-28");
    JsonValue result(JsonValue::object_tag);
    result["content"] = JsonValue(JsonValue::array_tag);
    auto encoded = codec->EncodeResult("tools/call", result);
    EXPECT_EQ(encoded["resultType"], "complete");
    EXPECT_TRUE(encoded.Contains("content"));
}

// ── 2026 notification method membership ──
TEST(WireCodecTest, Rev2026HasTaskAndSubscriptionNotifications) {
    auto codec = MakeWireCodec("2026-07-28");
    EXPECT_TRUE(codec->HasNotificationMethod("notifications/tasks/status"));
    EXPECT_TRUE(codec->HasNotificationMethod("notifications/tasks/working"));
    EXPECT_TRUE(codec->HasNotificationMethod("notifications/tasks/completed"));
    EXPECT_TRUE(codec->HasNotificationMethod("notifications/tasks/failed"));
    EXPECT_TRUE(codec->HasNotificationMethod("notifications/tasks/cancelled"));
    EXPECT_TRUE(codec->HasNotificationMethod("notifications/tasks/input_required"));
    EXPECT_TRUE(codec->HasNotificationMethod("notifications/subscriptions/acknowledged"));
    EXPECT_FALSE(codec->HasNotificationMethod("notifications/initialized"));
    EXPECT_FALSE(codec->HasNotificationMethod("notifications/message"));
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
