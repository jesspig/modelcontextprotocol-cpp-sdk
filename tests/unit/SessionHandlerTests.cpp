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
#include <set>
#include <thread>

using namespace mcp;

namespace {

constexpr char kMetaSubscriptionIdKeyLiteral[] = "io.modelcontextprotocol/subscriptionId";

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

// Concurrent requests (half synchronous handlers, half async via worker)
// must each resolve with the response correlated to their own request id.
TEST(SessionHandlerTest, ConcurrentRequestsResolveCorrectly) {
    HandlerPair hp;
    constexpr int kCount = 16;

    hp.server->SetRequestHandler(methods::kCallTool,
        [](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            const int64_t id = std::get<int64_t>(req.id);
            if (id % 2 == 0) {
                std::thread([p = std::move(p), id]() mutable {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    JsonValue result(JsonValue::object_tag);
                    result["echo"] = JsonValue(id);
                    p.set_value(std::move(result));
                }).detach();
            } else {
                JsonValue result(JsonValue::object_tag);
                result["echo"] = JsonValue(id);
                p.set_value(std::move(result));
            }
        });

    std::vector<std::future<JsonValue>> futures;
    futures.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        futures.push_back(hp.client->SendRequest(methods::kCallTool,
            JsonValue(JsonValue::object_tag), ModernMeta(),
            std::chrono::milliseconds(2000)));
    }

    for (int i = 0; i < kCount; ++i) {
        ASSERT_EQ(futures[i].wait_for(std::chrono::seconds(5)),
                  std::future_status::ready);
        auto result = futures[i].get();
        ASSERT_TRUE(result.Contains("echo"));
        EXPECT_EQ(result["echo"].GetInt(), static_cast<int64_t>(i + 1));
    }
}

// Close() must fail a request whose server handler never completes and must
// join the response worker without blocking on the unsatisfied promise.
TEST(SessionHandlerTest, CloseCompletesHeldRequestsAndJoinsWorker) {
    HandlerPair hp;

    std::shared_ptr<std::promise<JsonValue>> held_promise;
    hp.server->SetRequestHandler(methods::kCallTool,
        [&held_promise](const JsonRpcRequest&, std::promise<JsonValue> p) {
            held_promise = std::make_shared<std::promise<JsonValue>>(std::move(p));
        });

    auto future = hp.client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), ModernMeta(),
        std::chrono::milliseconds(5000));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    hp.client->Close();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.Contains("code"));
    EXPECT_EQ(result["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::ConnectionClosed));

    hp.server->Close();
    held_promise->set_value(JsonValue(JsonValue::object_tag));
}

// An integer progress token (int64 variant of ProgressToken) must extend the
// deadline of the matching pending request, mirroring the string-token path.
TEST(SessionHandlerTest, IntProgressTokenExtendsDeadline) {
    HandlerPair hp;

    std::shared_ptr<std::promise<JsonValue>> held_promise;
    hp.server->SetRequestHandler(methods::kCallTool,
        [&held_promise](const JsonRpcRequest&, std::promise<JsonValue> p) {
            held_promise = std::make_shared<std::promise<JsonValue>>(std::move(p));
        });

    hp.client->SetNotificationHandler(notifications::kProgress,
        [client = hp.client.get()](const JsonRpcNotification& notif) {
            if (notif.params && notif.params->IsObject()) {
                auto* pt = notif.params->Find("progressToken");
                if (pt && pt->IsInt()) {
                    client->ResetTimeoutByProgressToken(std::to_string(pt->GetInt()));
                }
            }
        });

    RequestMeta meta = ModernMeta();
    meta.progress_token = int64_t(7);
    auto future = hp.client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), meta,
        std::chrono::milliseconds(1000));

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    JsonValue progress_params(JsonValue::object_tag);
    progress_params["progressToken"] = JsonValue(int64_t(7));
    hp.server->SendNotification(notifications::kProgress, std::move(progress_params));

    EXPECT_EQ(future.wait_for(std::chrono::milliseconds(800)),
              std::future_status::timeout);

    held_promise->set_value(JsonValue(JsonValue::object_tag));
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_FALSE(future.get().Contains("code"));
}

// SetNegotiatedProtocolVersion is safe while the session is running: after a
// mid-flight era switch the server answers requests with the new codec.
TEST(SessionHandlerTest, SetNegotiatedProtocolVersionDuringRun) {
    auto pair = InMemoryTransport::CreatePair();
    auto client = std::make_shared<McpSessionHandler>(
        std::move(pair.client), MakeWireCodec(kLegacyProtocolVersion));
    auto server = std::make_shared<McpSessionHandler>(
        std::move(pair.server), MakeWireCodec(kLegacyProtocolVersion));
    client->Start();
    server->Start();

    server->SetNegotiatedProtocolVersion(kLatestProtocolVersion);

    server->SetRequestHandler(methods::kCallTool,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            p.set_value(JsonValue(JsonValue::object_tag));
        });

    auto future = client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), ModernMeta(),
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    EXPECT_EQ(result["resultType"].GetString(), std::string("complete"));
    EXPECT_FALSE(result.Contains("code"));

    client->Close();
    server->Close();
}

