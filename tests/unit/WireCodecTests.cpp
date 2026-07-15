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

TEST(WireCodecTest, FactoryFallbackForUnknown) {
    auto codec = MakeWireCodec("2024-01-01");
    ASSERT_NE(codec, nullptr);
    EXPECT_EQ(codec->Era(), "2025-11-25");
}

// ── 2025 codec ──
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

TEST(WireCodecTest, Rev2025StampIsNoop) {
    auto codec = MakeWireCodec("2025-11-25");
    nlohmann::json body = nlohmann::json::object();
    RequestMeta meta;
    meta.protocol_version = "2025-11-25";
    codec->StampOutgoingRequest(body, meta);
    EXPECT_TRUE(body.empty());  // no _meta added
}

// ── 2026 codec ──
TEST(WireCodecTest, Rev2026HasRequestMethod) {
    auto codec = MakeWireCodec("2026-07-28");
    EXPECT_TRUE(codec->HasRequestMethod("tools/list"));
    EXPECT_TRUE(codec->HasRequestMethod("server/discover"));
    EXPECT_TRUE(codec->HasRequestMethod("subscriptions/listen"));
    EXPECT_FALSE(codec->HasRequestMethod("initialize"));
    EXPECT_FALSE(codec->HasRequestMethod("logging/setLevel"));
}

TEST(WireCodecTest, Rev2026Validates_MetaRequired) {
    auto codec = MakeWireCodec("2026-07-28");

    // tools/call without _meta → Invalid
    nlohmann::json body = {{"name", "test"}};
    EXPECT_EQ(codec->ValidateRequest("tools/call", body),
              WireValidation::Invalid);

    // server/discover doesn't require _meta
    EXPECT_EQ(codec->ValidateRequest("server/discover", body),
              WireValidation::Ok);
}

TEST(WireCodecTest, Rev2026StampAddsMeta) {
    auto codec = MakeWireCodec("2026-07-28");
    nlohmann::json body = nlohmann::json::object();

    RequestMeta meta;
    meta.protocol_version = "2026-07-28";
    meta.client_info = Implementation{"test-client", "1.0.0"};
    meta.client_capabilities = ClientCapabilities{};

    codec->StampOutgoingRequest(body, meta);

    ASSERT_TRUE(body.contains("_meta"));
    EXPECT_EQ(body["_meta"]["io.modelcontextprotocol/protocolVersion"],
              "2026-07-28");
    EXPECT_EQ(
        body["_meta"]["io.modelcontextprotocol/clientInfo"]["name"],
        "test-client");
    EXPECT_TRUE(
        body["_meta"].contains(
            "io.modelcontextprotocol/clientCapabilities"));
}

TEST(WireCodecTest, Rev2026ExtractMeta) {
    auto codec = MakeWireCodec("2026-07-28");
    nlohmann::json body;
    body["_meta"] = nlohmann::json::object();
    body["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2026-07-28";
    body["_meta"]["io.modelcontextprotocol/clientInfo"] =
        Implementation{"cli", "2.0"};

    auto meta = codec->ExtractIncomingMeta(body);
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->protocol_version, "2026-07-28");
    ASSERT_TRUE(meta->client_info.has_value());
    EXPECT_EQ(meta->client_info->name, "cli");
}

TEST(WireCodecTest, Rev2026EncodeResult) {
    auto codec = MakeWireCodec("2026-07-28");
    nlohmann::json result = {{"content", nlohmann::json::array()}};
    auto encoded = codec->EncodeResult("tools/call", result);
    EXPECT_EQ(encoded["resultType"], "complete");
    EXPECT_TRUE(encoded.contains("content"));
}

TEST(WireCodecTest, Rev2026ErrorCodeRemapping) {
    auto codec = MakeWireCodec("2026-07-28");
    EXPECT_EQ(codec->EncodeErrorCode(-32001), -32020);
    EXPECT_EQ(codec->EncodeErrorCode(-32003), -32021);
    EXPECT_EQ(codec->EncodeErrorCode(-32004), -32022);
    EXPECT_EQ(codec->EncodeErrorCode(-32601), -32601);  // unchanged
}

// ── JsonRpcRequest with _meta ──
TEST(WireCodecTest, JsonRpcRequestWithMetaRoundTrip) {
    JsonRpcRequest req;
    req.id = RequestId{int64_t(1)};
    req.method = "tools/call";
    req.params = nlohmann::json{{"name", "echo"}};
    req.meta = nlohmann::json{
        {"io.modelcontextprotocol/protocolVersion", "2026-07-28"}
    };

    nlohmann::json j = req;
    EXPECT_EQ(j["_meta"]["io.modelcontextprotocol/protocolVersion"],
              "2026-07-28");

    auto req2 = j.get<JsonRpcRequest>();
    ASSERT_TRUE(req2.meta.has_value());
    EXPECT_EQ((*req2.meta)["io.modelcontextprotocol/protocolVersion"],
              "2026-07-28");
}
