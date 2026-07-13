#include <mcp/JsonRpc.hpp>

#include <gtest/gtest.h>

using namespace mcp;

// ── RequestId ──
TEST(JsonRpcTest, RequestIdIntRoundTrip) {
    RequestId id{int64_t(42)};
    nlohmann::json j;
    mcp::to_json(j, id);
    RequestId id2 = mcp::request_id_from_json(j);
    EXPECT_EQ(id, id2);
    EXPECT_TRUE(std::holds_alternative<int64_t>(id2));
}

TEST(JsonRpcTest, RequestIdStringRoundTrip) {
    RequestId id{std::string("abc")};
    nlohmann::json j;
    mcp::to_json(j, id);
    RequestId id2 = mcp::request_id_from_json(j);
    EXPECT_EQ(id, id2);
    EXPECT_TRUE(std::holds_alternative<std::string>(id2));
}

// ── JsonRpcRequest ──
TEST(JsonRpcTest, RequestRoundTrip) {
    JsonRpcRequest req;
    req.id = RequestId{int64_t(1)};
    req.method = "tools/list";
    req.params = nlohmann::json::object();

    nlohmann::json j = req;
    auto req2 = j.get<JsonRpcRequest>();

    EXPECT_EQ(req.jsonrpc, req2.jsonrpc);
    EXPECT_EQ(req.id, req2.id);
    EXPECT_EQ(req.method, req2.method);
    EXPECT_EQ(req.params, req2.params);
}

// ── JsonRpcNotification ──
TEST(JsonRpcTest, NotificationRoundTrip) {
    JsonRpcNotification notif;
    notif.method = "notifications/initialized";

    nlohmann::json j = notif;
    auto notif2 = j.get<JsonRpcNotification>();

    EXPECT_EQ(notif.method, notif2.method);
    EXPECT_FALSE(j.contains("id"));
}

// ── JsonRpcResponse ──
TEST(JsonRpcTest, ResponseRoundTrip) {
    JsonRpcResponse resp;
    resp.id = RequestId{int64_t(1)};
    resp.result = {{"tools", nlohmann::json::array()}};

    nlohmann::json j = resp;
    auto resp2 = j.get<JsonRpcResponse>();

    EXPECT_EQ(resp.id, resp2.id);
    EXPECT_EQ(resp.result, resp2.result);
}

// ── JsonRpcErrorResponse ──
TEST(JsonRpcTest, ErrorRoundTrip) {
    JsonRpcErrorResponse err;
    err.id = RequestId{int64_t(1)};
    err.error = {McpErrorCode::MethodNotFound, "method not found"};

    nlohmann::json j = err;
    auto err2 = j.get<JsonRpcErrorResponse>();

    EXPECT_EQ(err.id, err2.id);
    EXPECT_EQ(err.error.code, err2.error.code);
    EXPECT_EQ(err.error.message, err2.error.message);
}

// ── JsonRpcMessage 变体多态反序列化 ──
TEST(JsonRpcTest, MessageVariantDetectsRequest) {
    nlohmann::json j = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/list"},
        {"params", nlohmann::json::object()}
    };
    auto msg = j.get<JsonRpcMessage>();
    EXPECT_TRUE(IsRequest(msg));

    auto* req = AsRequest(msg);
    ASSERT_NE(req, nullptr);
    EXPECT_EQ(req->method, "tools/list");
}

TEST(JsonRpcTest, MessageVariantDetectsNotification) {
    nlohmann::json j = {
        {"jsonrpc", "2.0"},
        {"method", "notifications/initialized"}
    };
    auto msg = j.get<JsonRpcMessage>();
    EXPECT_TRUE(IsNotification(msg));
}

TEST(JsonRpcTest, MessageVariantDetectsResponse) {
    nlohmann::json j = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"result", {{"tools", nlohmann::json::array()}}}
    };
    auto msg = j.get<JsonRpcMessage>();
    EXPECT_TRUE(IsResponse(msg));
}

TEST(JsonRpcTest, MessageVariantDetectsError) {
    nlohmann::json j = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"error", {{"code", -32601}, {"message", "method not found"}}}
    };
    auto msg = j.get<JsonRpcMessage>();
    EXPECT_TRUE(IsError(msg));
}

TEST(JsonRpcTest, MessageVariantRoundTrip) {
    nlohmann::json j = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", {{"name", "test"}}}
    };
    auto msg = j.get<JsonRpcMessage>();
    nlohmann::json j2 = msg;
    EXPECT_EQ(j, j2);
}

// ── ErrorData ──
TEST(JsonRpcTest, ErrorDataRoundTrip) {
    ErrorData ed{McpErrorCode::InvalidParams, "invalid params"};
    nlohmann::json j = ed;
    auto ed2 = j.get<ErrorData>();
    EXPECT_EQ(ed.code, ed2.code);
    EXPECT_EQ(ed.message, ed2.message);
}