// NotifySubscribers shares one message skeleton across matching subscriptions:
// every subscriber receives the same method/params and its own subscriptionId
// in _meta (io.modelcontextprotocol/subscriptionId).
TEST(SessionHandlerTest, NotifySubscribersBroadcastsSharedPayloadWithPerSubscriptionMeta) {
    HandlerPair hp;

    std::promise<void> received_two;
    auto received_two_future = received_two.get_future();
    std::vector<JsonRpcNotification> received;
    hp.client->SetNotificationHandler(notifications::kToolListChanged,
        [&received, &received_two](const JsonRpcNotification& notif) {
            received.push_back(notif);
            if (received.size() == 2) received_two.set_value();
        });

    SubscriptionEntry sub_a;
    sub_a.id = "sub-a";
    sub_a.filter.tools_list_changed = true;
    SubscriptionEntry sub_b;
    sub_b.id = "sub-b";
    sub_b.filter.tools_list_changed = true;
    hp.server->AddSubscriptionEntry(std::move(sub_a));
    hp.server->AddSubscriptionEntry(std::move(sub_b));

    JsonValue params(JsonValue::object_tag);
    params["serverName"] = JsonValue("test-server");
    hp.server->NotifySubscribers(notifications::kToolListChanged, std::move(params));

    ASSERT_EQ(received_two_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    ASSERT_EQ(received.size(), 2u);

    std::set<std::string> ids;
    for (const auto& notif : received) {
        EXPECT_EQ(notif.method, notifications::kToolListChanged);
        ASSERT_TRUE(notif.params);
        auto* name = notif.params->Find("serverName");
        ASSERT_TRUE(name);
        EXPECT_EQ(name->GetString(), std::string("test-server"));
        ASSERT_TRUE(notif.meta);
        auto* sid = notif.meta->Find(kMetaSubscriptionIdKeyLiteral);
        ASSERT_TRUE(sid);
        ids.insert(sid->GetString());
    }
    EXPECT_EQ(ids, (std::set<std::string>{"sub-a", "sub-b"}));
}

// Only subscriptions whose resource filter matches the updated uri are notified.
TEST(SessionHandlerTest, NotifySubscribersHonorsResourceFilter) {
    HandlerPair hp;

    std::promise<void> received;
    auto received_future = received.get_future();
    std::vector<std::string> sub_ids;
    hp.client->SetNotificationHandler(notifications::kResourceUpdated,
        [&sub_ids, &received](const JsonRpcNotification& notif) {
            if (notif.meta) {
                if (auto* sid = notif.meta->Find(kMetaSubscriptionIdKeyLiteral)) {
                    sub_ids.push_back(sid->GetString());
                }
            }
            if (sub_ids.size() == 1) received.set_value();
        });

    SubscriptionEntry sub_a;
    sub_a.id = "sub-a";
    sub_a.filter.resource_subscriptions = {"uri-a"};
    SubscriptionEntry sub_b;
    sub_b.id = "sub-b";
    sub_b.filter.resource_subscriptions = {"uri-b"};
    SubscriptionEntry sub_c;
    sub_c.id = "sub-c";
    sub_c.filter.tools_list_changed = true;
    hp.server->AddSubscriptionEntry(std::move(sub_a));
    hp.server->AddSubscriptionEntry(std::move(sub_b));
    hp.server->AddSubscriptionEntry(std::move(sub_c));

    JsonValue params(JsonValue::object_tag);
    params["uri"] = JsonValue("uri-a");
    hp.server->NotifySubscribers(notifications::kResourceUpdated,
        std::move(params), std::string("uri-a"));

    ASSERT_EQ(received_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    ASSERT_EQ(sub_ids.size(), 1u);
    EXPECT_EQ(sub_ids[0], std::string("sub-a"));
}

// Event notifications echo the client-provided subscriptionId (entry.session_id)
// when one was captured from the listen request, falling back to the server id.
TEST(SessionHandlerTest, NotifySubscribersPrefersClientSubscriptionId) {
    HandlerPair hp;

    std::promise<std::string> sid_promise;
    auto sid_future = sid_promise.get_future();
    hp.client->SetNotificationHandler(notifications::kToolListChanged,
        [&sid_promise](const JsonRpcNotification& notif) {
            if (notif.meta) {
                if (auto* sid = notif.meta->Find(kMetaSubscriptionIdKeyLiteral)) {
                    sid_promise.set_value(sid->GetString());
                }
            }
        });

    SubscriptionEntry entry;
    entry.id = "server-1";
    entry.session_id = "client-sub-9";
    entry.filter.tools_list_changed = true;
    hp.server->AddSubscriptionEntry(std::move(entry));

    JsonValue params(JsonValue::object_tag);
    hp.server->NotifySubscribers(notifications::kToolListChanged, std::move(params));

    ASSERT_EQ(sid_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    EXPECT_EQ(sid_future.get(), std::string("client-sub-9"));
}
