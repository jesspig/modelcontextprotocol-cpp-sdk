#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/protocol/WireCodec.hpp>
#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/McpCore.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <future>

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

TEST(WireCodecTest, Rev2026RejectsLegacyTasks) {
    auto modern = MakeWireCodec("2026-07-28");
    auto legacy = MakeWireCodec("2025-11-25");
    const std::string_view kMethods[] = {
        "tasks/result", "tasks/list",
    };
    for (auto method : kMethods) {
        SCOPED_TRACE(method);
        EXPECT_FALSE(modern->HasRequestMethod(method));
        JsonValue raw(JsonValue::object_tag);
        JsonValue params(JsonValue::object_tag);
        params["_meta"] = JsonValue(JsonValue::object_tag);
        raw["params"] = std::move(params);
        EXPECT_EQ(modern->ValidateRequest(method, raw), WireValidation::NotInEra);
        EXPECT_TRUE(legacy->HasRequestMethod(method));
        EXPECT_EQ(legacy->ValidateRequest(method, raw), WireValidation::Ok);
    }
}

TEST(WireCodecTest, Rev2026AcceptsTaskExtensionMethods) {
    auto modern = MakeWireCodec("2026-07-28");
    auto legacy = MakeWireCodec("2025-11-25");
    const std::string_view kMethods[] = {
        "tasks/get", "tasks/update", "tasks/cancel",
    };
    for (auto method : kMethods) {
        SCOPED_TRACE(method);
        EXPECT_TRUE(modern->HasRequestMethod(method));
        EXPECT_TRUE(legacy->HasRequestMethod(method));
        JsonValue raw(JsonValue::object_tag);
        JsonValue params(JsonValue::object_tag);
        params["_meta"] = JsonValue(JsonValue::object_tag);
        raw["params"] = std::move(params);
        EXPECT_EQ(modern->ValidateRequest(method, raw), WireValidation::Ok);
        EXPECT_EQ(legacy->ValidateRequest(method, raw), WireValidation::Ok);
    }
    JsonValue bare(JsonValue::object_tag);
    bare["params"] = JsonValue(JsonValue::object_tag);
    EXPECT_EQ(modern->ValidateRequest("tasks/get", bare), WireValidation::Invalid);
}

TEST(WireCodecTest, Rev2026RejectsPing) {
    auto modern = MakeWireCodec("2026-07-28");
    auto legacy = MakeWireCodec("2025-11-25");
    const std::string_view kMethods[] = {
        "ping", "logging/setLevel", "resources/subscribe", "resources/unsubscribe",
    };
    for (auto method : kMethods) {
        SCOPED_TRACE(method);
        EXPECT_FALSE(modern->HasRequestMethod(method));
        JsonValue raw(JsonValue::object_tag);
        JsonValue params(JsonValue::object_tag);
        params["_meta"] = JsonValue(JsonValue::object_tag);
        raw["params"] = std::move(params);
        EXPECT_EQ(modern->ValidateRequest(method, raw), WireValidation::NotInEra);
        EXPECT_TRUE(legacy->HasRequestMethod(method));
        EXPECT_EQ(legacy->ValidateRequest(method, raw), WireValidation::Ok);
    }
}

