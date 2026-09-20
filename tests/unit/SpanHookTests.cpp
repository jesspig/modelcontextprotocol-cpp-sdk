// SpanHookTests — behavior tests for the span observation hooks: an
// uninstrumented session is left untouched, and an injected handler observes
// the server request and transport boundaries with the caller's trace context.

#include <mcp/SpanHooks.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/protocol/WireCodec.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/Methods.hpp>

#include <mcp/test/McpTest.hpp>
#include <mcp/test/McpTimeout.hpp>

#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace mcp;

namespace {

struct SessionPair {
    std::shared_ptr<McpSessionHandler> client;
    std::shared_ptr<McpSessionHandler> server;

    explicit SessionPair(std::string_view era = kLatestProtocolVersion) {
        auto pair = InMemoryTransport::CreatePair();
        client = std::make_shared<McpSessionHandler>(
            std::move(pair.client), MakeWireCodec(era));
        server = std::make_shared<McpSessionHandler>(
            std::move(pair.server), MakeWireCodec(era));
        client->Start();
        server->Start();
    }

    ~SessionPair() {
        client->Close();
        server->Close();
    }
};

// Hooks fire on the message loop and response worker threads, so the sink
// synchronizes.
struct SpanRecorder {
    std::mutex mutex;
    std::vector<SpanEvent> events;

    SpanHandler Handler() {
        return [this](const SpanEvent& event) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(event);
        };
    }

    std::optional<SpanEvent> Find(SpanKind kind, SpanPhase phase) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& event : events) {
            if (event.kind == kind && event.phase == phase) return event;
        }
        return std::nullopt;
    }

    std::size_t Count(SpanKind kind, SpanPhase phase) {
        std::lock_guard<std::mutex> lock(mutex);
        std::size_t total = 0;
        for (const auto& event : events) {
            if (event.kind == kind && event.phase == phase) ++total;
        }
        return total;
    }
};

void RegisterEchoHandler(const std::shared_ptr<McpSessionHandler>& server, std::string_view method) {
    server->SetRequestHandler(method,
        [](const JsonRpcRequest&, std::promise<JsonValue> p) {
            p.set_value(JsonValue(JsonValue::object_tag));
        });
}

RequestMeta ModernMeta() {
    RequestMeta meta;
    meta.protocol_version = std::string(kLatestProtocolVersion);
    meta.client_info = Implementation{"span-hook-client", "1.0"};
    return meta;
}

JsonValue CallServer(const SessionPair& sp, const RequestMeta& meta) {
    auto future = sp.client->SendRequest(methods::kCallTool,
        JsonValue(JsonValue::object_tag), meta, std::chrono::milliseconds(2000));
    EXPECT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    return future.get();
}

} // namespace

// Regression guard: with no handler injected the session answers exactly as
// before, so the hooks leave the uninstrumented path untouched.
TEST(SpanHookTest, UninstrumentedSessionAnswersRequests) {
    MCP_RUN_WITH_TIMEOUT([&] {
        SessionPair sp;
        RegisterEchoHandler(sp.server, methods::kCallTool);

        auto result = CallServer(sp, ModernMeta());
        EXPECT_EQ(result["resultType"], JsonValue("complete"));
    });
}

// An injected handler observes the server request boundary: Begin when the
// request is dispatched, End once the response has been produced.
TEST(SpanHookTest, InjectedHandlerObservesServerRequestBoundary) {
    MCP_RUN_WITH_TIMEOUT([&] {
        SpanRecorder recorder;
        SessionPair sp;
        sp.server->SetSpanHandler(recorder.Handler());
        RegisterEchoHandler(sp.server, methods::kCallTool);

        auto result = CallServer(sp, ModernMeta());
        EXPECT_EQ(result["resultType"], JsonValue("complete"));

        auto begin = recorder.Find(SpanKind::ServerRequest, SpanPhase::Begin);
        ASSERT_TRUE(begin.has_value());
        EXPECT_EQ(begin->method, std::string(methods::kCallTool));
        EXPECT_EQ(begin->name, std::string(methods::kCallTool));
        EXPECT_FALSE(begin->request_id.empty());
        EXPECT_FALSE(begin->failed);

        auto end = recorder.Find(SpanKind::ServerRequest, SpanPhase::End);
        ASSERT_TRUE(end.has_value());
        EXPECT_EQ(end->request_id, begin->request_id);
        EXPECT_EQ(end->method, std::string(methods::kCallTool));
        EXPECT_FALSE(end->failed);
    });
}

// The transport send/receive boundaries are observable too, and every Begin is
// paired with an End.
TEST(SpanHookTest, InjectedHandlerObservesTransportBoundaries) {
    MCP_RUN_WITH_TIMEOUT([&] {
        SpanRecorder recorder;
        SessionPair sp;
        sp.server->SetSpanHandler(recorder.Handler());
        RegisterEchoHandler(sp.server, methods::kCallTool);

        (void)CallServer(sp, ModernMeta());

        EXPECT_EQ(recorder.Count(SpanKind::TransportReceive, SpanPhase::Begin),
            recorder.Count(SpanKind::TransportReceive, SpanPhase::End));
        EXPECT_EQ(recorder.Count(SpanKind::TransportSend, SpanPhase::Begin),
            recorder.Count(SpanKind::TransportSend, SpanPhase::End));
        EXPECT_GT(recorder.Count(SpanKind::TransportReceive, SpanPhase::Begin), std::size_t{0});
        EXPECT_GT(recorder.Count(SpanKind::TransportSend, SpanPhase::Begin), std::size_t{0});
    });
}

// The server request span carries the caller's trace context from _meta.
TEST(SpanHookTest, ServerRequestSpanCarriesTraceContext) {
    MCP_RUN_WITH_TIMEOUT([&] {
        SpanRecorder recorder;
        SessionPair sp;
        sp.server->SetSpanHandler(recorder.Handler());
        RegisterEchoHandler(sp.server, methods::kCallTool);

        auto meta = ModernMeta();
        meta.traceparent = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
        meta.tracestate = "vendor=value";
        meta.baggage = "userId=42";
        (void)CallServer(sp, meta);

        auto begin = recorder.Find(SpanKind::ServerRequest, SpanPhase::Begin);
        ASSERT_TRUE(begin.has_value());
        EXPECT_EQ(begin->traceparent, *meta.traceparent);
        EXPECT_EQ(begin->tracestate, *meta.tracestate);
        EXPECT_EQ(begin->baggage, *meta.baggage);
    });
}

// A rejected request closes its span flagged as failed instead of leaking an
// unterminated Begin.
TEST(SpanHookTest, RejectedRequestClosesFailedSpan) {
    MCP_RUN_WITH_TIMEOUT([&] {
        SpanRecorder recorder;
        SessionPair sp;
        sp.server->SetSpanHandler(recorder.Handler());

        auto result = CallServer(sp, ModernMeta());
        ASSERT_TRUE(result.Contains("code"));

        auto begin = recorder.Find(SpanKind::ServerRequest, SpanPhase::Begin);
        ASSERT_TRUE(begin.has_value());
        auto end = recorder.Find(SpanKind::ServerRequest, SpanPhase::End);
        ASSERT_TRUE(end.has_value());
        EXPECT_EQ(end->request_id, begin->request_id);
        EXPECT_TRUE(end->failed);
    });
}
