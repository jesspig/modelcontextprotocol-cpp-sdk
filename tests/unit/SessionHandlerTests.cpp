// SessionHandlerTests — behavior tests for the McpSessionHandler protocol engine:
// 2026-era _meta round-trip, request validation wiring, timeouts, cancellation,
// and the error-path future contract.

#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/protocol/MessageFilter.hpp>
#include <mcp/protocol/WireCodec.hpp>
#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/McpError.hpp>
#include <mcp/Methods.hpp>

#include <mcp/test/McpTest.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

using namespace mcp;

namespace {

// Pair of session handlers connected via InMemoryTransport, both on the 2026 era.
struct HandlerPair {
    std::shared_ptr<McpSessionHandler> client;
    std::shared_ptr<McpSessionHandler> server;

    explicit HandlerPair(std::string_view era = kLatestProtocolVersion) {
        auto pair = InMemoryTransport::CreatePair();
        client = std::make_shared<McpSessionHandler>(
            std::move(pair.client), MakeWireCodec(era));
        server = std::make_shared<McpSessionHandler>(
            std::move(pair.server), MakeWireCodec(era));
        client->Start();
        server->Start();
    }

    ~HandlerPair() {
        client->Close();
        server->Close();
    }
};

RequestMeta ModernMeta() {
    RequestMeta meta;
    meta.protocol_version = std::string(kLatestProtocolVersion);
    meta.client_info = Implementation{"meta-client", "1.0"};
    return meta;
}

} // namespace

// 2026 era: a request sent with top-level _meta is received by the peer,
// whose ExtractIncomingMeta yields the protocolVersion.
TEST(SessionHandlerTest, IncomingMetaCarriesProtocolVersion) {
    HandlerPair hp;

    std::promise<std::string> meta_promise;
    auto meta_future = meta_promise.get_future();
    hp.server->SetRequestHandler(methods::kCallTool,
        [srv = hp.server.get(), &meta_promise](
            const JsonRpcRequest& req, std::promise<JsonValue> p) {
            auto meta = srv->ExtractIncomingMeta(req);
            meta_promise.set_value(meta.protocol_version);
            p.set_value(JsonValue(JsonValue::object_tag));
        });

    auto future = hp.client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), ModernMeta(),
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_EQ(meta_future.get(), std::string(kLatestProtocolVersion));
    EXPECT_EQ(future.get()["resultType"], JsonValue("complete"));
}

// 2026 era: request methods removed from the era are rejected with
// MethodNotFound (ValidateRequest wired into OnRequest).
TEST(SessionHandlerTest, UnnegotiatedMethodRejected2026) {
    HandlerPair hp;
    hp.server->SetRequestHandler(methods::kPing,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            p.set_value(JsonValue(JsonValue::object_tag));
        });

    auto future = hp.client->SendRequest("logging/setLevel",
        JsonValue(JsonValue::object_tag), ModernMeta(),
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.Contains("code"));
    EXPECT_EQ(result["code"].GetInt(), static_cast<int64_t>(McpErrorCode::MethodNotFound));
}

// Regression guard: ping is a 2025-only method, so the 2026 era must
// reject it with MethodNotFound (wire method set excludes ping).
TEST(SessionHandlerTest, PingRejectedIn2026) {
    HandlerPair hp;
    hp.server->SetRequestHandler(methods::kPing,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            p.set_value(JsonValue(JsonValue::object_tag));
        });

    auto future = hp.client->SendRequest(methods::kPing,
        JsonValue(JsonValue::object_tag), ModernMeta(),
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.Contains("code"));
    EXPECT_EQ(result["code"].GetInt(), static_cast<int64_t>(McpErrorCode::MethodNotFound));
}

// Ping remains available in the 2025 era: the handler is invoked and the
// request completes without an error payload.
TEST(SessionHandlerTest, PingAvailableIn2025) {
    HandlerPair hp(kLegacyProtocolVersion);
    hp.server->SetRequestHandler(methods::kPing,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            p.set_value(JsonValue(JsonValue::object_tag));
        });

    RequestMeta meta = ModernMeta();
    meta.protocol_version = std::string(kLegacyProtocolVersion);
    auto future = hp.client->SendRequest(methods::kPing,
        JsonValue(JsonValue::object_tag), meta,
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    EXPECT_FALSE(result.Contains("code"));
}

// A request that is never answered resolves via CheckTimeouts with a
// RequestTimeout error JSON payload.
TEST(SessionHandlerTest, CheckTimeoutsResolvesWithRequestTimeout) {
    HandlerPair hp;

    // Keep the server-side promise alive so the request never completes.
    std::shared_ptr<std::promise<JsonValue>> held_promise;
    hp.server->SetRequestHandler(methods::kCallTool,
        [&held_promise](const JsonRpcRequest&, std::promise<JsonValue> p) {
            held_promise =
                std::make_shared<std::promise<JsonValue>>(std::move(p));
        });

    auto future = hp.client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), ModernMeta(),
        std::chrono::milliseconds(300));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.Contains("code"));
    EXPECT_EQ(result["code"].GetInt(), static_cast<int64_t>(McpErrorCode::RequestTimeout));
    EXPECT_EQ(result["message"].GetString(), std::string("request timed out"));
}

