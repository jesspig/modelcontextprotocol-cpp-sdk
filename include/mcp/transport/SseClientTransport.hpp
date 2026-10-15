#pragma once

#include <mcp/Export.hpp>

#include <mcp/Transport.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <cstddef>
#include <optional>
#include <string>

namespace mcp {

struct SseClientTransportOptions {
    std::string server_url;
    std::string name{"sse"};
    std::optional<std::size_t> max_sse_event_bytes = detail::kMaxSseEventBytes;
    detail::HttpRedirectPolicy redirect_policy = detail::HttpRedirectPolicy::SameOriginOnly;
    int max_redirects = detail::kMaxHttpRedirects;
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
};

} // namespace mcp