TEST(WireCodecTest, TasksMcpNameMustEqualTaskId) {
    auto legacy = MakeWireCodec("2025-11-25");
    auto modern = MakeWireCodec("2026-07-28");
    const std::string_view kMethods[] = {
        "tasks/get", "tasks/update", "tasks/cancel",
    };
    for (auto method : kMethods) {
        SCOPED_TRACE(method);
        JsonValue params(JsonValue::object_tag);
        params["taskId"] = JsonValue("task-1");
        EXPECT_NO_THROW(legacy->ValidateTaskHeaders(method, params, "task-1"));
        EXPECT_NO_THROW(modern->ValidateTaskHeaders(method, params, "task-1"));
        try {
            legacy->ValidateTaskHeaders(method, params, "other");
            FAIL();
        } catch (const McpError& e) {
            EXPECT_EQ(static_cast<int32_t>(e.Code()),
                      static_cast<int32_t>(McpErrorCode::HeaderMismatch));
        }
        try {
            legacy->ValidateTaskHeaders(method, params, "");
            FAIL();
        } catch (const McpError& e) {
            EXPECT_EQ(static_cast<int32_t>(e.Code()),
                      static_cast<int32_t>(McpErrorCode::HeaderMismatch));
        }
        JsonValue missing(JsonValue::object_tag);
        try {
            legacy->ValidateTaskHeaders(method, missing, "task-1");
            FAIL();
        } catch (const McpError& e) {
            EXPECT_EQ(static_cast<int32_t>(e.Code()),
                      static_cast<int32_t>(McpErrorCode::InvalidParams));
        }
    }
    JsonValue params(JsonValue::object_tag);
    params["taskId"] = JsonValue("task-1");
    EXPECT_NO_THROW(legacy->ValidateTaskHeaders("tools/call", params, "other"));
    EXPECT_NO_THROW(
        legacy->ValidateTaskHeaders("tools/call", JsonValue(JsonValue::object_tag), ""));
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

TEST(WireCodecTest, ErrorDataRoundTrip) {
    const std::string versions[] = {
        std::string(kLegacyProtocolVersion), std::string(kLatestProtocolVersion)};
    for (const auto& version : versions) {
        SCOPED_TRACE(version);
        auto codec = MakeWireCodec(version);
        ASSERT_NE(codec, nullptr);
        EXPECT_EQ(codec->EncodeErrorCode(
            static_cast<int32_t>(McpErrorCode::MissingRequiredClientCapability)),
            static_cast<int32_t>(McpErrorCode::MissingRequiredClientCapability));

        JsonValue form(JsonValue::object_tag);
        JsonValue elicitation(JsonValue::object_tag);
        elicitation["form"] = std::move(form);
        JsonValue required(JsonValue::object_tag);
        required["elicitation"] = std::move(elicitation);
        JsonValue data(JsonValue::object_tag);
        data["requiredCapabilities"] = std::move(required);
        McpError error(McpErrorCode::MissingRequiredClientCapability,
            "Missing required client capabilities: elicitation", std::move(data));
        ASSERT_TRUE(error.Data().has_value());

        JsonRpcErrorResponse err_resp;
        err_resp.id = RequestId{int64_t(7)};
        err_resp.error.code = static_cast<McpErrorCode>(
            codec->EncodeErrorCode(static_cast<int32_t>(error.Code())));
        err_resp.error.message = std::string(error.what());
        if (error.Data()) err_resp.error.data = *error.Data();

        auto wire = JsonValue::Parse(SerializeMessage(JsonRpcMessage(err_resp)));
        EXPECT_EQ(wire["error"]["code"], JsonValue(int64_t(-32021)));
        ASSERT_TRUE(wire["error"].Contains("data"));
        EXPECT_TRUE(wire["error"]["data"]["requiredCapabilities"]
            ["elicitation"].Contains("form"));

        auto parsed = DeserializeMessage(SerializeMessage(JsonRpcMessage(err_resp)));
        auto* err2 = AsError(parsed);
        ASSERT_NE(err2, nullptr);
        EXPECT_EQ(err2->error.code, McpErrorCode::MissingRequiredClientCapability);
        EXPECT_EQ(err2->error.message,
            "Missing required client capabilities: elicitation");
        ASSERT_TRUE(err2->error.data.has_value());
        ASSERT_TRUE(err2->error.data->IsObject());
        const JsonValue* required2 = err2->error.data->Find("requiredCapabilities");
        ASSERT_NE(required2, nullptr);
        const JsonValue* elicitation2 = required2->Find("elicitation");
        ASSERT_NE(elicitation2, nullptr);
        EXPECT_TRUE(elicitation2->Contains("form"));
        EXPECT_FALSE(elicitation2->Contains("url"));
    }

    McpError plain(McpErrorCode::InternalError, "boom");
    EXPECT_FALSE(plain.Data().has_value());
    JsonRpcErrorResponse plain_resp;
    plain_resp.id = RequestId{int64_t(8)};
    plain_resp.error.code = plain.Code();
    plain_resp.error.message = std::string(plain.what());
    if (plain.Data()) plain_resp.error.data = *plain.Data();
    auto plain_wire = JsonValue::Parse(SerializeMessage(JsonRpcMessage(plain_resp)));
    EXPECT_FALSE(plain_wire["error"].Contains("data"));
}

TEST(WireCodecTest, Rev2026InitializeNotInEra) {
    auto modern = MakeWireCodec("2026-07-28");
    auto legacy = MakeWireCodec("2025-11-25");
    EXPECT_FALSE(modern->HasRequestMethod("initialize"));
    EXPECT_TRUE(legacy->HasRequestMethod("initialize"));

    JsonValue bare(JsonValue::object_tag);
    bare["method"] = JsonValue("initialize");
    EXPECT_EQ(modern->ValidateRequest("initialize", bare), WireValidation::NotInEra);

    JsonValue full(JsonValue::object_tag);
    full["method"] = JsonValue("initialize");
    JsonValue params(JsonValue::object_tag);
    params["protocolVersion"] = JsonValue("2026-07-28");
    params["capabilities"] = JsonValue(JsonValue::object_tag);
    params["clientInfo"] =
        SerializeImplementation(Implementation{"test-client", "1.0.0"});
    full["params"] = std::move(params);
    EXPECT_EQ(modern->ValidateRequest("initialize", full), WireValidation::NotInEra);
    EXPECT_EQ(legacy->ValidateRequest("initialize", full), WireValidation::Ok);
}

TEST(WireCodecTest, Rev2026InitializeModernNegotiatedRejected) {
    auto pair = InMemoryTransport::CreatePair();
    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    auto server = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();
    server->Start();
    server->SetNegotiatedProtocolVersion(kLatestProtocolVersion);
    bool handled = false;
    server->SetRequestHandler(methods::kInitialize,
        [&handled](const JsonRpcRequest&, std::promise<JsonValue> p) {
            handled = true;
            p.set_value(JsonValue(JsonValue::object_tag));
        });
    RequestMeta meta;
    meta.protocol_version = std::string(kLatestProtocolVersion);
    meta.client_info = Implementation{"wire-client", "1.0"};
    auto future = client->SendRequest(methods::kInitialize,
        JsonValue(JsonValue::object_tag), meta, std::chrono::milliseconds(2000));
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.Contains("code"));
    EXPECT_EQ(result["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::MethodNotFound));
    EXPECT_FALSE(handled);
    client->Close();
    server->Close();
}

TEST(WireCodecTest, Rev2026InitializeAllowedBeforeNegotiation) {
    auto pair = InMemoryTransport::CreatePair();
    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    auto server = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();
    server->Start();
    bool handled = false;
    server->SetRequestHandler(methods::kInitialize,
        [&handled](const JsonRpcRequest&, std::promise<JsonValue> p) {
            handled = true;
            JsonValue result(JsonValue::object_tag);
            result["protocolVersion"] = JsonValue(std::string(kLegacyProtocolVersion));
            result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
            result["serverInfo"] =
                SerializeImplementation(Implementation{"raw-server", "1.0"});
            p.set_value(std::move(result));
        });
    RequestMeta meta;
    meta.protocol_version = std::string(kLatestProtocolVersion);
    meta.client_info = Implementation{"wire-client", "1.0"};
    auto future = client->SendRequest(methods::kInitialize,
        JsonValue(JsonValue::object_tag), meta, std::chrono::milliseconds(2000));
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    EXPECT_FALSE(result.Contains("code"));
    EXPECT_TRUE(handled);
    client->Close();
    server->Close();
}

TEST(WireCodecTest, Rev2026MissingMetaRejectedAsInvalidRequest) {
    auto pair = InMemoryTransport::CreatePair();
    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    auto server = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();
    server->Start();
    bool handled = false;
    server->SetRequestHandler(methods::kListTools,
        [&handled](const JsonRpcRequest&, std::promise<JsonValue> p) {
            handled = true;
            p.set_value(JsonValue(JsonValue::object_tag));
        });
    RequestMeta legacy_meta;
    legacy_meta.protocol_version = std::string(kLegacyProtocolVersion);
    auto future = client->SendRequest(methods::kListTools,
        JsonValue(JsonValue::object_tag), legacy_meta, std::chrono::milliseconds(2000));
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.Contains("code"));
    EXPECT_EQ(result["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::InvalidRequest));
    EXPECT_FALSE(handled);
    client->Close();
    server->Close();
}

TEST(WireCodecTest, Rev2026UnsupportedDeclaredVersionRejected) {
    auto pair = InMemoryTransport::CreatePair();
    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    auto server = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();
    server->Start();
    bool handled = false;
    server->SetRequestHandler(methods::kCallTool,
        [&handled](const JsonRpcRequest&, std::promise<JsonValue> p) {
            handled = true;
            p.set_value(JsonValue(JsonValue::object_tag));
        });
    RequestMeta meta;
    meta.protocol_version = "2099-01-01";
    meta.client_info = Implementation{"wire-client", "1.0"};
    auto future = client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), meta, std::chrono::milliseconds(2000));
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.Contains("code"));
    EXPECT_EQ(result["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::UnsupportedProtocolVersion));
    EXPECT_FALSE(handled);
    ASSERT_TRUE(result.Contains("data"));
    const JsonValue* supported = result["data"].Find("supported");
    ASSERT_NE(supported, nullptr);
    bool lists_latest = false;
    for (const auto& v : supported->GetArray()) {
        if (v.IsString() && v.GetString() == std::string(kLatestProtocolVersion))
            lists_latest = true;
    }
    EXPECT_TRUE(lists_latest);
    const JsonValue* requested = result["data"].Find("requested");
    ASSERT_NE(requested, nullptr);
    EXPECT_EQ(requested->GetString(), "2099-01-01");
    client->Close();
    server->Close();
}

TEST(WireCodecTest, Rev2026SamplingCreateMessageNotInEra) {
    auto modern = MakeWireCodec("2026-07-28");
    auto legacy = MakeWireCodec("2025-11-25");
    EXPECT_FALSE(modern->HasRequestMethod(methods::kCreateMessage));
    EXPECT_TRUE(legacy->HasRequestMethod(methods::kCreateMessage));

    auto pair = InMemoryTransport::CreatePair();
    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    auto server = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();
    server->Start();
    server->SetNegotiatedProtocolVersion(kLatestProtocolVersion);
    bool handled = false;
    server->SetRequestHandler(methods::kCreateMessage,
        [&handled](const JsonRpcRequest&, std::promise<JsonValue> p) {
            handled = true;
            p.set_value(JsonValue(JsonValue::object_tag));
        });
    RequestMeta meta;
    meta.protocol_version = std::string(kLatestProtocolVersion);
    meta.client_info = Implementation{"wire-client", "1.0"};
    auto future = client->SendRequest(methods::kCreateMessage,
        JsonValue(JsonValue::object_tag), meta, std::chrono::milliseconds(2000));
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.Contains("code"));
    EXPECT_EQ(result["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::MethodNotFound));
    EXPECT_FALSE(handled);
    client->Close();
    server->Close();
}

TEST(WireCodecTest, VerifyCapabilityObjectShape) {
    const std::pair<std::string_view, std::string_view> cases[] = {
        {methods::kCreateMessage, "sampling"},
        {methods::kListRoots, "roots"},
    };
    for (const auto& [method, key] : cases) {
        SCOPED_TRACE(std::string(method));
        auto pair = InMemoryTransport::CreatePair();
        auto client = std::make_shared<McpSessionHandler>(
            std::move(pair.client), MakeWireCodec(std::string(kLegacyProtocolVersion)));
        auto server = std::make_shared<McpSessionHandler>(
            std::move(pair.server), MakeWireCodec(std::string(kLegacyProtocolVersion)));
        client->Start();
        server->Start();
        server->SetRequestHandler(method,
            [](const JsonRpcRequest&, std::promise<JsonValue> p) {
                p.set_value(JsonValue(JsonValue::object_tag));
            });
        auto future = client->SendRequest(
            method, JsonValue(JsonValue::object_tag), {}, std::chrono::milliseconds(2000));
        ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
        auto result = future.get();
        ASSERT_TRUE(result.Contains("code"));
        EXPECT_EQ(result["code"].GetInt(), int64_t(-32021));
        ASSERT_TRUE(result.Contains("data"));
        ASSERT_TRUE(result["data"].IsObject());
        const JsonValue* required = result["data"].Find("requiredCapabilities");
        ASSERT_NE(required, nullptr);
        EXPECT_TRUE(required->IsObject());
        const JsonValue* cap = required->Find(key);
        ASSERT_NE(cap, nullptr);
        EXPECT_TRUE(cap->IsObject());
        client->Close();
        server->Close();
    }
}
