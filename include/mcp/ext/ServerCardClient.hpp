#pragma once

#include <mcp/ext/ServerCard.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace mcp::ext {

inline constexpr std::size_t kDefaultServerCardResponseBytes = 1048576;
inline constexpr int kDefaultServerCardMaxRedirects = 5;

struct ServerCardClientOptions {
    std::optional<std::string> if_none_match;
    std::optional<ServerCard> cached_card;
    std::optional<std::size_t> max_response_bytes;
    std::chrono::milliseconds timeout{30000};
    int max_redirects{kDefaultServerCardMaxRedirects};
};

struct ServerCardFetchResult {
    ServerCard card;
    std::string etag;
    bool from_cache{false};
};

[[nodiscard]] std::string BuildServerCardDiscoveryUrl(std::string_view endpoint_url);

[[nodiscard]] bool IsServerCardJsonMediaType(std::string_view content_type);

[[nodiscard]] bool ServerCardEtagMatches(std::string_view if_none_match,
                                         std::string_view etag);

[[nodiscard]] std::string CanonicalizeServerCardBody(std::string_view body);

[[nodiscard]] std::string ComputeServerCardEtag(std::string_view body);

[[nodiscard]] ServerCard ParseServerCard(std::string_view json);

[[nodiscard]] ServerCardFetchResult FetchServerCard(
    std::string_view endpoint_url, const ServerCardClientOptions& options = {});

} // namespace mcp::ext
