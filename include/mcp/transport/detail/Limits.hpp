#pragma once

#include <cstddef>
#include <cstdint>

namespace mcp { namespace detail {

enum class HttpRedirectPolicy {
    SameOriginOnly,
    FollowAll,
    NoFollow
};

inline constexpr size_t kMaxMessageSize = 8 * 1024 * 1024;
inline constexpr size_t kMaxHttpBodyBytes = 4 * 1024 * 1024;
inline constexpr size_t kMaxHttpResponseBytes = 8 * 1024 * 1024;
inline constexpr size_t kMaxSseEventBytes = 1 * 1024 * 1024;
inline constexpr size_t kMaxDcrBytes = 1 * 1024 * 1024;
inline constexpr size_t kMaxStdioFrameBytes = 8 * 1024 * 1024;
inline constexpr int kMaxHttpRedirects = 5;
inline constexpr size_t kReadBufferSize = 4096;
inline constexpr size_t kChannelCapacity = 64;
inline constexpr size_t kMaxHttpSessions = 10000;
inline constexpr int64_t kSessionIdleTimeoutMs = 30LL * 60 * 1000;

}} // namespace mcp::detail
