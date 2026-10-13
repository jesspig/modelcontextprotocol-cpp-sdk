#include <mcp/transport/StreamableHttpClientTransport.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/Transport.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/http/HttpServer.hpp>
#include <mcp/http/SessionStore.hpp>
#include <mcp/ext/DPoP.hpp>
#include <mcp/test/McpTest.hpp>
#include "TestServerUtil.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace mcp;

namespace mcp {
namespace streamable_http_client_impl {

enum class ListenState { Idle, Connecting, Streaming, Unsupported, GivenUp };

struct SseBlockParseResult {
    std::string data;
    std::string event_id;
    std::optional<uint64_t> retry_ms;
    std::optional<JsonRpcMessage> message;
};

SseBlockParseResult ParseSseBlock(const std::string& block);
ListenState ListenStateForStatusCode(int status_code);
bool ShouldListenStream(const HttpClientTransportOptions& options);
std::chrono::milliseconds ReconnectDelayForAttempt(int attempt,
                                                  std::optional<uint64_t> retry_ms);
std::string EffectiveProtocolVersion(const std::string& negotiated_version);
std::optional<std::string> ProtocolVersionHeaderFor(const std::string& method,
                                                    const std::string& negotiated_version);
std::optional<std::string> SessionIdHeaderFor(const std::string& method,
                                              const std::string& session_id);
std::optional<std::string> NegotiatedVersionFromResponse(const std::string& response_json);

} // namespace streamable_http_client_impl
} // namespace mcp

TEST(StreamableHttpTransportTest, ClientConstruction) {
    HttpClientTransportOptions opts;
    opts.endpoint = "http://localhost:9999/mcp";
    opts.transport_mode = HttpTransportMode::StreamableHttp;
    opts.name = "test-client";

    StreamableHttpClientTransport transport(opts);
    EXPECT_EQ(transport.Name(), "test-client");
}

TEST(StreamableHttpTransportTest, ClientDefaultMode) {
    HttpClientTransportOptions opts;
    opts.endpoint = "http://localhost:9999/mcp";
    StreamableHttpClientTransport transport(opts);
    EXPECT_EQ(transport.Name(), "streamable-http");
}

TEST(StreamableHttpTransportTest, ClientEndpointUrl) {
    HttpClientTransportOptions opts;
    opts.endpoint = "https://mcp.example.com/stream";
    opts.transport_mode = HttpTransportMode::StreamableHttp;
    StreamableHttpClientTransport transport(opts);
    EXPECT_EQ(transport.Name(), "streamable-http");
}

TEST(StreamableHttpTransportTest, ClientSseMode) {
    HttpClientTransportOptions opts;
    opts.endpoint = "http://localhost:9999/sse";
    opts.transport_mode = HttpTransportMode::Sse;
    StreamableHttpClientTransport transport(opts);
    EXPECT_EQ(transport.Name(), "streamable-http");
}

TEST(StreamableHttpTransportTest, ClientAdditionalHeaders) {
    HttpClientTransportOptions opts;
    opts.endpoint = "http://localhost:9999/mcp";
    opts.additional_headers["X-Custom"] = "test-value";
    StreamableHttpClientTransport transport(opts);
    EXPECT_EQ(transport.Name(), "streamable-http");
}

TEST(StreamableHttpTransportTest, ServerTransportConstruction) {
    StreamableHttpServerOptions opts;
    opts.port = 3001;
    opts.endpoint = "/mcp";
    opts.server_name = "test-server";
    StreamableHttpServerTransport transport(opts);
    EXPECT_TRUE(transport.IsStateless());
}

TEST(StreamableHttpTransportTest, ServerTransportStateless) {
    StreamableHttpServerOptions opts;
    opts.stateless = true;
    opts.server_name = "stateless-server";
    StreamableHttpServerTransport transport(opts);
    EXPECT_TRUE(transport.IsStateless());
}

TEST(StreamableHttpTransportTest, ServerTransportDefaultOptions) {
    StreamableHttpServerTransport transport;
    EXPECT_TRUE(transport.IsStateless());
}

TEST(StreamableHttpTransportTest, ServerMcpHeadersValidation) {
    std::string error;
    auto body = JsonValue::Parse(R"({"jsonrpc":"2.0","method":"tools/list","id":1})");

    EXPECT_TRUE(StreamableHttpServerTransport::ValidateMcpHeaders("tools/list", "", body, error));
    EXPECT_TRUE(error.empty());

    EXPECT_FALSE(StreamableHttpServerTransport::ValidateMcpHeaders("tools/call", "", body, error));
    EXPECT_FALSE(error.empty());
}

TEST(StreamableHttpTransportTest, ServerMcpHeadersEmpty) {
    std::string error;
    auto body = JsonValue::Parse(R"({"jsonrpc":"2.0","method":"tools/list","id":1})");

    EXPECT_TRUE(StreamableHttpServerTransport::ValidateMcpHeaders("", "", body, error));
    EXPECT_TRUE(error.empty());
}

TEST(StreamableHttpTransportTest, ServerMcpHeadersNameMismatch) {
    std::string error;
    auto body = JsonValue::Parse(R"({"jsonrpc":"2.0","method":"resources/list","params":{"name":"test-resource"},"id":1})");

    EXPECT_TRUE(StreamableHttpServerTransport::ValidateMcpHeaders("resources/list", "test-resource", body, error));
    EXPECT_TRUE(error.empty());

    EXPECT_FALSE(StreamableHttpServerTransport::ValidateMcpHeaders("resources/list", "wrong-name", body, error));
    EXPECT_FALSE(error.empty());
}

TEST(StreamableHttpTransportTest, ClientListenStreamEnabledByDefault) {
    using mcp::streamable_http_client_impl::ShouldListenStream;
    HttpClientTransportOptions opts;
    EXPECT_FALSE(opts.enable_listen_stream.has_value());
    EXPECT_TRUE(ShouldListenStream(opts));
    opts.stateless = true;
    EXPECT_FALSE(ShouldListenStream(opts));
    opts.enable_listen_stream = true;
    EXPECT_TRUE(ShouldListenStream(opts));
    opts.enable_listen_stream = false;
    EXPECT_FALSE(ShouldListenStream(opts));
}

TEST(StreamableHttpTransportTest, SseBlockParseIdAndData) {
    auto parsed = mcp::streamable_http_client_impl::ParseSseBlock(
        "event: message\n"
        "id: 7\n"
        "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/message\","
        "\"params\":{\"level\":\"info\",\"data\":\"hello\"}}");
    EXPECT_EQ(parsed.event_id, "7");
    ASSERT_TRUE(parsed.message.has_value());
    auto* notification = mcp::AsNotification(*parsed.message);
    ASSERT_TRUE(notification != nullptr);
    EXPECT_EQ(notification->method, "notifications/message");
}

TEST(StreamableHttpTransportTest, SseBlockParseInvalidDataIgnored) {
    auto parsed = mcp::streamable_http_client_impl::ParseSseBlock(
        "id: 8\ndata: {not-valid-json");
    EXPECT_FALSE(parsed.message.has_value());
    EXPECT_EQ(parsed.event_id, "8");
}

TEST(StreamableHttpTransportTest, StreamableRetryParsedAndClamped) {
    auto parsed = mcp::streamable_http_client_impl::ParseSseBlock(
        "retry: 500\n"
        "id: 7\n"
        "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/message\","
        "\"params\":{\"level\":\"info\",\"data\":\"hello\"}}");
    ASSERT_TRUE(parsed.retry_ms.has_value());
    EXPECT_EQ(parsed.retry_ms.value(), static_cast<uint64_t>(500));
    EXPECT_EQ(parsed.event_id, "7");
    ASSERT_TRUE(parsed.message.has_value());

    auto non_numeric = mcp::streamable_http_client_impl::ParseSseBlock(
        "retry: fast\nid: 8\ndata: {}");
    EXPECT_FALSE(non_numeric.retry_ms.has_value());
    EXPECT_EQ(non_numeric.event_id, "8");

    auto mixed = mcp::streamable_http_client_impl::ParseSseBlock(
        "retry: 12x\nretry: \nretry:\nid: 9\ndata: {}");
    EXPECT_FALSE(mixed.retry_ms.has_value());
    EXPECT_EQ(mixed.event_id, "9");

    struct RetryWindowCase {
        int attempt;
        std::optional<uint64_t> retry_ms;
        int64_t expected_ms;
    };
    const RetryWindowCase cases[] = {
        {1, std::nullopt, 1000},
        {2, std::nullopt, 1000},
        {3, std::nullopt, 1000},
        {1, std::optional<uint64_t>{500}, 500},
        {2, std::optional<uint64_t>{500}, 500},
        {1, std::optional<uint64_t>{0}, 450},
        {1, std::optional<uint64_t>{1}, 450},
        {1, std::optional<uint64_t>{449}, 450},
        {1, std::optional<uint64_t>{450}, 450},
        {1, std::optional<uint64_t>{1000}, 1000},
        {1, std::optional<uint64_t>{1001}, 1000},
        {1, std::optional<uint64_t>{30000}, 1000},
        {1, std::optional<uint64_t>{UINT64_MAX}, 1000},
        {2, std::optional<uint64_t>{100}, 450},
    };
    for (const auto& c : cases) {
        EXPECT_EQ(mcp::streamable_http_client_impl::ReconnectDelayForAttempt(
                      c.attempt, c.retry_ms)
                      .count(),
                  c.expected_ms);
    }
    const std::optional<uint64_t> probes[] = {
        std::nullopt,
        std::optional<uint64_t>{0},
        std::optional<uint64_t>{1},
        std::optional<uint64_t>{449},
        std::optional<uint64_t>{450},
        std::optional<uint64_t>{500},
        std::optional<uint64_t>{999},
        std::optional<uint64_t>{1000},
        std::optional<uint64_t>{1001},
        std::optional<uint64_t>{5000},
        std::optional<uint64_t>{UINT64_MAX},
    };
    for (int attempt = 1; attempt <= 4; ++attempt) {
        for (const auto& retry : probes) {
            auto wait = mcp::streamable_http_client_impl::ReconnectDelayForAttempt(
                attempt, retry);
            EXPECT_GE(wait.count(), 450);
            EXPECT_LE(wait.count(), 1000);
        }
    }
}

