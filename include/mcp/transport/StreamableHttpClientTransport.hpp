#pragma once
// StreamableHttpClientTransport.hpp — HTTP client transport (Streamable HTTP / SSE)

#include <mcp/Transport.hpp>
#include <mcp/transport/HttpTransportMode.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace mcp {

struct HttpClientTransportOptions {
    std::string endpoint;
    HttpTransportMode transport_mode = HttpTransportMode::AutoDetect;
    std::string name;
    std::string known_session_id;
    std::map<std::string, std::string> additional_headers;
    // Optional auth challenge hook (RFC 9728): called with the server's
    // WWW-Authenticate header when a request fails with 401/403. It should
    // (re)authenticate — e.g. step-up via OAuthClientProvider — and return an
    // Authorization header value (e.g. "Bearer <token>"), or an empty string
    // to give up. A non-empty return triggers exactly one retry with the
    // returned header attached.
    std::function<std::string(std::string_view www_authenticate)> auth_challenge_handler;
};

class StreamableHttpClientTransport : public IClientTransport {
public:
    explicit StreamableHttpClientTransport(const HttpClientTransportOptions& options);
    ~StreamableHttpClientTransport() override;

    std::string_view Name() const override;
    std::shared_ptr<ITransport> Connect() override;

private:
    HttpClientTransportOptions options_;
};

} // namespace mcp
