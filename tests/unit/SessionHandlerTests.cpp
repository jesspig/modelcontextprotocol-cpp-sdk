#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/protocol/MessageFilter.hpp>
#include <mcp/protocol/WireCodec.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/McpError.hpp>
#include <mcp/Methods.hpp>

#include <mcp/test/McpTest.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <set>
#include <thread>

using namespace mcp;

namespace {

constexpr char kMetaSubscriptionIdKeyLiteral[] = "io.modelcontextprotocol/subscriptionId";

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

TEST(SessionHandlerTest, CheckTimeoutsResolvesWithRequestTimeout) {
    HandlerPair hp;

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
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline && !cancelled_handler_called) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_FALSE(cancelled_handler_called);
}

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

TEST(SessionHandlerTest, ProgressNotificationExtendsDeadline) {
    HandlerPair hp;

    std::shared_ptr<std::promise<JsonValue>> held_promise;
    hp.server->SetRequestHandler(methods::kCallTool,
        [&held_promise](const JsonRpcRequest&, std::promise<JsonValue> p) {
            held_promise =
                std::make_shared<std::promise<JsonValue>>(std::move(p));
        });

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

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    JsonValue progress_params(JsonValue::object_tag);
    progress_params["progressToken"] = JsonValue("pt-1");
    hp.server->SendNotification(notifications::kProgress, std::move(progress_params));

    EXPECT_EQ(future.wait_for(std::chrono::milliseconds(1500)),
              std::future_status::timeout);

    held_promise->set_value(JsonValue(JsonValue::object_tag));
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    EXPECT_FALSE(result.Contains("code"));
}

TEST(SessionHandlerTest, LegacyEraRequestCarriesProgressTokenInParams) {
    HandlerPair hp(kLegacyProtocolVersion);
    hp.server->SetNegotiatedProtocolVersion(kLegacyProtocolVersion);

    std::promise<JsonRpcRequest> req_promise;
    auto req_future = req_promise.get_future();
    hp.server->SetRequestHandler(methods::kCallTool,
        [&req_promise](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            req_promise.set_value(req);
            p.set_value(JsonValue(JsonValue::object_tag));
        });

    RequestMeta meta = ModernMeta();
    meta.protocol_version = std::string(kLegacyProtocolVersion);
    meta.progress_token = int64_t(1);
    auto future = hp.client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), meta,
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto req = req_future.get();
    ASSERT_TRUE(req.params);
    auto* legacy_meta = req.params->Find("_meta");
    ASSERT_TRUE(legacy_meta);
    auto* pt = legacy_meta->Find("progressToken");
    ASSERT_TRUE(pt);
    EXPECT_EQ(*pt, JsonValue(int64_t(1)));
    EXPECT_FALSE(req.meta);
}

TEST(SessionHandlerTest, ModernEraRequestCarriesProgressTokenInParamsMetaOnWire) {
    HandlerPair hp;

    std::promise<JsonRpcRequest> req_promise;
    auto req_future = req_promise.get_future();
    hp.server->SetRequestHandler(methods::kCallTool,
        [&req_promise](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            req_promise.set_value(req);
            p.set_value(JsonValue(JsonValue::object_tag));
        });

    RequestMeta meta = ModernMeta();
    meta.progress_token = int64_t(1);
    auto future = hp.client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), meta,
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto req = req_future.get();

    ASSERT_TRUE(req.meta);
    auto* pt = req.meta->Find("progressToken");
    ASSERT_TRUE(pt);
    EXPECT_EQ(*pt, JsonValue(int64_t(1)));
    ASSERT_TRUE(req.params);
    EXPECT_FALSE(req.params->Contains("_meta"));

    auto wire = JsonValue::Parse(SerializeMessage(JsonRpcMessage(req)));
    EXPECT_FALSE(wire.Contains("_meta"));
    ASSERT_TRUE(wire["params"].Contains("_meta"));
    EXPECT_EQ(wire["params"]["_meta"]["progressToken"], JsonValue(int64_t(1)));
    EXPECT_EQ(
        wire["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"],
        std::string(kLatestProtocolVersion));
    future.get();
}

