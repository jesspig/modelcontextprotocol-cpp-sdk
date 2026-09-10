#include <mcp/transport/StreamableHttpClientTransport.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/Transport.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/http/HttpServer.hpp>
#include <mcp/test/McpTest.hpp>
#include "TestServerUtil.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace mcp;

// ── src/http/StreamableHttpClientTransport.cpp 内部实现（定义于 mcp-http 库，
//    经外部链接符号引用；类型声明须与实现保持逐字一致）──
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

// ── 回环：GET SSE 流推送的通知经 listen 流注入 channel ──
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

// ── 回环：GET 流被 405 拒绝后置 Unsupported，本会话不再重试 ──
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

// ── SEP-2243：initialize 请求不带 MCP-Protocol-Version 头 ──
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

// ── 回环：initialize 请求无协议头，响应学习后后续请求携带协商版本 ──
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
