#pragma once

#include <mcp/Transport.hpp>
#include <mcp/transport/HttpTransportMode.hpp>
#include <mcp/transport/detail/HttpHooks.hpp>
#include <mcp/transport/detail/Limits.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace mcp {

using detail::HttpRequestPurpose;
using detail::HttpRequestHeaderContext;
using detail::HttpResponseChallengeContext;

struct HttpClientTransportOptions {
    std::string endpoint;
    HttpTransportMode transport_mode = HttpTransportMode::AutoDetect;
    std::string name;
    std::string known_session_id;
    std::map<std::string, std::string> additional_headers;
    std::function<std::string(std::string_view www_authenticate)> auth_challenge_handler;
    std::function<void(const HttpRequestHeaderContext& context,
                       std::map<std::string, std::string>& headers)>
        apply_per_request_headers;
    std::function<void(const HttpResponseChallengeContext& context)>
        observe_response_challenge;
    std::optional<bool> enable_listen_stream;
    bool stateless = false;
    std::optional<std::size_t> max_sse_event_bytes = detail::kMaxSseEventBytes;
    detail::HttpRedirectPolicy redirect_policy = detail::HttpRedirectPolicy::SameOriginOnly;
    int max_redirects = detail::kMaxHttpRedirects;
};

class StreamableHttpClientTransport : public IClientTransport {
public:
    explicit StreamableHttpClientTransport(const HttpClientTransportOptions& options);
    ~StreamableHttpClientTransport() override;

    std::string_view Name() const override;
    std::shared_ptr<ITransport> Connect() override;

private:
    HttpClientTransportOptions options_;
    std::shared_ptr<ITransport> current_;
    std::mutex connect_mutex_;
};

} // namespace mcp