TEST(SessionHandlerTest, IncomingFilterInterceptsRequests) {
    auto pair = InMemoryTransport::CreatePair();
    auto pipeline = std::make_shared<FilterPipeline>();
    pipeline->AddFilter(std::make_shared<MessageFilterFuncAdapter>(
        [](const JsonRpcMessage& msg, MessageFilterNext next) {
            if (const auto* req = std::get_if<JsonRpcRequest>(&msg)) {
                if (req->method == "blocked") return;
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

TEST(SessionHandlerTest, SlowHandlerDoesNotDelayReadyResponse) {
    HandlerPair hp;

    std::shared_ptr<std::promise<JsonValue>> held_promise;
    hp.server->SetRequestHandler(methods::kCallTool,
        [&held_promise](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            const int64_t id = std::get<int64_t>(req.id);
            if (id == 1) {
                held_promise = std::make_shared<std::promise<JsonValue>>(std::move(p));
                return;
            }
            JsonValue result(JsonValue::object_tag);
            result["echo"] = JsonValue(id);
            p.set_value(std::move(result));
        });

    auto slow = hp.client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), ModernMeta(), std::chrono::milliseconds(5000));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto fast = hp.client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), ModernMeta(), std::chrono::milliseconds(5000));

    ASSERT_EQ(fast.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto fast_result = fast.get();
    ASSERT_TRUE(fast_result.Contains("echo"));
    EXPECT_EQ(fast_result["echo"].GetInt(), static_cast<int64_t>(2));
    EXPECT_EQ(slow.wait_for(std::chrono::milliseconds(0)),
              std::future_status::timeout);

    held_promise->set_value(JsonValue(JsonValue::object_tag));
    ASSERT_EQ(slow.wait_for(std::chrono::seconds(2)), std::future_status::ready);
}

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

TEST(SessionHandlerTest, MaxTotalTimeoutCapsProgressExtensions) {
    HandlerPair hp;
    hp.client->SetMaxTotalTimeout(std::chrono::seconds(2));

    std::shared_ptr<std::promise<JsonValue>> held_promise;
    hp.server->SetRequestHandler(methods::kCallTool,
        [&held_promise](const JsonRpcRequest&, std::promise<JsonValue> p) {
            held_promise =
                std::make_shared<std::promise<JsonValue>>(std::move(p));
        });

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
    meta.progress_token = std::string("pt-cap");
    auto start = std::chrono::steady_clock::now();
    auto future = hp.client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), meta,
        std::chrono::milliseconds(500));

    int extensions = 0;
    while (future.wait_for(std::chrono::milliseconds(200)) ==
           std::future_status::timeout) {
        ++extensions;
        JsonValue progress_params(JsonValue::object_tag);
        progress_params["progressToken"] = JsonValue("pt-cap");
        hp.server->SendNotification(notifications::kProgress,
            std::move(progress_params));
        ASSERT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start).count(), 5000);
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_GE(extensions, 3);
    EXPECT_GE(elapsed_ms, static_cast<int64_t>(1500));
    auto result = future.get();
    ASSERT_TRUE(result.Contains("code"));
    EXPECT_EQ(result["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::RequestTimeout));
    EXPECT_EQ(result["message"].GetString(), std::string("request timed out"));
}

TEST(SessionHandlerTest, TotalTimeoutDisabledAllowsProgressExtension) {
    HandlerPair hp;

    std::shared_ptr<std::promise<JsonValue>> held_promise;
    hp.server->SetRequestHandler(methods::kCallTool,
        [&held_promise](const JsonRpcRequest&, std::promise<JsonValue> p) {
            held_promise =
                std::make_shared<std::promise<JsonValue>>(std::move(p));
        });

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
    meta.progress_token = std::string("pt-free");
    auto future = hp.client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), meta,
        std::chrono::milliseconds(800));

    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        JsonValue progress_params(JsonValue::object_tag);
        progress_params["progressToken"] = JsonValue("pt-free");
        hp.server->SendNotification(notifications::kProgress,
            std::move(progress_params));
    }

    EXPECT_EQ(future.wait_for(std::chrono::milliseconds(300)),
              std::future_status::timeout);

    held_promise->set_value(JsonValue(JsonValue::object_tag));
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_FALSE(future.get().Contains("code"));
}