TEST(StreamableHttpTransportTest, StreamableReconnectCarriesLastEventId) {
    auto port = PickFreePort(kTestBasePort + 1900);
    HttpServer mock(port);
    std::atomic<int> get_calls{0};
    std::mutex state_mutex;
    std::string second_last_event_id;
    std::chrono::steady_clock::time_point first_close{};
    std::chrono::steady_clock::time_point second_arrival{};
    std::promise<void> second_arrived;
    mock.SetHandler("POST", "/mcp", [](const HttpRequest&, HttpResponse& resp) {
        resp.status_code = 202;
        resp.status_text = "Accepted";
        resp.body = "{}";
    });
    mock.SetHandler("GET", "/mcp", [&](const HttpRequest& req, HttpResponse& resp) {
        int index = get_calls.fetch_add(1);
        if (index == 0) {
            resp.is_sse = true;
            resp.sse_close_after_write = true;
            resp.body =
                "retry: 500\n"
                "id: evt-1\n"
                "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/message\","
                "\"params\":{\"level\":\"info\",\"data\":\"hello\"}}\n\n";
            std::lock_guard<std::mutex> lk(state_mutex);
            first_close = std::chrono::steady_clock::now();
            return;
        }
        auto it = req.headers.find("last-event-id");
        std::lock_guard<std::mutex> lk(state_mutex);
        if (index == 1) {
            second_last_event_id =
                it == req.headers.end() ? std::string{} : it->second;
            second_arrival = std::chrono::steady_clock::now();
            second_arrived.set_value();
        }
        resp.status_code = 405;
        resp.status_text = "Method Not Allowed";
    });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);
    auto& channel = transport->GetMessageChannel();

    JsonRpcNotification initialized;
    initialized.method = "notifications/initialized";
    transport->SendMessageAsync(std::move(initialized));

    std::atomic<bool> got{false};
    std::thread receiver([&] {
        channel.AsyncReceive([&](std::error_code ec, JsonRpcMessage) {
            if (!ec) got.store(true);
        });
    });

    bool reconnected = second_arrived.get_future().wait_for(std::chrono::seconds(8)) ==
                       std::future_status::ready;
    transport->Close();
    mock.Stop();
    receiver.join();

    ASSERT_TRUE(reconnected);
    EXPECT_TRUE(got.load());
    EXPECT_GE(get_calls.load(), 2);
    std::string observed_id;
    std::chrono::steady_clock::time_point observed_close{};
    std::chrono::steady_clock::time_point observed_arrival{};
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        observed_id = second_last_event_id;
        observed_close = first_close;
        observed_arrival = second_arrival;
    }
    EXPECT_EQ(observed_id, "evt-1");
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          observed_arrival - observed_close)
                          .count();
    EXPECT_GE(elapsed_ms, 400);
}

TEST(StreamableHttpTransportTest, ListenStateForStatusCode) {
    using mcp::streamable_http_client_impl::ListenState;
    using mcp::streamable_http_client_impl::ListenStateForStatusCode;
    EXPECT_TRUE(ListenStateForStatusCode(405) == ListenState::Unsupported);
    EXPECT_TRUE(ListenStateForStatusCode(200) == ListenState::Streaming);
    EXPECT_TRUE(ListenStateForStatusCode(500) == ListenState::GivenUp);
    EXPECT_TRUE(ListenStateForStatusCode(503) == ListenState::GivenUp);
    EXPECT_TRUE(ListenStateForStatusCode(404) == ListenState::Connecting);
}

TEST(StreamableHttpTransportTest, ListenStreamDeliversNotification) {
    auto port = PickFreePort(kTestBasePort + 1300);
    HttpServer mock(port);
    mock.SetHandler("POST", "/mcp", [](const HttpRequest&, HttpResponse& resp) {
        resp.status_code = 202;
        resp.status_text = "Accepted";
        resp.body = "{}";
    });
    mock.SetHandler("GET", "/mcp", [](const HttpRequest&, HttpResponse& resp) {
        resp.is_sse = true;
        resp.sse_close_after_write = true;
        resp.body =
            "id: evt-1\n"
            "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/message\","
            "\"params\":{\"level\":\"info\",\"data\":\"hello\"}}\n\n";
    });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);
    auto& channel = transport->GetMessageChannel();

    JsonRpcNotification initialized;
    initialized.method = "notifications/initialized";
    transport->SendMessageAsync(std::move(initialized));

    JsonRpcMessage received;
    std::atomic<bool> got{false};
    channel.AsyncReceive([&](std::error_code ec, JsonRpcMessage msg) {
        if (!ec) {
            received = std::move(msg);
            got.store(true);
        }
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!got.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(got.load());
    auto* notification = AsNotification(received);
    ASSERT_TRUE(notification != nullptr);
    EXPECT_EQ(notification->method, "notifications/message");

    transport->Close();
    mock.Stop();
}

TEST(StreamableHttpTransportTest, ListenStreamStopsAfter405) {
    auto port = PickFreePort(kTestBasePort + 1350);
    HttpServer mock(port);
    std::atomic<int> get_calls{0};
    mock.SetHandler("POST", "/mcp", [](const HttpRequest&, HttpResponse& resp) {
        resp.status_code = 202;
        resp.status_text = "Accepted";
        resp.body = "{}";
    });
    mock.SetHandler("GET", "/mcp", [&](const HttpRequest&, HttpResponse& resp) {
        get_calls.fetch_add(1);
        resp.status_code = 405;
        resp.status_text = "Method Not Allowed";
    });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);

    JsonRpcNotification initialized;
    initialized.method = "notifications/initialized";
    transport->SendMessageAsync(std::move(initialized));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (get_calls.load() < 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(get_calls.load(), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EXPECT_EQ(get_calls.load(), 1);

    transport->Close();
    mock.Stop();
}

TEST(StreamableHttpTransportTest, StatelessClientSkipsListenStreamByDefault) {
    auto port = PickFreePort(kTestBasePort + 1352);
    HttpServer mock(port);
    std::atomic<int> get_calls{0};
    mock.SetHandler("POST", "/mcp", [](const HttpRequest&, HttpResponse& resp) {
        resp.status_code = 202;
        resp.status_text = "Accepted";
        resp.body = "{}";
    });
    mock.SetHandler("GET", "/mcp", [&](const HttpRequest&, HttpResponse& resp) {
        get_calls.fetch_add(1);
        resp.status_code = 405;
        resp.status_text = "Method Not Allowed";
    });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    opts.stateless = true;
    StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);

    JsonRpcNotification initialized;
    initialized.method = "notifications/initialized";
    transport->SendMessageAsync(std::move(initialized));

    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    EXPECT_EQ(get_calls.load(), 0);

    transport->Close();
    mock.Stop();
}

TEST(StreamableHttpTransportTest, StatelessClientForcedListenStreamSendsGet) {
    auto port = PickFreePort(kTestBasePort + 1354);
    HttpServer mock(port);
    std::atomic<int> get_calls{0};
    mock.SetHandler("POST", "/mcp", [](const HttpRequest&, HttpResponse& resp) {
        resp.status_code = 202;
        resp.status_text = "Accepted";
        resp.body = "{}";
    });
    mock.SetHandler("GET", "/mcp", [&](const HttpRequest&, HttpResponse& resp) {
        get_calls.fetch_add(1);
        resp.status_code = 405;
        resp.status_text = "Method Not Allowed";
    });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    opts.stateless = true;
    opts.enable_listen_stream = true;
    StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);

    JsonRpcNotification initialized;
    initialized.method = "notifications/initialized";
    transport->SendMessageAsync(std::move(initialized));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (get_calls.load() < 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(get_calls.load(), 1);

    transport->Close();
    mock.Stop();
}

TEST(StreamableHttpTransportTest, ProtocolVersionHeaderOmittedForInitialize) {
    using mcp::streamable_http_client_impl::ProtocolVersionHeaderFor;
    EXPECT_FALSE(ProtocolVersionHeaderFor("initialize", "").has_value());
    EXPECT_FALSE(ProtocolVersionHeaderFor("initialize", "2025-11-25").has_value());
}

TEST(StreamableHttpTransportTest, ProtocolVersionHeaderFallsBackToModern) {
    using mcp::streamable_http_client_impl::ProtocolVersionHeaderFor;
    auto v = ProtocolVersionHeaderFor("tools/list", "");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "2026-07-28");
}

TEST(StreamableHttpTransportTest, ProtocolVersionHeaderUsesNegotiatedVersion) {
    using mcp::streamable_http_client_impl::ProtocolVersionHeaderFor;
    auto v = ProtocolVersionHeaderFor("tools/list", "2025-11-25");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "2025-11-25");
}

TEST(StreamableHttpTransportTest, NegotiatedVersionExtractedFromInitializeResult) {
    using mcp::streamable_http_client_impl::NegotiatedVersionFromResponse;
    auto v = NegotiatedVersionFromResponse(
        R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-11-25"}})");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "2025-11-25");
    EXPECT_FALSE(NegotiatedVersionFromResponse(
        R"({"jsonrpc":"2.0","id":1,"result":{}})").has_value());
    EXPECT_FALSE(NegotiatedVersionFromResponse(
        R"({"jsonrpc":"2.0","id":1,"error":{"code":-32000,"message":"x"}})").has_value());
    EXPECT_FALSE(NegotiatedVersionFromResponse("not-json").has_value());
}

TEST(StreamableHttpTransportTest, ClientProtocolVersionHeaderNegotiationFlow) {
    auto port = PickFreePort(kTestBasePort + 1400);
    HttpServer mock(port);
    std::atomic<int> post_calls{0};
    std::atomic<bool> init_header_absent{false};
    std::atomic<bool> followup_header_learned{false};
    mock.SetHandler("POST", "/mcp", [&](const HttpRequest& req, HttpResponse& resp) {
        auto it = req.headers.find("mcp-protocol-version");
        std::string version =
            it == req.headers.end() ? std::string{} : it->second;
        if (post_calls.fetch_add(1) == 0) {
            init_header_absent = version.empty();
            resp.body =
                R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-11-25",)"
                R"("capabilities":{},"serverInfo":{"name":"mock","version":"0.0.1"}}})";
        } else {
            followup_header_learned = (version == "2025-11-25");
            resp.body = R"({"jsonrpc":"2.0","id":2,"result":{}})";
        }
    });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);
    auto& channel = transport->GetMessageChannel();

    std::atomic<bool> got1{false};
    std::atomic<bool> got2{false};

    JsonRpcRequest init;
    init.method = "initialize";
    init.id = 1;
    transport->SendMessageAsync(std::move(init));

    channel.AsyncReceive([&](std::error_code ec, JsonRpcMessage) {
        if (!ec) got1.store(true);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!got1.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(got1.load());

    JsonRpcRequest listing;
    listing.method = "tools/list";
    listing.id = 2;
    transport->SendMessageAsync(std::move(listing));

    channel.AsyncReceive([&](std::error_code ec, JsonRpcMessage) {
        if (!ec) got2.store(true);
    });

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!got2.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(got2.load());

    EXPECT_TRUE(init_header_absent.load());
    EXPECT_TRUE(followup_header_learned.load());

    transport->Close();
    mock.Stop();
}

TEST(StreamableHttpTransportTest, SessionIdHeaderOmittedForInitialize) {
    using mcp::streamable_http_client_impl::SessionIdHeaderFor;
    EXPECT_FALSE(SessionIdHeaderFor("initialize", "stale-session").has_value());
    EXPECT_FALSE(SessionIdHeaderFor("initialize", "").has_value());
    EXPECT_FALSE(SessionIdHeaderFor("tools/list", "").has_value());
    auto sid = SessionIdHeaderFor("tools/list", "stale-session");
    ASSERT_TRUE(sid.has_value());
    EXPECT_EQ(*sid, "stale-session");
}

TEST(StreamableHttpTransportTest, Post404DeliversSessionExpiredError) {
    auto port = PickFreePort(kTestBasePort + 1450);
    HttpServer mock(port);
    std::atomic<int> post_calls{0};
    std::atomic<bool> init_sid_absent{false};
    std::atomic<bool> followup_sid_learned{false};
    mock.SetHandler("POST", "/mcp", [&](const HttpRequest& req, HttpResponse& resp) {
        if (post_calls.fetch_add(1) == 0) {
            init_sid_absent =
                req.headers.find("mcp-session-id") == req.headers.end();
            resp.headers["Mcp-Session-Id"] = "fresh-session";
            resp.body =
                R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-11-25",)"
                R"("capabilities":{},"serverInfo":{"name":"mock","version":"0.0.1"}}})";
            return;
        }
        auto it = req.headers.find("mcp-session-id");
        followup_sid_learned = it != req.headers.end() && it->second == "fresh-session";
        resp.status_code = 404;
        resp.status_text = "Not Found";
        resp.body = "session expired";
    });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    opts.known_session_id = "stale-session";
    StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);
    auto& channel = transport->GetMessageChannel();

    std::atomic<bool> got1{false};
    std::atomic<bool> got2{false};
    JsonRpcMessage received2;

    JsonRpcRequest init;
    init.method = "initialize";
    init.id = 1;
    transport->SendMessageAsync(std::move(init));

    channel.AsyncReceive([&](std::error_code ec, JsonRpcMessage) {
        if (!ec) got1.store(true);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!got1.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(got1.load());

    JsonRpcRequest listing;
    listing.method = "tools/list";
    listing.id = 2;
    transport->SendMessageAsync(std::move(listing));

    channel.AsyncReceive([&](std::error_code ec, JsonRpcMessage msg) {
        if (!ec) {
            received2 = std::move(msg);
            got2.store(true);
        }
    });

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!got2.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(got2.load());

    EXPECT_TRUE(init_sid_absent.load());
    EXPECT_TRUE(followup_sid_learned.load());

    auto* err = AsError(received2);
    ASSERT_TRUE(err != nullptr);
    EXPECT_EQ(err->error.code, McpErrorCode::SessionExpired);
    ASSERT_TRUE(err->id.has_value());
    EXPECT_EQ(std::get<int64_t>(*err->id), 2);

    transport->Close();
    mock.Stop();
}

#ifdef _WIN32
TEST(StreamableHttpTransportTest, PostSseStreamEofDeliversConnectionClosedError) {
    auto port = PickFreePort(kTestBasePort + 1970);
    HttpServer mock(port);
    mock.SetHandler("POST", "/mcp", [](const HttpRequest&, HttpResponse& resp) {
        resp.is_sse = true;
        resp.sse_close_after_write = true;
        resp.body = "id: evt-9\nretry: 500\n\n";
    });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);
    auto& channel = transport->GetMessageChannel();

    std::atomic<bool> got_error{false};
    JsonRpcMessage received;
    std::thread receiver([&] {
        channel.AsyncReceive([&](std::error_code ec, JsonRpcMessage msg) {
            if (!ec) {
                received = std::move(msg);
                got_error.store(true);
            }
        });
    });

    JsonRpcRequest call;
    call.method = "tools/call";
    call.id = 7;
    transport->SendMessageAsync(std::move(call));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!got_error.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!got_error.load()) {
        transport->Close();
        mock.Stop();
        receiver.join();
        ASSERT_TRUE(got_error.load());
        return;
    }
    receiver.join();

    auto* err = AsError(received);
    ASSERT_TRUE(err != nullptr);
    EXPECT_EQ(err->error.code, McpErrorCode::ConnectionClosed);
    ASSERT_TRUE(err->id.has_value());
    EXPECT_EQ(std::get<int64_t>(*err->id), 7);

    transport->Close();
    mock.Stop();
}
#endif

