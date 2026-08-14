#include <mcp/transport/StreamableHttpClientTransport.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/Transport.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/test/McpTest.hpp>

using namespace mcp;

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
    EXPECT_FALSE(transport.IsStateless());
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
    EXPECT_FALSE(transport.IsStateless());
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
