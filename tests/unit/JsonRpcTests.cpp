#include <mcp/JsonRpc.hpp>

#include <gtest/gtest.h>

using namespace mcp;

// ── RequestId ──
TEST(JsonRpcTest, RequestIdIntRoundTrip) {
    RequestId id{int64_t(42)};
    auto jv = RequestIdToJson(id);
    RequestId id2 = RequestIdFromJson(jv);
    EXPECT_EQ(id, id2);
    EXPECT_TRUE(std::holds_alternative<int64_t>(id2));
}

TEST(JsonRpcTest, RequestIdStringRoundTrip) {
    RequestId id{std::string("abc")};
    auto jv = RequestIdToJson(id);
    RequestId id2 = RequestIdFromJson(jv);
    EXPECT_EQ(id, id2);
    EXPECT_TRUE(std::holds_alternative<std::string>(id2));
}

// ── JsonRpcRequest ──
TEST(JsonRpcTest, RequestRoundTrip) {
    JsonRpcRequest req;
    req.id = RequestId{int64_t(1)};
    req.method = "tools/list";
    req.params = JsonValue(JsonValue::object_tag);

    auto json_str = SerializeMessage(JsonRpcMessage(req));
    auto msg2 = DeserializeMessage(json_str);
    auto* req2 = AsRequest(msg2);
    ASSERT_NE(req2, nullptr);
    EXPECT_EQ(req.jsonrpc, req2->jsonrpc);
    EXPECT_EQ(req.id, req2->id);
    EXPECT_EQ(req.method, req2->method);
    EXPECT_EQ(req.params, req2->params);
}

// ── JsonRpcNotification ──
TEST(JsonRpcTest, NotificationRoundTrip) {
    JsonRpcNotification notif;
    notif.method = "notifications/initialized";

    auto json_str = SerializeMessage(JsonRpcMessage(notif));
    EXPECT_EQ(json_str.find("\"id\""), std::string::npos);
    auto msg2 = DeserializeMessage(json_str);
    auto* notif2 = AsNotification(msg2);
    ASSERT_NE(notif2, nullptr);
    EXPECT_EQ(notif.method, notif2->method);
}

// ── JsonRpcResponse ──
TEST(JsonRpcTest, ResponseRoundTrip) {
    JsonRpcResponse resp;
    resp.id = RequestId{int64_t(1)};
    resp.result = JsonValue(JsonValue::object_tag);
    resp.result["tools"] = JsonValue(JsonValue::array_tag);

    auto json_str = SerializeMessage(JsonRpcMessage(resp));
    auto msg2 = DeserializeMessage(json_str);
    auto* resp2 = AsResponse(msg2);
    ASSERT_NE(resp2, nullptr);
    EXPECT_EQ(resp.id, resp2->id);
    EXPECT_EQ(resp.result, resp2->result);
}

// ── JsonRpcErrorResponse ──
TEST(JsonRpcTest, ErrorRoundTrip) {
    JsonRpcErrorResponse err;
    err.id = RequestId{int64_t(1)};
    err.error = {McpErrorCode::MethodNotFound, "method not found"};

    auto json_str = SerializeMessage(JsonRpcMessage(err));
    auto msg2 = DeserializeMessage(json_str);
    auto* err2 = AsError(msg2);
    ASSERT_NE(err2, nullptr);
    EXPECT_EQ(err.id, err2->id);
    EXPECT_EQ(err.error.code, err2->error.code);
    EXPECT_EQ(err.error.message, err2->error.message);
}

// ── JsonRpcMessage variant dispatch ──
TEST(JsonRpcTest, MessageVariantDetectsRequest) {
    auto msg = DeserializeMessage(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})");
    EXPECT_TRUE(IsRequest(msg));

    auto* req = AsRequest(msg);
    ASSERT_NE(req, nullptr);
    EXPECT_EQ(req->method, "tools/list");
}

TEST(JsonRpcTest, MessageVariantDetectsNotification) {
    auto msg = DeserializeMessage(
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    EXPECT_TRUE(IsNotification(msg));
}

TEST(JsonRpcTest, MessageVariantDetectsResponse) {
    auto msg = DeserializeMessage(
        R"({"jsonrpc":"2.0","id":1,"result":{"tools":[]}})");
    EXPECT_TRUE(IsResponse(msg));
}

TEST(JsonRpcTest, MessageVariantDetectsError) {
    auto msg = DeserializeMessage(
        R"({"jsonrpc":"2.0","id":1,"error":{"code":-32601,"message":"method not found"}})");
    EXPECT_TRUE(IsError(msg));
}

TEST(JsonRpcTest, MessageVariantRoundTrip) {
    auto msg = DeserializeMessage(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"test"}})");
    auto json_str = SerializeMessage(msg);
    auto msg2 = DeserializeMessage(json_str);
    EXPECT_EQ(SerializeMessage(msg), SerializeMessage(msg2));
}

// ── ErrorData ──
TEST(JsonRpcTest, ErrorDataRoundTrip) {
    ErrorData ed{McpErrorCode::InvalidParams, "invalid params"};
    JsonRpcErrorResponse err;
    err.id = RequestId{int64_t(1)};
    err.error = ed;
    auto json_str = SerializeMessage(JsonRpcMessage(err));
    auto msg2 = DeserializeMessage(json_str);
    auto* err2 = AsError(msg2);
    ASSERT_NE(err2, nullptr);
    EXPECT_EQ(ed.code, err2->error.code);
    EXPECT_EQ(ed.message, err2->error.message);
}