TEST(StreamableHttpTransportTest, PostSseInterruptedResumesViaGetWithLastEventId) {
    auto port = PickFreePort(kTestBasePort + 1980);
    HttpServer mock(port);
    std::atomic<int> post_calls{0};
    std::atomic<int> get_calls{0};
    std::mutex state_mutex;
    std::string resume_last_event_id;
    std::chrono::steady_clock::time_point post_eof{};
    std::chrono::steady_clock::time_point resume_arrival{};
    std::promise<void> resumed;
    mock.SetHandler("POST", "/mcp", [&](const HttpRequest&, HttpResponse& resp) {
        post_calls.fetch_add(1);
        resp.is_sse = true;
        resp.sse_close_after_write = true;
        resp.body = "retry: 500\nid: evt-resume\n\n";
        std::lock_guard<std::mutex> lk(state_mutex);
        post_eof = std::chrono::steady_clock::now();
    });
    mock.SetHandler("GET", "/mcp", [&](const HttpRequest& req, HttpResponse& resp) {
        int index = get_calls.fetch_add(1);
        auto it = req.headers.find("last-event-id");
        std::string observed = it == req.headers.end() ? std::string{} : it->second;
        if (index == 0) {
            std::lock_guard<std::mutex> lk(state_mutex);
            resume_last_event_id = observed;
            resume_arrival = std::chrono::steady_clock::now();
            resumed.set_value();
        }
        resp.is_sse = true;
        resp.sse_close_after_write = true;
        resp.body =
            "id: evt-done\n"
            "data: {\"jsonrpc\":\"2.0\",\"id\":9,\"result\":{\"resumed\":true}}\n\n";
    });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);
    auto& channel = transport->GetMessageChannel();

    std::atomic<bool> got{false};
    JsonRpcMessage received;
    std::thread receiver([&] {
        channel.AsyncReceive([&](std::error_code ec, JsonRpcMessage msg) {
            if (!ec) {
                received = std::move(msg);
                got.store(true);
            }
        });
    });

    JsonRpcRequest call;
    call.method = "tools/call";
    call.id = 9;
    transport->SendMessageAsync(std::move(call));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!got.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    bool resume_seen = resumed.get_future().wait_for(std::chrono::seconds(8)) ==
                       std::future_status::ready;
    transport->Close();
    mock.Stop();
    receiver.join();

    ASSERT_TRUE(resume_seen);
    ASSERT_TRUE(got.load());
    EXPECT_EQ(post_calls.load(), 1);
    EXPECT_EQ(get_calls.load(), 1);
    std::string observed_id;
    std::chrono::steady_clock::time_point observed_eof{};
    std::chrono::steady_clock::time_point observed_arrival{};
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        observed_id = resume_last_event_id;
        observed_eof = post_eof;
        observed_arrival = resume_arrival;
    }
    EXPECT_EQ(observed_id, "evt-resume");
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          observed_arrival - observed_eof)
                          .count();
    EXPECT_GE(elapsed_ms, 400);
    auto* response = AsResponse(received);
    ASSERT_TRUE(response != nullptr);
    EXPECT_EQ(std::get<int64_t>(response->id), 9);
}

TEST(StreamableHttpTransportTest, ImmediatePostWhileRequestInFlight) {
    auto port = PickFreePort(kTestBasePort + 1500);
    HttpServer mock(port);
    std::atomic<int> post_calls{0};
    std::atomic<bool> notification_arrived{false};
    std::atomic<bool> release_first{false};
    mock.SetHandler("POST", "/mcp", [&](const HttpRequest& req, HttpResponse& resp) {
        auto method_it = req.headers.find("mcp-method");
        bool is_notification = method_it != req.headers.end() &&
            method_it->second.compare(0, 14, "notifications/") == 0;
        if (post_calls.fetch_add(1) == 0 && !is_notification) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (!release_first.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            resp.body = R"({"jsonrpc":"2.0","id":1,"result":{}})";
            return;
        }
        if (is_notification) {
            notification_arrived.store(true);
            release_first.store(true);
        }
        resp.status_code = 202;
        resp.status_text = "Accepted";
        resp.body = "{}";
    });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);
    auto& channel = transport->GetMessageChannel();

    JsonRpcRequest call;
    call.method = "tools/call";
    call.id = 1;
    transport->SendMessageAsync(std::move(call));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (post_calls.load() < 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(post_calls.load() >= 1);

    JsonRpcNotification complete;
    complete.method = "notifications/elicitation/complete";
    transport->SendMessageAsync(std::move(complete));

    std::atomic<bool> got_response{false};
    channel.AsyncReceive([&](std::error_code ec, JsonRpcMessage msg) {
        if (!ec) {
            auto* response = AsResponse(msg);
            if (response && std::get<int64_t>(response->id) == 1)
                got_response.store(true);
        }
    });

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!got_response.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(notification_arrived.load());
    ASSERT_TRUE(got_response.load());

    transport->Close();
    mock.Stop();
}