// notifications/cancelled is handled hard-coded before the handler map
// lookup: the notification arrives, but a registered cancelled handler
// must not be invoked.
TEST(SessionHandlerTest, CancelledNotificationNotRoutedToHandlerMap) {
    HandlerPair hp;

    std::promise<void> observed_promise;
    auto observed_future = observed_promise.get_future();
    bool cancelled_handler_called = false;

    hp.client->SetNotificationHandler(notifications::kCancelled,
        [&cancelled_handler_called](const JsonRpcNotification&) {
            cancelled_handler_called = true;
        });
    hp.client->SetOnNotificationCallback(
        [&observed_promise](const JsonRpcNotification&) {
            observed_promise.set_value();
        });

    JsonValue params(JsonValue::object_tag);
    params["requestId"] = JsonValue(int64_t(42));
    params["reason"] = JsonValue("test");
    hp.server->SendNotification(notifications::kCancelled, std::move(params));

    ASSERT_EQ(observed_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    // Give the (hard-coded) cancelled dispatch path a bounded window to run,
    // then assert the user handler was never invoked.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline && !cancelled_handler_called) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_FALSE(cancelled_handler_called);
}

// A server error response satisfies the request future with a
// {code, message} JSON payload instead of throwing.
TEST(SessionHandlerTest, ErrorResponseSatisfiesFutureContract) {
    HandlerPair hp;

    hp.server->SetRequestHandler(methods::kCallTool,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            p.set_exception(std::make_exception_ptr(
                McpError(McpErrorCode::MethodNotFound, "boom")));
        });

    auto future = hp.client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), ModernMeta(),
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.Contains("code"));
    EXPECT_EQ(result["code"].GetInt(), static_cast<int64_t>(McpErrorCode::MethodNotFound));
    EXPECT_EQ(result["message"].GetString(), std::string("boom"));
}

// A progress notification carrying a matching progressToken extends the
// deadline of the pending request by kProgressTimeoutExtension.
TEST(SessionHandlerTest, ProgressNotificationExtendsDeadline) {
    HandlerPair hp;

    std::shared_ptr<std::promise<JsonValue>> held_promise;
    hp.server->SetRequestHandler(methods::kCallTool,
        [&held_promise](const JsonRpcRequest&, std::promise<JsonValue> p) {
            held_promise =
                std::make_shared<std::promise<JsonValue>>(std::move(p));
        });

    // Client-side progress handling: a progress notification with a matching
    // token resets the pending request deadline (mirrors McpServer wiring).
    hp.client->SetNotificationHandler(notifications::kProgress,
        [client = hp.client.get()](const JsonRpcNotification& notif) {
            if (notif.params && notif.params->IsObject()) {
                auto* pt = notif.params->Find("progressToken");
                if (pt && pt->IsString()) {
                    client->ResetTimeoutByProgressToken(pt->GetString());
                }
            }
        });

    RequestMeta meta = ModernMeta();
    meta.progress_token = std::string("pt-1");
    auto future = hp.client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), meta,
        std::chrono::milliseconds(2000));

    // Server reports progress 1s in, before the 2s deadline.
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    JsonValue progress_params(JsonValue::object_tag);
    progress_params["progressToken"] = JsonValue("pt-1");
    hp.server->SendNotification(notifications::kProgress, std::move(progress_params));

    // Past the original deadline the request must still be pending.
    EXPECT_EQ(future.wait_for(std::chrono::milliseconds(1500)),
              std::future_status::timeout);

    // Completing the handler satisfies the request normally.
    held_promise->set_value(JsonValue(JsonValue::object_tag));
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    EXPECT_FALSE(result.Contains("code"));
}

// A request dropped by an incoming filter never reaches the handler map;
// the sender observes a request timeout instead.
TEST(SessionHandlerTest, IncomingFilterInterceptsRequests) {
    auto pair = InMemoryTransport::CreatePair();
    auto pipeline = std::make_shared<FilterPipeline>();
    pipeline->AddFilter(std::make_shared<MessageFilterFuncAdapter>(
        [](const JsonRpcMessage& msg, MessageFilterNext next) {
            if (const auto* req = std::get_if<JsonRpcRequest>(&msg)) {
                if (req->method == "blocked") return;  // drop, do not forward
            }
            next(msg);
        }));
    auto server = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(std::string(kLatestProtocolVersion)),
        pipeline, nullptr);
    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(std::string(kLatestProtocolVersion)));
    client->Start();
    server->Start();

    bool handler_called = false;
    server->SetRequestHandler("blocked",
        [&handler_called](const JsonRpcRequest&, std::promise<JsonValue> p) {
            handler_called = true;
            p.set_value(JsonValue(JsonValue::object_tag));
        });

    auto future = client->SendRequest("blocked",
        JsonValue(JsonValue::object_tag), ModernMeta(),
        std::chrono::milliseconds(500));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.Contains("code"));
    EXPECT_EQ(result["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::RequestTimeout));
    EXPECT_FALSE(handler_called);

    client->Close();
    server->Close();
}
