#include <mcp/ext/ServerCardClient.hpp>
#include <mcp/McpError.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>

#include <transport/detail/net/HttpClient.hpp>

#include <gtest/gtest.h>
#include "TestServerUtil.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using NetResp = mcp::detail::net::HttpResponseInfo;

NetResp SendRequest(const std::string& method, const std::string& url,
                    const std::unordered_map<std::string, std::string>& headers = {}) {
    mcp::detail::net::HttpClient client;
    mcp::detail::net::HttpRequestSpec req;
    req.method = method;
    req.url = url;
    req.headers = headers;
    req.timeout = std::chrono::milliseconds(5000);
    return client.Request(req);
}

std::string LowerCopy(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return text;
}

std::size_t CountHeader(const std::string& raw, std::string_view name) {
    const auto text = LowerCopy(raw);
    const auto needle = LowerCopy(std::string(name) + ":");
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

struct RawClient {
    RawClient() {
#ifdef _WIN32
        WSADATA wsa;
        valid = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        sock = socket(AF_INET, SOCK_STREAM, 0);
        valid = valid && sock != INVALID_SOCKET;
#else
        sock = socket(AF_INET, SOCK_STREAM, 0);
        valid = sock >= 0;
#endif
    }
    ~RawClient() {
#ifdef _WIN32
        if (sock != INVALID_SOCKET) closesocket(sock);
        if (valid) WSACleanup();
#else
        if (sock >= 0) close(sock);
#endif
    }
    RawClient(const RawClient&) = delete;
    RawClient& operator=(const RawClient&) = delete;

    bool Connect(uint16_t port) {
        if (!valid) return false;
#ifdef _WIN32
        DWORD timeout = 3000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout{};
        timeout.tv_sec = 3;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        return connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    std::string Request(const std::string& request) {
        if (!valid) return {};
        if (send(sock, request.data(), static_cast<int>(request.size()), 0) !=
            static_cast<int>(request.size()))
            return {};
        std::string result;
        char chunk[8192];
        while (true) {
            int n = recv(sock, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            result.append(chunk, static_cast<std::size_t>(n));
        }
        return result;
    }

    bool valid{false};
#ifdef _WIN32
    SOCKET sock{INVALID_SOCKET};
#else
    int sock{-1};
#endif
};

mcp::ext::ServerCard MakeFullCard() {
    mcp::ext::ServerCard card;
    card.name = "card-client-test";
    card.version = "1.2.3";
    card.description = "full card";
    card.title = "Full Card";
    card.repository = "https://example.com/org/repo";
    card.website_url = "https://example.com";
    mcp::Icon icon;
    icon.src = "https://example.com/icon.png";
    icon.mime_type = "image/png";
    icon.sizes = {"48x48", "any"};
    icon.theme = "light";
    card.icons.push_back(icon);
    mcp::ext::ServerCardEndpoint endpoint;
    endpoint.url = "https://example.com/mcp";
    endpoint.headers["x-tenant"] = "alpha";
    endpoint.variables["region"] = "west";
    card.endpoints.push_back(endpoint);
    card.supported_protocol_versions = {"2025-06-18", "2026-07-28"};
    mcp::JsonValue::Object meta;
    meta["source"] = mcp::JsonValue("unit-test");
    card.meta = mcp::JsonValue(std::move(meta));
    return card;
}

mcp::JsonValue MakeFullCardJson() {
    return mcp::ext::SerializeCard(MakeFullCard());
}

struct CardServer {
    uint16_t port{0};
    std::unique_ptr<mcp::StreamableHttpServerTransport> transport;
    bool ready{false};

    explicit CardServer(mcp::ext::ServerCard card = MakeFullCard()) {
        port = PickFreePort(kTestBasePort + 700);
        mcp::StreamableHttpServerOptions options;
        options.port = port;
        options.stateless = true;
        options.enable_server_card = true;
        options.server_card = std::move(card);
        transport = std::make_unique<mcp::StreamableHttpServerTransport>(options);
        transport->Start();
        ready = WaitUntilReady(port);
    }
    ~CardServer() { transport->Close(); }
};

} // namespace

TEST(ServerCardClientTest, DiscoveryUrlFromEndpoint) {
    using mcp::ext::BuildServerCardDiscoveryUrl;
    EXPECT_EQ(BuildServerCardDiscoveryUrl("http://localhost:3001/mcp"),
              "http://localhost:3001/mcp/server-card");
    EXPECT_EQ(BuildServerCardDiscoveryUrl("http://127.0.0.1:3001/mcp?x=1#frag"),
              "http://127.0.0.1:3001/mcp/server-card");
    EXPECT_EQ(BuildServerCardDiscoveryUrl("https://example.com/a/b/"),
              "https://example.com/a/b/server-card");
    EXPECT_EQ(BuildServerCardDiscoveryUrl("https://example.com"),
              "https://example.com/server-card");
    EXPECT_EQ(BuildServerCardDiscoveryUrl("http://[::1]:3001/mcp"),
              "http://[::1]:3001/mcp/server-card");
}

TEST(ServerCardClientTest, DiscoveryUrlRejectsMalformed) {
    using mcp::ext::BuildServerCardDiscoveryUrl;
    EXPECT_THROW((void)BuildServerCardDiscoveryUrl("http://user@localhost/mcp"), mcp::McpError);
    EXPECT_THROW((void)BuildServerCardDiscoveryUrl("ftp://localhost/mcp"), mcp::McpError);
    EXPECT_THROW((void)BuildServerCardDiscoveryUrl("localhost/mcp"), mcp::McpError);
    EXPECT_THROW((void)BuildServerCardDiscoveryUrl("http:///mcp"), mcp::McpError);
    EXPECT_THROW((void)BuildServerCardDiscoveryUrl("http://localhost:abc/mcp"), mcp::McpError);
    EXPECT_THROW((void)BuildServerCardDiscoveryUrl(""), mcp::McpError);
}

TEST(ServerCardClientTest, MediaTypeAcceptsJsonVariants) {
    using mcp::ext::IsServerCardJsonMediaType;
    EXPECT_TRUE(IsServerCardJsonMediaType("application/json"));
    EXPECT_TRUE(IsServerCardJsonMediaType("Application/JSON; charset=utf-8"));
    EXPECT_TRUE(IsServerCardJsonMediaType("application/json;charset=UTF-8"));
    EXPECT_TRUE(IsServerCardJsonMediaType("application/vnd.example+json"));
}

TEST(ServerCardClientTest, MediaTypeRejectsNonJson) {
    using mcp::ext::IsServerCardJsonMediaType;
    EXPECT_FALSE(IsServerCardJsonMediaType("text/html"));
    EXPECT_FALSE(IsServerCardJsonMediaType("text/plain"));
    EXPECT_FALSE(IsServerCardJsonMediaType("application/xml"));
    EXPECT_FALSE(IsServerCardJsonMediaType(""));
    EXPECT_FALSE(IsServerCardJsonMediaType("application/jsonx"));
}

TEST(ServerCardClientTest, EtagWeakListComparison) {
    using mcp::ext::ServerCardEtagMatches;
    EXPECT_TRUE(ServerCardEtagMatches("\"abc\"", "\"abc\""));
    EXPECT_TRUE(ServerCardEtagMatches("W/\"abc\"", "\"abc\""));
    EXPECT_TRUE(ServerCardEtagMatches("\"other\", W/\"abc\"", "\"abc\""));
    EXPECT_TRUE(ServerCardEtagMatches("*", "\"abc\""));
    EXPECT_FALSE(ServerCardEtagMatches("\"other\"", "\"abc\""));
    EXPECT_FALSE(ServerCardEtagMatches("", "\"abc\""));
}

TEST(ServerCardClientTest, FullFieldsRoundTrip) {
    const auto card = MakeFullCard();
    const std::string body = mcp::ext::SerializeCard(card).Dump();
    const auto parsed = mcp::ext::ParseServerCard(body);
    EXPECT_EQ(parsed.name, card.name);
    EXPECT_EQ(parsed.version, card.version);
    EXPECT_EQ(parsed.description, card.description);
    EXPECT_EQ(parsed.title, card.title);
    EXPECT_EQ(parsed.repository, card.repository);
    EXPECT_EQ(parsed.website_url, card.website_url);
    ASSERT_EQ(parsed.icons.size(), 1u);
    EXPECT_EQ(parsed.icons[0].src, card.icons[0].src);
    EXPECT_EQ(parsed.icons[0].mime_type, card.icons[0].mime_type);
    EXPECT_EQ(parsed.icons[0].sizes, card.icons[0].sizes);
    EXPECT_EQ(parsed.icons[0].theme, card.icons[0].theme);
    ASSERT_EQ(parsed.endpoints.size(), 1u);
    EXPECT_EQ(parsed.endpoints[0].url, card.endpoints[0].url);
    EXPECT_EQ(parsed.endpoints[0].headers, card.endpoints[0].headers);
    EXPECT_EQ(parsed.endpoints[0].variables, card.endpoints[0].variables);
    EXPECT_EQ(parsed.supported_protocol_versions, card.supported_protocol_versions);
    ASSERT_TRUE(parsed.meta.has_value());
    EXPECT_EQ(*parsed.meta, *card.meta);
    EXPECT_EQ(mcp::ext::SerializeCard(parsed).Dump(), body);
}

TEST(ServerCardClientTest, ParseRejectsInvalidFields) {
    {
        auto json = MakeFullCardJson();
        json["name"] = mcp::JsonValue("");
        EXPECT_THROW((void)mcp::ext::ParseServerCard(json.Dump()), mcp::McpError);
    }
    {
        auto json = MakeFullCardJson();
        json["version"] = mcp::JsonValue("");
        EXPECT_THROW((void)mcp::ext::ParseServerCard(json.Dump()), mcp::McpError);
    }
    {
        auto json = MakeFullCardJson();
        json["endpoints"] = mcp::JsonValue(mcp::JsonValue::Array{});
        EXPECT_THROW((void)mcp::ext::ParseServerCard(json.Dump()), mcp::McpError);
    }
    {
        auto json = MakeFullCardJson();
        json["endpoints"] = mcp::JsonValue("not-an-array");
        EXPECT_THROW((void)mcp::ext::ParseServerCard(json.Dump()), mcp::McpError);
    }
    {
        auto json = MakeFullCardJson();
        json["endpoints"][static_cast<std::size_t>(0)]["url"] =
            mcp::JsonValue("ftp://example.com/mcp");
        EXPECT_THROW((void)mcp::ext::ParseServerCard(json.Dump()), mcp::McpError);
    }
    {
        auto json = MakeFullCardJson();
        json["endpoints"][static_cast<std::size_t>(0)]["url"] =
            mcp::JsonValue("https://user@example.com/mcp");
        EXPECT_THROW((void)mcp::ext::ParseServerCard(json.Dump()), mcp::McpError);
    }
    {
        auto json = MakeFullCardJson();
        json["endpoints"][static_cast<std::size_t>(0)]["headers"]["x-bad"] =
            mcp::JsonValue("bad\r\nvalue");
        EXPECT_THROW((void)mcp::ext::ParseServerCard(json.Dump()), mcp::McpError);
    }
    {
        auto json = MakeFullCardJson();
        json["endpoints"][static_cast<std::size_t>(0)]["variables"]["bad key"] =
            mcp::JsonValue("value");
        EXPECT_THROW((void)mcp::ext::ParseServerCard(json.Dump()), mcp::McpError);
    }
    {
        auto json = MakeFullCardJson();
        json["icons"][static_cast<std::size_t>(0)]["theme"] = mcp::JsonValue("neon");
        EXPECT_THROW((void)mcp::ext::ParseServerCard(json.Dump()), mcp::McpError);
    }
    {
        auto json = MakeFullCardJson();
        json["icons"][static_cast<std::size_t>(0)]["sizes"] =
            mcp::JsonValue(mcp::JsonValue::Array{mcp::JsonValue("huge")});
        EXPECT_THROW((void)mcp::ext::ParseServerCard(json.Dump()), mcp::McpError);
    }
    {
        auto json = MakeFullCardJson();
        json["website_url"] = mcp::JsonValue("not-a-url");
        EXPECT_THROW((void)mcp::ext::ParseServerCard(json.Dump()), mcp::McpError);
    }
    {
        auto json = MakeFullCardJson();
        json["supportedProtocolVersions"] =
            mcp::JsonValue(mcp::JsonValue::Array{mcp::JsonValue("tomorrow")});
        EXPECT_THROW((void)mcp::ext::ParseServerCard(json.Dump()), mcp::McpError);
    }
    EXPECT_THROW((void)mcp::ext::ParseServerCard("not json"), mcp::McpError);
    EXPECT_THROW((void)mcp::ext::ParseServerCard("[]"), mcp::McpError);
}

TEST(ServerCardClientTest, ServerCardRouteServesCanonicalJsonAndStrongEtag) {
    CardServer server;
    ASSERT_TRUE(server.ready);
    const auto base = "http://127.0.0.1:" + std::to_string(server.port);
    const auto expected = mcp::ext::SerializeCard(MakeFullCard()).Dump();
    for (const std::string& path : {"/server-card", "/mcp/server-card"}) {
        auto response = SendRequest("GET", base + path);
        EXPECT_EQ(response.status_code, 200);
        EXPECT_EQ(response.body, expected);
        EXPECT_EQ(response.headers.at("content-type"), "application/json");
        ASSERT_TRUE(response.headers.find("etag") != response.headers.end());
        const auto& etag = response.headers.at("etag");
        EXPECT_EQ(etag.size(), 66u);
        EXPECT_EQ(etag.front(), '"');
        EXPECT_EQ(etag.back(), '"');
        EXPECT_EQ(etag, mcp::ext::ComputeServerCardEtag(expected));
        EXPECT_EQ(response.headers.at("cache-control"), "public,max-age=60");
    }
}

TEST(ServerCardClientTest, ServerCardRouteConditionalRequest) {
    CardServer server;
    ASSERT_TRUE(server.ready);
    const auto url = "http://127.0.0.1:" + std::to_string(server.port) + "/mcp/server-card";
    auto first = SendRequest("GET", url);
    ASSERT_EQ(first.status_code, 200);
    const auto etag = first.headers.at("etag");

    auto weak = SendRequest("GET", url, {{"if-none-match", "W/" + etag}});
    EXPECT_EQ(weak.status_code, 304);
    EXPECT_TRUE(weak.body.empty());
    EXPECT_EQ(weak.headers.at("etag"), etag);
    EXPECT_EQ(weak.headers.at("cache-control"), "public,max-age=60");
    EXPECT_EQ(weak.headers.at("access-control-allow-origin"), "*");

    auto list = SendRequest("GET", url, {{"if-none-match", "\"other\", " + etag}});
    EXPECT_EQ(list.status_code, 304);
    EXPECT_TRUE(list.body.empty());

    auto miss = SendRequest("GET", url, {{"if-none-match", "\"stale\""}});
    EXPECT_EQ(miss.status_code, 200);
    EXPECT_EQ(miss.body, first.body);
    EXPECT_EQ(miss.headers.at("etag"), etag);
}

TEST(ServerCardClientTest, ServerCardRouteCorsAndOriginAdmission) {
    CardServer server;
    ASSERT_TRUE(server.ready);
    const auto port = server.port;
    const auto origin = "http://127.0.0.1:" + std::to_string(port);

    RawClient get_client;
    ASSERT_TRUE(get_client.Connect(port));
    const auto raw = get_client.Request(
        "GET /server-card HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
        "\r\nOrigin: " + origin + "\r\nConnection: close\r\n\r\n");
    EXPECT_EQ(CountHeader(raw, "access-control-allow-origin"), 1u);
    EXPECT_EQ(CountHeader(raw, "access-control-expose-headers"), 1u);
    EXPECT_NE(LowerCopy(raw).find("etag"), std::string::npos);
    EXPECT_NE(LowerCopy(raw).find("etag\r\n"), std::string::npos);

    RawClient preflight_client;
    ASSERT_TRUE(preflight_client.Connect(port));
    const auto preflight = preflight_client.Request(
        "OPTIONS /server-card HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
        "\r\nOrigin: " + origin + "\r\nAccess-Control-Request-Method: GET\r\n"
        "Connection: close\r\n\r\n");
    EXPECT_NE(preflight.find("204"), std::string::npos);
    EXPECT_EQ(CountHeader(preflight, "access-control-allow-origin"), 1u);
    EXPECT_NE(LowerCopy(preflight).find("etag"), std::string::npos);

    auto foreign = SendRequest("GET", "http://127.0.0.1:" + std::to_string(port) + "/server-card",
                               {{"origin", "http://evil.example"}});
    EXPECT_EQ(foreign.status_code, 403);

    RawClient no_host;
    ASSERT_TRUE(no_host.Connect(port));
    const auto rejected = no_host.Request(
        "GET /server-card HTTP/1.1\r\nOrigin: " + origin + "\r\nConnection: close\r\n\r\n");
    EXPECT_NE(rejected.find("403"), std::string::npos);
}

TEST(ServerCardClientTest, FetchServerCardRoundTripAndConditional) {
    CardServer server;
    ASSERT_TRUE(server.ready);
    const auto endpoint = "http://127.0.0.1:" + std::to_string(server.port) +
                          "/mcp?session=ignored#fragment";
    const auto fetched = mcp::ext::FetchServerCard(endpoint);
    EXPECT_EQ(fetched.card.name, "card-client-test");
    EXPECT_FALSE(fetched.etag.empty());
    EXPECT_FALSE(fetched.from_cache);

    mcp::ext::ServerCardClientOptions options;
    options.if_none_match = fetched.etag;
    options.cached_card = fetched.card;
    const auto cached = mcp::ext::FetchServerCard(endpoint, options);
    EXPECT_TRUE(cached.from_cache);
    EXPECT_EQ(cached.etag, fetched.etag);
    EXPECT_EQ(cached.card.name, fetched.card.name);

    options.cached_card.reset();
    EXPECT_THROW((void)mcp::ext::FetchServerCard(endpoint, options), mcp::McpError);
}

TEST(ServerCardClientTest, FetchServerCardRejectsNonJsonAndEmptyBody) {
    {
        auto port = PickFreePort(kTestBasePort + 800);
        mcp::HttpServer server(port);
        server.SetHandler("GET", "/mcp/server-card", [](const mcp::HttpRequest&,
                                                         mcp::HttpResponse& resp) {
            resp.headers["content-type"] = "text/html";
            resp.body = "<html></html>";
        });
        server.Start();
        ASSERT_TRUE(WaitUntilReady(port));
        EXPECT_THROW((void)mcp::ext::FetchServerCard(
            "http://127.0.0.1:" + std::to_string(port) + "/mcp"), mcp::McpError);
        server.Stop();
    }
    {
        auto port = PickFreePort(kTestBasePort + 810);
        mcp::HttpServer server(port);
        server.SetHandler("GET", "/mcp/server-card", [](const mcp::HttpRequest&,
                                                         mcp::HttpResponse& resp) {
            resp.headers["content-type"] = "application/json";
            resp.body.clear();
        });
        server.Start();
        ASSERT_TRUE(WaitUntilReady(port));
        EXPECT_THROW((void)mcp::ext::FetchServerCard(
            "http://127.0.0.1:" + std::to_string(port) + "/mcp"), mcp::McpError);
        server.Stop();
    }
    {
        auto port = PickFreePort(kTestBasePort + 820);
        mcp::HttpServer server(port);
        server.SetHandler("GET", "/mcp/server-card", [](const mcp::HttpRequest&,
                                                         mcp::HttpResponse& resp) {
            resp.headers["content-type"] = "Application/JSON; charset=UTF-8";
            resp.body = mcp::ext::SerializeCard(MakeFullCard()).Dump();
        });
        server.Start();
        ASSERT_TRUE(WaitUntilReady(port));
        auto result = mcp::ext::FetchServerCard(
            "http://127.0.0.1:" + std::to_string(port) + "/mcp");
        EXPECT_EQ(result.card.name, "card-client-test");
        server.Stop();
    }
    {
        auto port = PickFreePort(kTestBasePort + 830);
        mcp::HttpServer server(port);
        server.SetHandler("GET", "/mcp/server-card", [](const mcp::HttpRequest&,
                                                         mcp::HttpResponse& resp) {
            resp.headers["content-type"] = "application/json";
            resp.body = std::string(2048, 'x');
        });
        server.Start();
        ASSERT_TRUE(WaitUntilReady(port));
        mcp::ext::ServerCardClientOptions options;
        options.max_response_bytes = 128;
        EXPECT_THROW((void)mcp::ext::FetchServerCard(
            "http://127.0.0.1:" + std::to_string(port) + "/mcp", options), mcp::McpError);
        server.Stop();
    }
}