TEST(StreamableHttpTransportTest, PostSseStreamDeliversServerRequestWhileOpen) {
    auto port = PickFreePort(kTestBasePort + 1550);
    HttpServer mock(port);
    std::atomic<int> post_calls{0};
    mock.SetHandler("POST", "/mcp", [&](const HttpRequest& req, HttpResponse& resp) {
        auto method_it = req.headers.find("mcp-method");
        if (post_calls.fetch_add(1) == 0 && method_it != req.headers.end() &&
            method_it->second == "tools/call") {
            resp.is_sse = true;
            resp.sse_close_after_write = false;
            resp.body =
                "data: {\"jsonrpc\":\"2.0\",\"id\":100,\"method\":\"elicitation/create\","
                "\"params\":{}}\n\n";
            return;
        }
        try {
            auto jv = JsonValue::Parse(req.body);
            auto* id = jv.Find("id");
            if (id && id->IsInt() && id->GetInt() == 100) {
                mock.BroadcastSse(
                    "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"action\":\"accept\"}}\n\n");
            }
        } catch (...) {
        }
        resp.status_code = 202;
        resp.status_text = "Accepted";
        resp.body = "{}";
    });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    HttpClientTransportOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    StreamableHttpClientTransport client(opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);
    auto& channel = transport->GetMessageChannel();

    JsonRpcRequest call;
    call.method = "tools/call";
    call.id = 1;
    transport->SendMessageAsync(std::move(call));

    std::atomic<bool> got_elicitation{false};
    JsonRpcMessage elicitation_msg;
    std::thread receiver([&] {
        channel.AsyncReceive([&](std::error_code ec, JsonRpcMessage msg) {
            if (!ec) {
                elicitation_msg = std::move(msg);
                got_elicitation.store(true);
            }
        });
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!got_elicitation.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(got_elicitation.load());
    if (!got_elicitation.load()) {
        mock.Stop();
        transport->Close();
        receiver.join();
        return;
    }
    receiver.join();

    auto* server_request = AsRequest(elicitation_msg);
    ASSERT_TRUE(server_request != nullptr);
    EXPECT_EQ(server_request->method, "elicitation/create");

    JsonRpcResponse answer;
    answer.id = server_request->id;
    answer.result = JsonValue::Parse(R"({"action":"accept"})");
    transport->SendMessageAsync(std::move(answer));

    std::atomic<bool> got_result{false};
    JsonRpcMessage result_msg;
    std::thread receiver2([&] {
        channel.AsyncReceive([&](std::error_code ec, JsonRpcMessage msg) {
            if (!ec) {
                result_msg = std::move(msg);
                got_result.store(true);
            }
        });
    });

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!got_result.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(got_result.load());
    if (!got_result.load()) {
        mock.Stop();
        transport->Close();
        receiver2.join();
        return;
    }
    receiver2.join();

    auto* response = AsResponse(result_msg);
    ASSERT_TRUE(response != nullptr);
    EXPECT_EQ(std::get<int64_t>(response->id), 1);

    mock.Stop();
    transport->Close();
}

namespace streamable_http_bearer_test_impl {

std::string HeaderOf(const mcp::detail::net::HttpResponseInfo& resp,
                     const std::string& name) {
    std::string lower;
    for (char c : name)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto it = resp.headers.find(lower);
    return it == resp.headers.end() ? std::string{} : it->second;
}

mcp::detail::net::HttpResponseInfo HttpPost(uint16_t port, const std::string& path,
                                            const std::string& body,
                                            const std::string& authorization = {}) {
    mcp::detail::net::HttpClient client;
    mcp::detail::net::HttpRequestSpec req;
    req.method = "POST";
    req.url = "http://127.0.0.1:" + std::to_string(port) + path;
    req.body = body;
    req.timeout = std::chrono::seconds(5);
    if (!authorization.empty()) req.headers["Authorization"] = authorization;
    return client.Request(req);
}

mcp::detail::net::HttpResponseInfo HttpPostDpop(uint16_t port, const std::string& path,
                                                const std::string& body,
                                                const std::string& authorization,
                                                const std::string& proof) {
    mcp::detail::net::HttpClient client;
    mcp::detail::net::HttpRequestSpec req;
    req.method = "POST";
    req.url = "http://127.0.0.1:" + std::to_string(port) + path;
    req.body = body;
    req.timeout = std::chrono::seconds(5);
    if (!authorization.empty()) req.headers["Authorization"] = authorization;
    if (!proof.empty()) req.headers["DPoP"] = proof;
    return client.Request(req);
}

mcp::detail::net::HttpResponseInfo HttpGet(uint16_t port, const std::string& path) {
    mcp::detail::net::HttpClient client;
    mcp::detail::net::HttpRequestSpec req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(port) + path;
    req.timeout = std::chrono::seconds(5);
    return client.Request(req);
}

std::string MetadataUrlFor(uint16_t port) {
    return "http://127.0.0.1:" + std::to_string(port) +
           "/.well-known/oauth-protected-resource";
}

StreamableHttpServerOptions BearerServerOptions(uint16_t port) {
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.enable_legacy_sse = true;
    opts.bearer_auth = StreamableHttpServerOptions::BearerAuthConfig{};
    opts.bearer_auth->resource_metadata_url = MetadataUrlFor(port);
    return opts;
}

const char* kNotificationBody = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";

void WaitUntil(std::atomic<bool>& flag) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!flag.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

} // namespace streamable_http_bearer_test_impl

using namespace streamable_http_bearer_test_impl;

TEST(StreamableHttpTransportTest, BearerAuthMissingHeaderReturnsChallenge) {
    auto port = PickFreePort(kTestBasePort + 1600);
    auto opts = BearerServerOptions(port);
    opts.bearer_auth->verify = [](const std::string&) { return AuthResult{}; };
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto resp = HttpPost(port, "/mcp", kNotificationBody);

    EXPECT_EQ(resp.status_code, 401);
    EXPECT_EQ(HeaderOf(resp, "WWW-Authenticate"),
        "Bearer resource_metadata=\"" + MetadataUrlFor(port) + "\"");

    auto get_resp = HttpGet(port, "/mcp");
    EXPECT_EQ(get_resp.status_code, 401);
    EXPECT_EQ(HeaderOf(get_resp, "WWW-Authenticate"),
        "Bearer resource_metadata=\"" + MetadataUrlFor(port) + "\"");
    server.Close();
}

TEST(StreamableHttpTransportTest, BearerAuthNonBearerSchemeRejected) {
    auto port = PickFreePort(kTestBasePort + 1620);
    auto opts = BearerServerOptions(port);
    opts.bearer_auth->verify = [](const std::string&) { return AuthResult{}; };
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto resp = HttpPost(port, "/mcp", kNotificationBody, "Basic dXNlcjpwYXNz");

    EXPECT_EQ(resp.status_code, 401);
    EXPECT_EQ(HeaderOf(resp, "WWW-Authenticate"),
        "Bearer resource_metadata=\"" + MetadataUrlFor(port) + "\"");
    server.Close();
}

TEST(StreamableHttpTransportTest, BearerAuthInvalidTokenChallenge) {
    auto port = PickFreePort(kTestBasePort + 1640);
    auto opts = BearerServerOptions(port);
    opts.bearer_auth->verify = [](const std::string& token) {
        AuthResult result;
        result.ok = (token == "test-token");
        result.scopes = {"mcp:read"};
        return result;
    };
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto resp = HttpPost(port, "/mcp", kNotificationBody, "Bearer wrong-token");

    EXPECT_EQ(resp.status_code, 401);
    EXPECT_EQ(HeaderOf(resp, "WWW-Authenticate"),
        "Bearer resource_metadata=\"" + MetadataUrlFor(port) +
        "\", error=\"invalid_token\"");
    server.Close();
}

TEST(StreamableHttpTransportTest, BearerAuthInsufficientScope) {
    auto port = PickFreePort(kTestBasePort + 1660);
    auto opts = BearerServerOptions(port);
    opts.bearer_auth->verify = [](const std::string&) {
        AuthResult result;
        result.ok = true;
        result.scopes = {"mcp:read"};
        return result;
    };
    opts.bearer_auth->required_scopes = {"mcp:read", "mcp:write"};
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto resp = HttpPost(port, "/mcp", kNotificationBody, "Bearer test-token");

    EXPECT_EQ(resp.status_code, 403);
    EXPECT_EQ(HeaderOf(resp, "WWW-Authenticate"),
        "Bearer error=\"insufficient_scope\", scope=\"mcp:read mcp:write\"");
    server.Close();
}

TEST(StreamableHttpTransportTest, BearerAuthAudienceMismatch) {
    auto port = PickFreePort(kTestBasePort + 1780);
    auto opts = BearerServerOptions(port);
    opts.bearer_auth->expected_resource =
        "http://127.0.0.1:" + std::to_string(port);
    opts.bearer_auth->verify = [](const std::string&) {
        AuthResult result;
        result.ok = true;
        result.scopes = {"mcp:read"};
        result.resource = "https://other.example/resource";
        return result;
    };
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto resp = HttpPost(port, "/mcp", kNotificationBody, "Bearer test-token");

    EXPECT_EQ(resp.status_code, 401);
    EXPECT_NE(HeaderOf(resp, "WWW-Authenticate").find("invalid_token"),
              std::string::npos);
    server.Close();
}

TEST(StreamableHttpTransportTest, BearerAuthAudienceNormalizedMatch) {
    auto port = PickFreePort(kTestBasePort + 1820);
    auto opts = BearerServerOptions(port);
    std::string expected = "http://127.0.0.1:" + std::to_string(port);
    opts.bearer_auth->expected_resource = expected;
    opts.bearer_auth->verify = [expected](const std::string&) {
        AuthResult result;
        result.ok = true;
        result.scopes = {"mcp:read"};
        result.resource = expected + "/#section";
        return result;
    };
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto resp = HttpPost(port, "/mcp", kNotificationBody, "Bearer test-token");

    EXPECT_EQ(resp.status_code, 202);
    server.Close();
}

TEST(StreamableHttpTransportTest, BearerAuthMissingAudienceRejected) {
    auto port = PickFreePort(kTestBasePort + 1860);
    auto opts = BearerServerOptions(port);
    opts.bearer_auth->expected_resource =
        "http://127.0.0.1:" + std::to_string(port);
    opts.bearer_auth->verify = [](const std::string&) {
        AuthResult result;
        result.ok = true;
        result.scopes = {"mcp:read"};
        return result;
    };
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto resp = HttpPost(port, "/mcp", kNotificationBody, "Bearer test-token");

    EXPECT_EQ(resp.status_code, 401);
    EXPECT_NE(HeaderOf(resp, "WWW-Authenticate").find("invalid_token"),
              std::string::npos);
    server.Close();
}

TEST(StreamableHttpTransportTest, BearerAuthAudienceCheckDisabled) {
    auto port = PickFreePort(kTestBasePort + 1880);
    auto opts = BearerServerOptions(port);
    opts.bearer_auth->expected_resource = "https://other.example/resource";
    opts.bearer_auth->validate_token_resource = false;
    opts.bearer_auth->verify = [](const std::string&) {
        AuthResult result;
        result.ok = true;
        result.scopes = {"mcp:read"};
        result.resource = "https://other.example/resource";
        return result;
    };
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto resp = HttpPost(port, "/mcp", kNotificationBody, "Bearer test-token");

    EXPECT_EQ(resp.status_code, 202);
    server.Close();
}

TEST(StreamableHttpTransportTest, BearerAuthForbiddenWithoutInsufficientScopeNoRetry) {
    auto port = PickFreePort(kTestBasePort + 1840);
    mcp::HttpServer mock(port);
    std::atomic<int> calls{0};
    mock.SetHandler("POST", "/mcp",
        [&calls](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
            calls.fetch_add(1);
            resp.status_code = 403;
            resp.status_text = "Forbidden";
            resp.body = R"({"jsonrpc":"2.0","id":1,"error":{"code":-32000,"message":"forbidden"}})";
            resp.headers["content-type"] = "application/json";
        });
    mock.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    HttpClientTransportOptions client_opts;
    client_opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    std::atomic<int> challenge_calls{0};
    client_opts.auth_challenge_handler = [&challenge_calls](std::string_view) {
        challenge_calls.fetch_add(1);
        return "Bearer test-token";
    };
    StreamableHttpClientTransport client(client_opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);
    auto& channel = transport->GetMessageChannel();

    JsonRpcRequest call;
    call.method = "tools/call";
    call.id = 1;
    transport->SendMessageAsync(std::move(call));

    std::atomic<bool> got_error{false};
    JsonRpcMessage error_msg;
    channel.AsyncReceive([&](std::error_code ec, JsonRpcMessage msg) {
        if (!ec) {
            error_msg = std::move(msg);
            got_error.store(true);
        }
    });
    WaitUntil(got_error);
    ASSERT_TRUE(got_error.load());

    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(challenge_calls.load(), 0);
    auto* err = AsError(error_msg);
    ASSERT_TRUE(err != nullptr);
    EXPECT_EQ(err->error.message, "forbidden");

    transport->Close();
    mock.Stop();
}

TEST(StreamableHttpTransportTest, BearerAuthValidTokenReachesHandler) {
    auto port = PickFreePort(kTestBasePort + 1680);
    auto opts = BearerServerOptions(port);
    opts.bearer_auth->verify = [](const std::string& token) {
        AuthResult result;
        result.ok = (token == "test-token");
        result.scopes = {"mcp:read"};
        return result;
    };
    opts.bearer_auth->required_scopes = {"mcp:read"};
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto resp = HttpPost(port, "/mcp", kNotificationBody, "Bearer test-token");

    EXPECT_EQ(resp.status_code, 202);
    server.Close();
}

TEST(StreamableHttpTransportTest, BearerAuthMetadataEndpointAnonymous) {
    auto port = PickFreePort(kTestBasePort + 1700);
    auto opts = BearerServerOptions(port);
    opts.bearer_auth->verify = [](const std::string&) { return AuthResult{}; };
    opts.bearer_auth->authorization_servers = {"https://auth.example.com"};
    opts.bearer_auth->scopes_supported = {"mcp:read", "mcp:write"};
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto resp = HttpGet(port, "/.well-known/oauth-protected-resource");

    EXPECT_EQ(resp.status_code, 200);
    auto doc = JsonValue::Parse(resp.body);
    ASSERT_TRUE(doc.IsObject());
    auto* resource = doc.Find("resource");
    ASSERT_TRUE(resource != nullptr && resource->IsString());
    EXPECT_EQ(resource->GetString(), "http://127.0.0.1:" + std::to_string(port));
    auto* servers = doc.Find("authorization_servers");
    ASSERT_TRUE(servers != nullptr && servers->IsArray());
    ASSERT_EQ(servers->GetArray().size(), 1u);
    EXPECT_EQ(servers->GetArray()[0].GetString(), "https://auth.example.com");
    auto* scopes = doc.Find("scopes_supported");
    ASSERT_TRUE(scopes != nullptr && scopes->IsArray());
    ASSERT_EQ(scopes->GetArray().size(), 2u);
    EXPECT_EQ(scopes->GetArray()[0].GetString(), "mcp:read");
    EXPECT_EQ(scopes->GetArray()[1].GetString(), "mcp:write");
    auto* methods = doc.Find("bearer_methods_supported");
    ASSERT_TRUE(methods != nullptr && methods->IsArray());
    ASSERT_EQ(methods->GetArray().size(), 1u);
    EXPECT_EQ(methods->GetArray()[0].GetString(), "header");
    server.Close();
}

TEST(StreamableHttpTransportTest, BearerAuthUnconfiguredKeepsLegacyBehavior) {
    auto port = PickFreePort(kTestBasePort + 1720);
    StreamableHttpServerOptions opts;
    opts.port = port;
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto without_header = HttpPost(port, "/mcp", kNotificationBody);
    auto with_header = HttpPost(port, "/mcp", kNotificationBody, "Bearer anything");

    EXPECT_EQ(without_header.status_code, 202);
    EXPECT_EQ(with_header.status_code, 202);
    EXPECT_TRUE(HeaderOf(without_header, "WWW-Authenticate").empty());
    EXPECT_TRUE(HeaderOf(with_header, "WWW-Authenticate").empty());
    server.Close();
}

TEST(StreamableHttpTransportTest, BearerAuthClientLoopSuccess) {
    auto port = PickFreePort(kTestBasePort + 1740);
    auto opts = BearerServerOptions(port);
    opts.bearer_auth->verify = [](const std::string& token) {
        AuthResult result;
        result.ok = (token == "test-token");
        result.scopes = {"mcp:read"};
        return result;
    };
    opts.bearer_auth->required_scopes = {"mcp:read"};
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    HttpClientTransportOptions client_opts;
    client_opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    std::mutex challenge_mutex;
    std::string challenge;
    client_opts.auth_challenge_handler = [&](std::string_view www_authenticate) {
        std::lock_guard<std::mutex> lock(challenge_mutex);
        challenge = std::string(www_authenticate);
        return "Bearer test-token";
    };
    StreamableHttpClientTransport client(client_opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);
    auto& channel = transport->GetMessageChannel();

    JsonRpcMessage incoming;
    std::atomic<bool> got_request{false};
    std::thread server_consumer([&] {
        server.GetMessageChannel().AsyncReceive(
            [&](std::error_code ec, JsonRpcMessage msg) {
                if (!ec) {
                    incoming = std::move(msg);
                    got_request.store(true);
                }
            });
    });

    JsonRpcRequest call;
    call.method = "tools/call";
    call.id = 1;
    transport->SendMessageAsync(std::move(call));

    WaitUntil(got_request);
    if (!got_request.load()) {
        transport->Close();
        server.Close();
        server_consumer.join();
        return;
    }
    server_consumer.join();

    auto* server_request = AsRequest(incoming);
    ASSERT_TRUE(server_request != nullptr);
    EXPECT_EQ(server_request->method, "tools/call");

    JsonRpcResponse ok;
    ok.id = server_request->id;
    ok.result = JsonValue::Parse(R"({"ok":true})");
    server.SendMessageAsync(JsonRpcMessage(std::move(ok)));

    std::atomic<bool> got_result{false};
    JsonRpcMessage result_msg;
    std::thread receiver([&] {
        channel.AsyncReceive([&](std::error_code ec, JsonRpcMessage msg) {
            if (!ec) {
                result_msg = std::move(msg);
                got_result.store(true);
            }
        });
    });

    WaitUntil(got_result);
    if (!got_result.load()) {
        transport->Close();
        server.Close();
        receiver.join();
        return;
    }
    receiver.join();

    auto* response = AsResponse(result_msg);
    ASSERT_TRUE(response != nullptr);
    EXPECT_EQ(std::get<int64_t>(response->id), 1);
    {
        std::lock_guard<std::mutex> lock(challenge_mutex);
        EXPECT_EQ(challenge,
            "Bearer resource_metadata=\"" + MetadataUrlFor(port) + "\"");
    }

    transport->Close();
    server.Close();
}

TEST(StreamableHttpTransportTest, BearerAuthClientLoopInsufficientScope) {
    auto port = PickFreePort(kTestBasePort + 1760);
    auto opts = BearerServerOptions(port);
    opts.bearer_auth->verify = [](const std::string& token) {
        AuthResult result;
        result.ok = (token == "test-token");
        result.scopes = {"mcp:read"};
        return result;
    };
    opts.bearer_auth->required_scopes = {"mcp:write"};
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    HttpClientTransportOptions client_opts;
    client_opts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    std::atomic<int> challenge_calls{0};
    client_opts.auth_challenge_handler = [&challenge_calls](std::string_view) {
        challenge_calls.fetch_add(1);
        return "Bearer test-token";
    };
    StreamableHttpClientTransport client(client_opts);
    auto transport = client.Connect();
    ASSERT_NE(transport, nullptr);
    auto& channel = transport->GetMessageChannel();

    JsonRpcRequest call;
    call.method = "tools/call";
    call.id = 1;
    transport->SendMessageAsync(std::move(call));

    std::atomic<bool> got_error{false};
    JsonRpcMessage error_msg;
    channel.AsyncReceive([&](std::error_code ec, JsonRpcMessage msg) {
        if (!ec) {
            error_msg = std::move(msg);
            got_error.store(true);
        }
    });
    WaitUntil(got_error);
    ASSERT_TRUE(got_error.load());

    auto* err = AsError(error_msg);
    ASSERT_TRUE(err != nullptr);
    EXPECT_EQ(err->error.code, McpErrorCode::ConnectionClosed);
    EXPECT_EQ(err->error.message, "insufficient_scope");
    EXPECT_EQ(challenge_calls.load(), 1);

    transport->Close();
    server.Close();
}

namespace {

class BoundedSessionStore : public SessionStore {
public:
    explicit BoundedSessionStore(size_t capacity) : capacity_(capacity) {}
    void Save(const std::string& session_id, const SessionRecord& record) override {
        if (!TrySave(session_id, record))
            throw std::runtime_error("session store is full");
    }
    bool TrySave(const std::string& session_id, const SessionRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(session_id);
        if (it == records_.end() && records_.size() >= capacity_) return false;
        records_[session_id] = record;
        return true;
    }
    std::optional<SessionRecord> Load(const std::string& session_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(session_id);
        if (it == records_.end()) return std::nullopt;
        return it->second;
    }
    void Remove(const std::string& session_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.erase(session_id);
    }
    size_t Size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_.size();
    }
private:
    size_t capacity_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, SessionRecord> records_;
};

std::string RawSocketGet(uint16_t port, const std::string& target,
                         const std::string& session_id) {
    std::string received;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return received;
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return received;
    }
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return received;
#endif
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int connected = connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (connected == 0) {
        std::string req = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" +
            "mcp-session-id: " + session_id + "\r\nConnection: close\r\n\r\n";
        int sent = send(sock, req.c_str(), static_cast<int>(req.size()), 0);
        if (sent > 0) {
            char buf[4096];
            int n = recv(sock, buf, sizeof(buf) - 1, 0);
            if (n > 0) received.assign(buf, static_cast<size_t>(n));
        }
    }
#ifdef _WIN32
    closesocket(sock);
    WSACleanup();
#else
    close(sock);
#endif
    return received;
}

} // namespace

