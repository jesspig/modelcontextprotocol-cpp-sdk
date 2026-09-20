#include <mcp/transport/StreamableHttpClientTransport.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/Transport.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/http/HttpServer.hpp>
#include <mcp/test/McpTest.hpp>
#include "TestServerUtil.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

using namespace mcp;

namespace mcp {
namespace streamable_http_client_impl {

enum class ListenState { Idle, Connecting, Streaming, Unsupported, GivenUp };

struct SseBlockParseResult {
    std::string data;
    std::string event_id;
    std::optional<JsonRpcMessage> message;
};

SseBlockParseResult ParseSseBlock(const std::string& block);
ListenState ListenStateForStatusCode(int status_code);
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
    HttpClientTransportOptions opts;
    EXPECT_TRUE(opts.enable_listen_stream);
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

TEST(StreamableHttpTransportTest, ListenStateForStatusCode) {
    using mcp::streamable_http_client_impl::ListenState;
    using mcp::streamable_http_client_impl::ListenStateForStatusCode;
    EXPECT_TRUE(ListenStateForStatusCode(405) == ListenState::Unsupported);
    EXPECT_TRUE(ListenStateForStatusCode(200) == ListenState::Streaming);
    EXPECT_TRUE(ListenStateForStatusCode(500) == ListenState::Connecting);
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
