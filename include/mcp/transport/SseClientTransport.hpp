#pragma once

#include <mcp/Export.hpp>

#include <mcp/Transport.hpp>
#include <mcp/transport/detail/HttpHooks.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace mcp {

struct SseClientTransportOptions {
    std::string server_url;
    std::string name{"sse"};
    std::optional<std::size_t> max_sse_event_bytes = detail::kMaxSseEventBytes;
    detail::HttpRedirectPolicy redirect_policy = detail::HttpRedirectPolicy::SameOriginOnly;
    int max_redirects = detail::kMaxHttpRedirects;
    std::map<std::string, std::string> additional_headers;
    std::function<std::string(std::string_view www_authenticate)> auth_challenge_handler;
    std::function<void(const detail::HttpRequestHeaderContext&,
                       std::map<std::string, std::string>&)>
        apply_per_request_headers;
    std::function<void(const detail::HttpResponseChallengeContext&)>
        observe_response_challenge;
    std::function<void(bool)> on_establish_result;
};

class MCP_API SseClientTransport : public IClientTransport {
public:
    explicit SseClientTransport(std::string_view server_url, std::string_view name = {});
    explicit SseClientTransport(SseClientTransportOptions options);
    ~SseClientTransport() override;

    std::string_view Name() const override;
    std::shared_ptr<ITransport> Connect() override;

private:
    SseClientTransportOptions options_;
    std::shared_ptr<detail::SessionLifecycle> lifecycle_ =
        std::make_shared<detail::SessionLifecycle>();
    std::shared_ptr<ITransport> current_;
    uint64_t current_generation_ = 0;
    std::mutex connect_mutex_;
};

} // namespace mcp