TEST(StreamableHttpTransportTest, SessionExhaustionMapsTo503) {
    auto store = std::make_shared<BoundedSessionStore>(1);
    SessionRecord filler;
    filler.protocol_version = "2025-06-18";
    ASSERT_TRUE(store->TrySave("filler-session", filler));

    auto port = PickFreePort(kTestBasePort + 1950);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = false;
    opts.session_store = store;
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto resp = HttpPost(port, "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
        R"("protocolVersion":"2025-06-18","capabilities":{},)"
        R"("clientInfo":{"name":"test","version":"0"}}})");
    EXPECT_EQ(resp.status_code, 503);
    EXPECT_NE(resp.body.find("session store is full"), std::string::npos);
    EXPECT_EQ(store->Size(), size_t{1});

    server.Close();
}

TEST(StreamableHttpTransportTest, GetKeepsSessionAlive) {
    auto store = std::make_shared<InMemorySessionStore>();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    SessionRecord record;
    record.protocol_version = "2025-06-18";
    record.created_at_ms = now_ms - 29LL * 60 * 1000;
    record.last_seen_ms = now_ms - 29LL * 60 * 1000;
    ASSERT_TRUE(store->TrySave("sess-get-alive", record));

    auto port = PickFreePort(kTestBasePort + 1960);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = false;
    opts.enable_legacy_sse = true;
    opts.session_store = store;
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto raw = RawSocketGet(port, "/mcp", "sess-get-alive");
    ASSERT_FALSE(raw.empty());
    EXPECT_NE(raw.find("200"), std::string::npos);

    auto after = store->Load("sess-get-alive");
    ASSERT_TRUE(after.has_value());
    EXPECT_GT(after->last_seen_ms, record.last_seen_ms);

    server.Close();
}

namespace {

size_t CountSseFrames(const std::string& body) {
    size_t frames = 0;
    size_t at = 0;
    while ((at = body.find("event: message", at)) != std::string::npos) {
        ++frames;
        ++at;
    }
    return frames;
}

mcp::detail::net::HttpResponseInfo HoldOpenPost(
    uint16_t port, const std::string& body,
    const std::unordered_map<std::string, std::string>& extra_headers) {
    mcp::detail::net::HttpClient client;
    mcp::detail::net::HttpRequestSpec req;
    req.method = "POST";
    req.url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    req.body = body;
    req.headers["Content-Type"] = "application/json";
    req.headers["Accept"] = "application/json, text/event-stream";
    for (const auto& [key, value] : extra_headers) req.headers[key] = value;
    req.timeout = std::chrono::seconds(5);
    return client.Request(req);
}

void RespondWithResult(StreamableHttpServerTransport& server,
                       const RequestId& id,
                       std::promise<void>* done) {
    JsonRpcResponse resp;
    resp.id = id;
    JsonValue result(JsonValue::object_tag);
    result["ok"] = JsonValue(true);
    resp.result = std::move(result);
    server.SendMessageAsync(JsonRpcMessage(std::move(resp)));
    if (done) done->set_value();
}

struct SseStreamReader {
    mcp::detail::net::HttpClient client;
    std::string chunks;
    std::promise<void> done;
    std::thread worker;
    ~SseStreamReader() {
        client.Close();
        if (worker.joinable()) worker.join();
    }
    void Start(uint16_t port, const std::string& body,
               const std::unordered_map<std::string, std::string>& extra_headers,
               size_t want_frames) {
        mcp::detail::net::HttpRequestSpec req;
        req.method = "POST";
        req.url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
        req.body = body;
        req.headers["Content-Type"] = "application/json";
        req.headers["Accept"] = "application/json, text/event-stream";
        for (const auto& [key, value] : extra_headers) req.headers[key] = value;
        req.timeout = std::chrono::seconds(5);
        worker = std::thread([this, req, want_frames] {
            try {
                client.Request(req, [&](std::string_view chunk) {
                    chunks.append(chunk.data(), chunk.size());
                    if (CountSseFrames(chunks) >= want_frames) {
                        try {
                            done.set_value();
                        } catch (...) {
                        }
                    }
                });
            } catch (...) {
            }
            try {
                done.set_value();
            } catch (...) {
            }
        });
    }
    void Stop() {
        client.Close();
        if (worker.joinable()) worker.join();
    }
};

} // namespace