TEST(SessionHandlerTest, MaxTotalTimeoutDoesNotAffectPlainIdleTimeout) {
    HandlerPair hp;
    hp.client->SetMaxTotalTimeout(std::chrono::seconds(1));

    std::shared_ptr<std::promise<JsonValue>> held_promise;
    hp.server->SetRequestHandler(methods::kCallTool,
        [&held_promise](const JsonRpcRequest&, std::promise<JsonValue> p) {
            held_promise =
                std::make_shared<std::promise<JsonValue>>(std::move(p));
        });

    auto start = std::chrono::steady_clock::now();
    auto future = hp.client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), ModernMeta(),
        std::chrono::milliseconds(300));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(elapsed_ms, static_cast<int64_t>(900));
    auto result = future.get();
    ASSERT_TRUE(result.Contains("code"));
    EXPECT_EQ(result["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::RequestTimeout));
}

TEST(SessionHandlerTest, CancelledNotificationFlagsInFlightIncomingRequest) {
    HandlerPair hp;

    std::promise<JsonValue> id_promise;
    auto id_future = id_promise.get_future();
    std::shared_ptr<std::atomic<bool>> flag;
    std::shared_ptr<std::promise<JsonValue>> held_promise;

    hp.server->SetRequestHandler(methods::kCallTool,
        [srv = hp.server.get(), &id_promise, &flag, &held_promise](
            const JsonRpcRequest& req, std::promise<JsonValue> p) {
            flag = srv->GetIncomingCancellationFlag(req.id);
            held_promise = std::make_shared<std::promise<JsonValue>>(std::move(p));
            JsonValue id = std::holds_alternative<int64_t>(req.id)
                ? JsonValue(std::get<int64_t>(req.id))
                : JsonValue(std::get<std::string>(req.id));
            id_promise.set_value(std::move(id));
        });

    auto future = hp.client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), ModernMeta(),
        std::chrono::milliseconds(5000));

    ASSERT_EQ(id_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    ASSERT_NE(flag, nullptr);
    ASSERT_NE(held_promise, nullptr);
    EXPECT_FALSE(flag->load());

    JsonValue params(JsonValue::object_tag);
    params["requestId"] = id_future.get();
    hp.client->SendNotification(notifications::kCancelled, std::move(params));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!flag->load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(flag->load());

    held_promise->set_value(JsonValue(JsonValue::object_tag));
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_FALSE(future.get().Contains("code"));
}

TEST(SessionHandlerTest, RejectsUnsupportedDeclaredProtocolVersion) {
    HandlerPair hp;

    bool handled = false;
    hp.server->SetRequestHandler(methods::kCallTool,
        [&handled](const JsonRpcRequest&, std::promise<JsonValue> p) {
            handled = true;
            p.set_value(JsonValue(JsonValue::object_tag));
        });

    RequestMeta meta = ModernMeta();
    meta.protocol_version = "2099-01-01";

    auto future = hp.client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), meta,
        std::chrono::milliseconds(2000));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result.Contains("code"));
    EXPECT_EQ(result["code"].GetInt(),
              static_cast<int64_t>(McpErrorCode::UnsupportedProtocolVersion));
    EXPECT_FALSE(handled);

    auto* data = result.Find("data");
    ASSERT_NE(data, nullptr);
    ASSERT_TRUE(data->IsObject());
    auto* supported = data->Find("supported");
    ASSERT_NE(supported, nullptr);
    ASSERT_TRUE(supported->IsArray());
    bool lists_latest = false;
    for (const auto& v : supported->GetArray()) {
        if (v.IsString() && v.GetString() == std::string(kLatestProtocolVersion))
            lists_latest = true;
    }
    EXPECT_TRUE(lists_latest);
    auto* requested = data->Find("requested");
    ASSERT_NE(requested, nullptr);
    EXPECT_EQ(requested->GetString(), "2099-01-01");
}

TEST(SessionHandlerTest, RejectsRequestWithMalformedMetaField) {
    HandlerPair hp;

    bool handled = false;
    hp.server->SetRequestHandler(methods::kCallTool,
        [&handled](const JsonRpcRequest&, std::promise<JsonValue> p) {
            handled = true;
            p.set_value(JsonValue(JsonValue::object_tag));
        });

    std::promise<JsonRpcErrorResponse> error_promise;
    auto error_future = error_promise.get_future();
    hp.client->SetOnErrorCallback([&error_promise](const JsonRpcErrorResponse& e) {
        error_promise.set_value(e);
    });

    JsonValue meta(JsonValue::object_tag);
    meta["io.modelcontextprotocol/protocolVersion"] =
        JsonValue(std::string(kLatestProtocolVersion));
    meta["progressToken"] = JsonValue(true);

    JsonRpcRequest req;
    req.id = RequestId{int64_t(4242)};
    req.method = std::string(methods::kCallTool);
    req.params = JsonValue(JsonValue::object_tag);
    req.meta = std::move(meta);
    hp.client->SendMessage(JsonRpcMessage{std::move(req)});

    ASSERT_EQ(error_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto err = error_future.get();
    EXPECT_EQ(static_cast<int32_t>(err.error.code),
              static_cast<int32_t>(McpErrorCode::InvalidParams));
    EXPECT_FALSE(handled);
}