TEST(StreamableHttpTransportTest, RequestStreamHoldOpenDeliversProgressBeforeResult) {
    auto port = PickFreePort(kTestBasePort + 2050);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    StreamableHttpServerTransport server(opts);
    std::promise<void> consumed;
    auto consumed_future = consumed.get_future();
    std::thread consumer([&] {
        server.GetMessageChannel().AsyncReceive(
            [&](std::error_code ec, JsonRpcMessage msg) {
                if (ec) return;
                auto* req = AsRequest(msg);
                if (!req) return;
                JsonRpcNotification progress;
                progress.method = "notifications/progress";
                JsonValue progress_params(JsonValue::object_tag);
                progress_params["progressToken"] = JsonValue("tok-hold");
                progress_params["progress"] = JsonValue(0.5);
                progress.params = std::move(progress_params);
                server.SendMessageAsync(JsonRpcMessage(std::move(progress)));
                RespondWithResult(server, req->id, &consumed);
            });
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::unordered_map<std::string, std::string> headers;
    headers["Mcp-Method"] = "tools/call";
    headers["Mcp-Name"] = "echo";
    headers["Mcp-Protocol-Version"] = "2026-07-28";
    auto resp = HoldOpenPost(port,
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call",)"
        R"("params":{"name":"echo","arguments":{},)"
        R"("_meta":{"progressToken":"tok-hold"}}})",
        headers);

    EXPECT_EQ(consumed_future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    EXPECT_EQ(resp.status_code, 200);
    auto content_type = resp.headers.find("content-type");
    ASSERT_TRUE(content_type != resp.headers.end());
    EXPECT_EQ(content_type->second, "text/event-stream");
    auto progress_pos = resp.body.find("notifications/progress");
    auto result_pos = resp.body.find("\"ok\":true");
    EXPECT_NE(progress_pos, std::string::npos);
    EXPECT_NE(result_pos, std::string::npos);
    EXPECT_LT(progress_pos, result_pos);
    EXPECT_EQ(CountSseFrames(resp.body), size_t{2});

    server.Close();
    consumer.join();
}

TEST(StreamableHttpTransportTest, PlainRequestKeepsSingleWriteAndClose) {
    auto port = PickFreePort(kTestBasePort + 2060);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    StreamableHttpServerTransport server(opts);
    std::promise<void> consumed;
    auto consumed_future = consumed.get_future();
    std::thread consumer([&] {
        server.GetMessageChannel().AsyncReceive(
            [&](std::error_code ec, JsonRpcMessage msg) {
                if (ec) return;
                auto* req = AsRequest(msg);
                if (!req) return;
                RespondWithResult(server, req->id, &consumed);
            });
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto resp = HoldOpenPost(port,
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call",)"
        R"("params":{"name":"echo","arguments":{}}})",
        {});

    EXPECT_EQ(consumed_future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    EXPECT_EQ(resp.status_code, 200);
    auto content_type = resp.headers.find("content-type");
    ASSERT_TRUE(content_type != resp.headers.end());
    EXPECT_EQ(content_type->second, "text/event-stream");
    EXPECT_EQ(CountSseFrames(resp.body), size_t{1});
    EXPECT_NE(resp.body.find("\"ok\":true"), std::string::npos);
    EXPECT_EQ(resp.body.find("notifications/progress"), std::string::npos);

    server.Close();
    consumer.join();
}

TEST(StreamableHttpTransportTest, DPoPBoundTokenRequiresMatchingProof) {
    auto port = PickFreePort(kTestBasePort + 2100);
    auto opts = BearerServerOptions(port);
    opts.bearer_auth->verify = [](const std::string&) {
        AuthResult result;
        result.ok = true;
        result.scopes = {"mcp:read"};
        return result;
    };
    opts.bearer_auth->enable_dpop = true;
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string htu = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    bool have_crypto = true;
    std::string proof;
    std::string wrong_htm_proof;
    try {
        auto key = mcp::ext::dpop::GenerateKey();
        proof = mcp::ext::dpop::CreateProof(
            key, "POST", htu, std::string_view("dpop-token"));
        wrong_htm_proof = mcp::ext::dpop::CreateProof(
            key, "GET", htu, std::string_view("dpop-token"));
    } catch (const std::exception&) {
        have_crypto = false;
    }
    if (have_crypto) {
        auto ok_resp = HttpPostDpop(port, "/mcp", kNotificationBody,
                                    "DPoP dpop-token", proof);
        EXPECT_EQ(ok_resp.status_code, 202);
        EXPECT_FALSE(HeaderOf(ok_resp, "DPoP-Nonce").empty());

        auto wrong_token = HttpPostDpop(port, "/mcp", kNotificationBody,
                                        "DPoP other-token", proof);
        EXPECT_EQ(wrong_token.status_code, 401);
        EXPECT_NE(HeaderOf(wrong_token, "WWW-Authenticate").find("invalid_token"),
                  std::string::npos);
        EXPECT_FALSE(HeaderOf(wrong_token, "DPoP-Nonce").empty());

        auto wrong_htm = HttpPostDpop(port, "/mcp", kNotificationBody,
                                      "DPoP dpop-token", wrong_htm_proof);
        EXPECT_EQ(wrong_htm.status_code, 401);
        EXPECT_NE(HeaderOf(wrong_htm, "WWW-Authenticate").find("invalid_token"),
                  std::string::npos);
    } else {
        auto resp = HttpPostDpop(port, "/mcp", kNotificationBody,
                                 "DPoP dpop-token", "invalid.proof.value");
        EXPECT_EQ(resp.status_code, 401);
        EXPECT_NE(HeaderOf(resp, "WWW-Authenticate").find("invalid_token"),
                  std::string::npos);
        EXPECT_FALSE(HeaderOf(resp, "DPoP-Nonce").empty());
    }
    server.Close();
}

TEST(StreamableHttpTransportTest, DPoPProofReplayRejected) {
    auto port = PickFreePort(kTestBasePort + 2110);
    auto opts = BearerServerOptions(port);
    opts.bearer_auth->verify = [](const std::string&) {
        AuthResult result;
        result.ok = true;
        return result;
    };
    opts.bearer_auth->enable_dpop = true;
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string htu = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    bool have_crypto = true;
    std::string proof = "replay.probe.proof";
    try {
        auto key = mcp::ext::dpop::GenerateKey();
        proof = mcp::ext::dpop::CreateProof(
            key, "POST", htu, std::string_view("dpop-token"));
    } catch (const std::exception&) {
        have_crypto = false;
    }
    auto first = HttpPostDpop(port, "/mcp", kNotificationBody,
                              "DPoP dpop-token", proof);
    auto second = HttpPostDpop(port, "/mcp", kNotificationBody,
                               "DPoP dpop-token", proof);
    if (have_crypto) {
        EXPECT_EQ(first.status_code, 202);
        EXPECT_EQ(second.status_code, 401);
        EXPECT_NE(HeaderOf(second, "WWW-Authenticate").find("invalid_token"),
                  std::string::npos);
    } else {
        EXPECT_EQ(first.status_code, 401);
        EXPECT_EQ(second.status_code, 401);
    }

    mcp::ext::dpop::JtiReplayCache cache(300);
    int64_t now = mcp::ext::dpop::NowSeconds();
    EXPECT_TRUE(cache.CheckAndStore("jti-first", now, now));
    EXPECT_FALSE(cache.CheckAndStore("jti-first", now, now));
    EXPECT_TRUE(cache.CheckAndStore("jti-stale", now - 1000, now));
    EXPECT_TRUE(cache.CheckAndStore("jti-fresh", now, now));
    EXPECT_TRUE(cache.CheckAndStore("jti-stale", now - 1000, now));
    EXPECT_FALSE(cache.CheckAndStore("", now, now));
    server.Close();
}

TEST(StreamableHttpTransportTest, DPoPDisabledSkipsProofChecks) {
    auto port = PickFreePort(kTestBasePort + 2120);
    auto opts = BearerServerOptions(port);
    EXPECT_FALSE(opts.bearer_auth->enable_dpop);
    opts.bearer_auth->verify = [](const std::string& token) {
        AuthResult result;
        result.ok = (token == "plain-token");
        return result;
    };
    opts.bearer_auth->enable_dpop = false;
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto ok_resp = HttpPost(port, "/mcp", kNotificationBody, "Bearer plain-token");
    EXPECT_EQ(ok_resp.status_code, 202);
    EXPECT_TRUE(HeaderOf(ok_resp, "DPoP-Nonce").empty());

    auto dpop_scheme = HttpPostDpop(port, "/mcp", kNotificationBody,
                                    "DPoP plain-token", "any.proof.value");
    EXPECT_EQ(dpop_scheme.status_code, 401);
    EXPECT_EQ(HeaderOf(dpop_scheme, "WWW-Authenticate"),
        "Bearer resource_metadata=\"" + MetadataUrlFor(port) + "\"");
    EXPECT_TRUE(HeaderOf(dpop_scheme, "DPoP-Nonce").empty());
    server.Close();
}

TEST(StreamableHttpTransportTest, RequestStreamHoldOpenDeliversMessageBeforeResult) {
    auto port = PickFreePort(kTestBasePort + 2070);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    StreamableHttpServerTransport server(opts);
    std::promise<void> consumed;
    auto consumed_future = consumed.get_future();
    std::thread consumer([&] {
        server.GetMessageChannel().AsyncReceive(
            [&](std::error_code ec, JsonRpcMessage msg) {
                if (ec) return;
                auto* req = AsRequest(msg);
                if (!req) return;
                JsonRpcNotification log;
                log.method = "notifications/message";
                JsonValue log_params(JsonValue::object_tag);
                log_params["level"] = JsonValue("info");
                log_params["data"] = JsonValue("halfway");
                log.params = std::move(log_params);
                server.SendMessageAsync(JsonRpcMessage(std::move(log)));
                RespondWithResult(server, req->id, &consumed);
            });
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::unordered_map<std::string, std::string> headers;
    headers["Mcp-Method"] = "tools/call";
    headers["Mcp-Name"] = "echo";
    headers["Mcp-Protocol-Version"] = "2026-07-28";
    auto resp = HoldOpenPost(port,
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call",)"
        R"("params":{"name":"echo","arguments":{}}})",
        headers);

    EXPECT_EQ(consumed_future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    EXPECT_EQ(resp.status_code, 200);
    auto content_type = resp.headers.find("content-type");
    ASSERT_TRUE(content_type != resp.headers.end());
    EXPECT_EQ(content_type->second, "text/event-stream");
    auto message_pos = resp.body.find("notifications/message");
    auto result_pos = resp.body.find("\"ok\":true");
    EXPECT_NE(message_pos, std::string::npos);
    EXPECT_NE(result_pos, std::string::npos);
    EXPECT_LT(message_pos, result_pos);
    EXPECT_EQ(CountSseFrames(resp.body), size_t{2});

    server.Close();
    consumer.join();
}

namespace {

const char* kStatelessMetaFull =
    R"("_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
    R"("io.modelcontextprotocol/clientInfo":{"name":"conformance-client","version":"1.0.0"},)"
    R"("io.modelcontextprotocol/clientCapabilities":{}})";

std::unordered_map<std::string, std::string> StatelessModernHeaders(
    const std::string& method) {
    std::unordered_map<std::string, std::string> headers;
    headers["Mcp-Protocol-Version"] = "2026-07-28";
    headers["Mcp-Method"] = method;
    return headers;
}

int64_t ErrorCodeOf(const std::string& body) {
    auto doc = JsonValue::Parse(body);
    auto* err = doc.Find("error");
    if (!err || !err->IsObject()) return 0;
    auto* code = err->Find("code");
    if (!code || !code->IsInt()) return 0;
    return code->GetInt();
}

int64_t IdOf(const std::string& body) {
    auto doc = JsonValue::Parse(body);
    auto* id = doc.Find("id");
    if (!id || !id->IsInt()) return -1;
    return id->GetInt();
}

} // namespace

TEST(StreamableHttpTransportTest, StatelessModernDiscoverMissingMetaRejected) {
    auto port = PickFreePort(kTestBasePort + 2080);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    const std::string bodies[] = {
        R"({"jsonrpc":"2.0","id":101,"method":"server/discover","params":{}})",
        std::string(R"({"jsonrpc":"2.0","id":102,"method":"server/discover","params":{"_meta":{)") +
            std::string(R"("io.modelcontextprotocol/clientInfo":{"name":"c","version":"1"},)") +
            std::string(R"("io.modelcontextprotocol/clientCapabilities":{}}}})"),
        std::string(R"({"jsonrpc":"2.0","id":104,"method":"server/discover","params":{"_meta":{)") +
            std::string(R"("io.modelcontextprotocol/protocolVersion":"2026-07-28",)") +
            std::string(R"("io.modelcontextprotocol/clientInfo":{"name":"c","version":"1"}}}})"),
    };
    const int64_t ids[] = {101, 102, 104};
    for (size_t i = 0; i < 3; ++i) {
        auto resp = HoldOpenPost(port, bodies[i],
                                 StatelessModernHeaders("server/discover"));
        EXPECT_EQ(resp.status_code, 400);
        EXPECT_EQ(ErrorCodeOf(resp.body),
                  static_cast<int64_t>(McpErrorCode::InvalidParams));
        EXPECT_EQ(IdOf(resp.body), ids[i]);
    }

    server.Close();
}

TEST(StreamableHttpTransportTest, StatelessModernDiscoverClientInfoOptionalServed) {
    auto port = PickFreePort(kTestBasePort + 2082);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    StreamableHttpServerTransport server(opts);
    std::promise<void> consumed;
    auto consumed_future = consumed.get_future();
    std::thread consumer([&] {
        server.GetMessageChannel().AsyncReceive(
            [&](std::error_code ec, JsonRpcMessage msg) {
                if (ec) return;
                auto* req = AsRequest(msg);
                if (!req) return;
                RespondWithResult(server, req->id, &consumed);
            });
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string body =
        std::string(R"({"jsonrpc":"2.0","id":105,"method":"server/discover","params":{)") +
        R"("_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28",)" +
        R"("io.modelcontextprotocol/clientCapabilities":{}}}})";
    auto resp = HoldOpenPost(port, body,
                             StatelessModernHeaders("server/discover"));

    EXPECT_EQ(consumed_future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_NE(resp.body.find("\"ok\":true"), std::string::npos);
    EXPECT_EQ(CountSseFrames(resp.body), size_t{1});

    server.Close();
    consumer.join();
}

TEST(StreamableHttpTransportTest, StatelessModernVersionMismatchRejected) {
    auto port = PickFreePort(kTestBasePort + 2084);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string body =
        std::string(R"({"jsonrpc":"2.0","id":302,"method":"server/discover","params":{)") +
        std::string(kStatelessMetaFull) + "}}";
    auto dot = body.find("\"2026-07-28\"");
    ASSERT_NE(dot, std::string::npos);
    body.replace(dot, std::string("\"2026-07-28\"").size(), "\"v999.0.0\"");
    auto resp = HoldOpenPost(port, body,
                             StatelessModernHeaders("server/discover"));

    EXPECT_EQ(resp.status_code, 400);
    EXPECT_EQ(ErrorCodeOf(resp.body),
              static_cast<int64_t>(McpErrorCode::HeaderMismatch));
    EXPECT_EQ(IdOf(resp.body), 302);

    server.Close();
}

TEST(StreamableHttpTransportTest, StatelessUnsupportedVersionCarriesDataAndId) {
    auto port = PickFreePort(kTestBasePort + 2086);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string body =
        std::string(R"({"jsonrpc":"2.0","id":301,"method":"server/discover","params":{)") +
        std::string(kStatelessMetaFull) + "}}";
    auto dot = body.find("\"2026-07-28\"");
    ASSERT_NE(dot, std::string::npos);
    body.replace(dot, std::string("\"2026-07-28\"").size(), "\"v999.0.0\"");
    std::unordered_map<std::string, std::string> headers;
    headers["Mcp-Protocol-Version"] = "v999.0.0";
    headers["Mcp-Method"] = "server/discover";
    auto resp = HoldOpenPost(port, body, headers);

    EXPECT_EQ(resp.status_code, 400);
    EXPECT_EQ(ErrorCodeOf(resp.body),
              static_cast<int64_t>(McpErrorCode::UnsupportedProtocolVersion));
    EXPECT_EQ(IdOf(resp.body), 301);
    auto doc = JsonValue::Parse(resp.body);
    auto* err = doc.Find("error");
    ASSERT_TRUE(err != nullptr && err->IsObject());
    auto* data = err->Find("data");
    ASSERT_TRUE(data != nullptr && data->IsObject());
    auto* supported = data->Find("supported");
    ASSERT_TRUE(supported != nullptr && supported->IsArray());
    ASSERT_FALSE(supported->GetArray().empty());
    bool has_latest = false;
    for (const auto& entry : supported->GetArray()) {
        if (entry.IsString() && entry.GetString() == "2026-07-28") has_latest = true;
    }
    EXPECT_TRUE(has_latest);
    auto* requested = data->Find("requested");
    ASSERT_TRUE(requested != nullptr && requested->IsString());
    EXPECT_EQ(requested->GetString(), "v999.0.0");

    server.Close();
}

TEST(StreamableHttpTransportTest, StatelessModernRemovedMethodsMapTo404) {
    auto port = PickFreePort(kTestBasePort + 2088);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    const std::pair<const char*, int64_t> cases[] = {
        {"initialize", 501},
        {"ping", 502},
        {"logging/setLevel", 503},
    };
    for (const auto& [method, rpc_id] : cases) {
        std::string body = std::string(R"({"jsonrpc":"2.0","id":)") +
                           std::to_string(rpc_id) +
                           std::string(R"(,"method":")") + method +
                           std::string(R"(","params":{)") +
                           std::string(kStatelessMetaFull) + "}}";
        auto resp = HoldOpenPost(port, body, StatelessModernHeaders(method));
        EXPECT_EQ(resp.status_code, 404);
        EXPECT_EQ(ErrorCodeOf(resp.body),
                  static_cast<int64_t>(McpErrorCode::MethodNotFound));
        EXPECT_EQ(IdOf(resp.body), rpc_id);
    }

    server.Close();
}

TEST(StreamableHttpTransportTest, LegacyInitializeWithoutSignalsPassesThrough) {
    auto port = PickFreePort(kTestBasePort + 2089);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    StreamableHttpServerTransport server(opts);
    std::promise<void> consumed;
    auto consumed_future = consumed.get_future();
    std::thread consumer([&] {
        server.GetMessageChannel().AsyncReceive(
            [&](std::error_code ec, JsonRpcMessage msg) {
                if (ec) return;
                auto* req = AsRequest(msg);
                if (!req) return;
                RespondWithResult(server, req->id, &consumed);
            });
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    auto resp = HoldOpenPost(port,
        R"({"jsonrpc":"2.0","id":601,"method":"initialize","params":{)"
        R"("protocolVersion":"2025-03-26","capabilities":{},)"
        R"("clientInfo":{"name":"c","version":"1"}}})",
        {});

    EXPECT_EQ(consumed_future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    EXPECT_NE(resp.status_code, 404);

    server.Close();
    consumer.join();
}

TEST(StreamableHttpTransportTest, StatelessSubscriptionAckBufferedFirst) {
    auto port = PickFreePort(kTestBasePort + 2090);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    StreamableHttpServerTransport server(opts);
    std::promise<void> consumed;
    auto consumed_future = consumed.get_future();
    std::thread consumer([&] {
        server.GetMessageChannel().AsyncReceive(
            [&](std::error_code ec, JsonRpcMessage msg) {
                if (ec) return;
                auto* req = AsRequest(msg);
                if (!req) return;
                JsonRpcNotification ack;
                ack.method = "notifications/subscriptions/acknowledged";
                JsonValue ack_params(JsonValue::object_tag);
                JsonValue ack_meta(JsonValue::object_tag);
                ack_meta["io.modelcontextprotocol/subscriptionId"] =
                    JsonValue("sub-1");
                ack_params["_meta"] = std::move(ack_meta);
                ack.params = std::move(ack_params);
                server.SendMessageAsync(JsonRpcMessage(std::move(ack)));
                RespondWithResult(server, req->id, &consumed);
            });
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string body =
        std::string(R"({"jsonrpc":"2.0","id":700,"method":"subscriptions/listen",)") +
        std::string(R"("params":{)") + std::string(kStatelessMetaFull) +
        R"(,"notifications":{"toolsListChanged":true}}})";
    auto resp = HoldOpenPost(port, body,
                             StatelessModernHeaders("subscriptions/listen"));

    EXPECT_EQ(consumed_future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_EQ(CountSseFrames(resp.body), size_t{2});
    auto ack_pos = resp.body.find("notifications/subscriptions/acknowledged");
    auto result_pos = resp.body.find("\"ok\":true");
    EXPECT_NE(ack_pos, std::string::npos);
    EXPECT_NE(result_pos, std::string::npos);
    EXPECT_LT(ack_pos, result_pos);
    EXPECT_NE(resp.body.find("sub-1"), std::string::npos);

    server.Close();
    consumer.join();
}

TEST(StreamableHttpTransportTest, ListenHoldOpenBuffersListChangedIntoListenStream) {
    auto port = PickFreePort(kTestBasePort + 2110);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    opts.listen_hold_open = true;
    StreamableHttpServerTransport server(opts);
    std::promise<void> consumed;
    auto consumed_future = consumed.get_future();
    std::thread consumer([&] {
        server.GetMessageChannel().AsyncReceive(
            [&](std::error_code ec, JsonRpcMessage msg) {
                if (ec) return;
                auto* req = AsRequest(msg);
                if (!req) return;
                JsonRpcNotification changed;
                changed.method = "notifications/tools/list_changed";
                changed.params = JsonValue(JsonValue::object_tag);
                server.SendMessageAsync(JsonRpcMessage(std::move(changed)));
                JsonRpcNotification ack;
                ack.method = "notifications/subscriptions/acknowledged";
                JsonValue ack_params(JsonValue::object_tag);
                JsonValue ack_meta(JsonValue::object_tag);
                ack_meta["io.modelcontextprotocol/subscriptionId"] =
                    JsonValue("sub-hold");
                ack_params["_meta"] = std::move(ack_meta);
                ack.params = std::move(ack_params);
                server.SendMessageAsync(JsonRpcMessage(std::move(ack)));
                RespondWithResult(server, req->id, &consumed);
            });
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string body =
        std::string(R"({"jsonrpc":"2.0","id":710,"method":"subscriptions/listen",)") +
        std::string(R"("params":{)") + std::string(kStatelessMetaFull) +
        R"(,"notifications":{"toolsListChanged":true}}})";
    SseStreamReader stream;
    stream.Start(port, body, StatelessModernHeaders("subscriptions/listen"), 3);

    EXPECT_EQ(consumed_future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    EXPECT_EQ(stream.done.get_future().wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    stream.Stop();
    EXPECT_EQ(CountSseFrames(stream.chunks), size_t{3});
    auto changed_pos = stream.chunks.find("notifications/tools/list_changed");
    auto ack_pos = stream.chunks.find("notifications/subscriptions/acknowledged");
    auto result_pos = stream.chunks.find("\"ok\":true");
    EXPECT_NE(changed_pos, std::string::npos);
    EXPECT_NE(ack_pos, std::string::npos);
    EXPECT_NE(result_pos, std::string::npos);
    EXPECT_LT(changed_pos, ack_pos);
    EXPECT_LT(ack_pos, result_pos);
    EXPECT_NE(stream.chunks.find("sub-hold"), std::string::npos);

    server.Close();
    consumer.join();
}

TEST(StreamableHttpTransportTest, ListenHoldOpenSkipsUnmatchedListChanged) {
    auto port = PickFreePort(kTestBasePort + 2112);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    opts.listen_hold_open = true;
    StreamableHttpServerTransport server(opts);
    std::promise<void> consumed;
    auto consumed_future = consumed.get_future();
    std::thread consumer([&] {
        server.GetMessageChannel().AsyncReceive(
            [&](std::error_code ec, JsonRpcMessage msg) {
                if (ec) return;
                auto* req = AsRequest(msg);
                if (!req) return;
                JsonRpcNotification changed;
                changed.method = "notifications/tools/list_changed";
                changed.params = JsonValue(JsonValue::object_tag);
                server.SendMessageAsync(JsonRpcMessage(std::move(changed)));
                JsonRpcNotification ack;
                ack.method = "notifications/subscriptions/acknowledged";
                JsonValue ack_params(JsonValue::object_tag);
                JsonValue ack_meta(JsonValue::object_tag);
                ack_meta["io.modelcontextprotocol/subscriptionId"] =
                    JsonValue("sub-skip");
                ack_params["_meta"] = std::move(ack_meta);
                ack.params = std::move(ack_params);
                server.SendMessageAsync(JsonRpcMessage(std::move(ack)));
                RespondWithResult(server, req->id, &consumed);
            });
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string body =
        std::string(R"({"jsonrpc":"2.0","id":711,"method":"subscriptions/listen",)") +
        std::string(R"("params":{)") + std::string(kStatelessMetaFull) +
        R"(,"notifications":{"promptsListChanged":true}}})";
    SseStreamReader stream;
    stream.Start(port, body, StatelessModernHeaders("subscriptions/listen"), 2);

    EXPECT_EQ(consumed_future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    EXPECT_EQ(stream.done.get_future().wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    stream.Stop();
    EXPECT_EQ(CountSseFrames(stream.chunks), size_t{2});
    EXPECT_EQ(stream.chunks.find("notifications/tools/list_changed"),
              std::string::npos);
    EXPECT_NE(stream.chunks.find("notifications/subscriptions/acknowledged"),
              std::string::npos);
    EXPECT_NE(stream.chunks.find("\"ok\":true"), std::string::npos);
    EXPECT_NE(stream.chunks.find("sub-skip"), std::string::npos);

    server.Close();
    consumer.join();
}

TEST(StreamableHttpTransportTest, ListenBroadcastSkipsForeignToolsChangedOnPromptsStream) {
    auto port = PickFreePort(kTestBasePort + 2118);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    opts.listen_hold_open = true;
    StreamableHttpServerTransport server(opts);
    std::promise<void> consumed;
    auto consumed_future = consumed.get_future();
    std::thread consumer([&] {
        server.GetMessageChannel().AsyncReceive(
            [&](std::error_code ec, JsonRpcMessage msg) {
                if (ec) return;
                auto* req = AsRequest(msg);
                if (!req) return;
                RespondWithResult(server, req->id, &consumed);
            });
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string body =
        std::string(R"({"jsonrpc":"2.0","id":712,"method":"subscriptions/listen",)") +
        std::string(R"("params":{)") + std::string(kStatelessMetaFull) +
        R"(,"notifications":{"promptsListChanged":true}}})";
    SseStreamReader stream;
    stream.Start(port, body, StatelessModernHeaders("subscriptions/listen"), 2);

    EXPECT_EQ(consumed_future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    JsonRpcNotification foreign;
    foreign.method = "notifications/tools/list_changed";
    foreign.params = JsonValue(JsonValue::object_tag);
    JsonValue foreign_meta(JsonValue::object_tag);
    foreign_meta["io.modelcontextprotocol/subscriptionId"] = JsonValue("1");
    foreign.meta = std::move(foreign_meta);
    server.SendMessageAsync(JsonRpcMessage(std::move(foreign)));

    EXPECT_EQ(stream.done.get_future().wait_for(std::chrono::milliseconds(800)),
              std::future_status::timeout);
    stream.Stop();
    EXPECT_EQ(CountSseFrames(stream.chunks), size_t{1});
    EXPECT_EQ(stream.chunks.find("notifications/tools/list_changed"),
              std::string::npos);
    EXPECT_NE(stream.chunks.find("\"ok\":true"), std::string::npos);

    server.Close();
    consumer.join();
}

TEST(StreamableHttpTransportTest, ListenBroadcastSkipsForeignPromptsChangedOnToolsStream) {
    auto port = PickFreePort(kTestBasePort + 2122);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    opts.listen_hold_open = true;
    StreamableHttpServerTransport server(opts);
    std::promise<void> consumed;
    auto consumed_future = consumed.get_future();
    std::thread consumer([&] {
        server.GetMessageChannel().AsyncReceive(
            [&](std::error_code ec, JsonRpcMessage msg) {
                if (ec) return;
                auto* req = AsRequest(msg);
                if (!req) return;
                RespondWithResult(server, req->id, &consumed);
            });
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string body =
        std::string(R"({"jsonrpc":"2.0","id":713,"method":"subscriptions/listen",)") +
        std::string(R"("params":{)") + std::string(kStatelessMetaFull) +
        R"(,"notifications":{"toolsListChanged":true}}})";
    SseStreamReader stream;
    stream.Start(port, body, StatelessModernHeaders("subscriptions/listen"), 2);

    EXPECT_EQ(consumed_future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    JsonRpcNotification foreign;
    foreign.method = "notifications/prompts/list_changed";
    foreign.params = JsonValue(JsonValue::object_tag);
    JsonValue foreign_meta(JsonValue::object_tag);
    foreign_meta["io.modelcontextprotocol/subscriptionId"] = JsonValue("2");
    foreign.meta = std::move(foreign_meta);
    server.SendMessageAsync(JsonRpcMessage(std::move(foreign)));

    EXPECT_EQ(stream.done.get_future().wait_for(std::chrono::milliseconds(800)),
              std::future_status::timeout);
    stream.Stop();
    EXPECT_EQ(CountSseFrames(stream.chunks), size_t{1});
    EXPECT_EQ(stream.chunks.find("notifications/prompts/list_changed"),
              std::string::npos);
    EXPECT_NE(stream.chunks.find("\"ok\":true"), std::string::npos);

    server.Close();
    consumer.join();
}

TEST(StreamableHttpTransportTest, ListenBroadcastDeliversMatchingToolsChangedToOpenStream) {
    auto port = PickFreePort(kTestBasePort + 2124);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    opts.listen_hold_open = true;
    StreamableHttpServerTransport server(opts);
    std::promise<void> consumed;
    auto consumed_future = consumed.get_future();
    std::thread consumer([&] {
        server.GetMessageChannel().AsyncReceive(
            [&](std::error_code ec, JsonRpcMessage msg) {
                if (ec) return;
                auto* req = AsRequest(msg);
                if (!req) return;
                RespondWithResult(server, req->id, &consumed);
            });
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    std::string body =
        std::string(R"({"jsonrpc":"2.0","id":714,"method":"subscriptions/listen",)") +
        std::string(R"("params":{)") + std::string(kStatelessMetaFull) +
        R"(,"notifications":{"toolsListChanged":true}}})";
    SseStreamReader stream;
    stream.Start(port, body, StatelessModernHeaders("subscriptions/listen"), 2);

    EXPECT_EQ(consumed_future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    JsonRpcNotification changed;
    changed.method = "notifications/tools/list_changed";
    changed.params = JsonValue(JsonValue::object_tag);
    JsonValue changed_meta(JsonValue::object_tag);
    changed_meta["io.modelcontextprotocol/subscriptionId"] = JsonValue("7");
    changed.meta = std::move(changed_meta);
    server.SendMessageAsync(JsonRpcMessage(std::move(changed)));

    EXPECT_EQ(stream.done.get_future().wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    stream.Stop();
    EXPECT_EQ(CountSseFrames(stream.chunks), size_t{2});
    EXPECT_NE(stream.chunks.find("notifications/tools/list_changed"),
              std::string::npos);
    EXPECT_NE(stream.chunks.find("\"ok\":true"), std::string::npos);

    server.Close();
    consumer.join();
}

TEST(StreamableHttpTransportTest, StatelessSingleConnectionDefaults) {
    StreamableHttpServerOptions server_opts;
    EXPECT_TRUE(server_opts.stateless);
    EXPECT_FALSE(server_opts.enable_legacy_sse);
    EXPECT_FALSE(server_opts.listen_hold_open);

    HttpClientTransportOptions client_opts;
    EXPECT_FALSE(client_opts.stateless);
    EXPECT_TRUE(mcp::streamable_http_client_impl::ShouldListenStream(client_opts));
    client_opts.stateless = true;
    EXPECT_FALSE(mcp::streamable_http_client_impl::ShouldListenStream(client_opts));
    client_opts.enable_listen_stream = true;
    EXPECT_TRUE(mcp::streamable_http_client_impl::ShouldListenStream(client_opts));
}

TEST(StreamableHttpTransportTest, StatelessDeleteIsRejected) {
    auto port = PickFreePort(kTestBasePort + 2114);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    StreamableHttpServerTransport server(opts);
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    mcp::detail::net::HttpClient client;
    mcp::detail::net::HttpRequestSpec req;
    req.method = "DELETE";
    req.url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    req.timeout = std::chrono::seconds(5);
    auto resp = client.Request(req);

    EXPECT_EQ(resp.status_code, 405);
    EXPECT_EQ(ErrorCodeOf(resp.body),
              static_cast<int64_t>(McpErrorCode::MethodNotFound));

    server.Close();
}

TEST(StreamableHttpTransportTest, StatelessRequestsAreIndependentConnections) {
    auto port = PickFreePort(kTestBasePort + 2116);
    StreamableHttpServerOptions opts;
    opts.port = port;
    opts.stateless = true;
    StreamableHttpServerTransport server(opts);
    std::promise<void> consumed;
    auto consumed_future = consumed.get_future();
    std::atomic<int> seen{0};
    std::thread consumer([&] {
        for (int i = 0; i < 2; ++i) {
            std::promise<void> one;
            auto one_future = one.get_future();
            server.GetMessageChannel().AsyncReceive(
                [&](std::error_code ec, JsonRpcMessage msg) {
                    if (!ec) {
                        auto* req = AsRequest(msg);
                        if (req) RespondWithResult(server, req->id, nullptr);
                        seen.fetch_add(1);
                    }
                    try {
                        one.set_value();
                    } catch (...) {
                    }
                });
            one_future.wait();
        }
        try {
            consumed.set_value();
        } catch (...) {
        }
    });
    server.Start();
    ASSERT_TRUE(WaitUntilReady(port));

    for (int64_t rpc_id = 801; rpc_id <= 802; ++rpc_id) {
        std::string body = std::string(R"({"jsonrpc":"2.0","id":)") +
                           std::to_string(rpc_id) +
                           R"(,"method":"tools/call",)" +
                           R"("params":{"name":"echo","arguments":{}}})";
        auto resp = HoldOpenPost(port, body, {});
        EXPECT_EQ(resp.status_code, 200);
        EXPECT_EQ(CountSseFrames(resp.body), size_t{1});
        EXPECT_NE(resp.body.find("\"ok\":true"), std::string::npos);
        EXPECT_TRUE(HeaderOf(resp, "Mcp-Session-Id").empty());
    }

    EXPECT_EQ(consumed_future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    EXPECT_EQ(seen.load(), 2);

    server.Close();
    consumer.join();
}
