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

static_assert(kMaxStdioFrameBytes >= kReadBufferSize,
    "a stdio frame must exceed one read buffer so frame accumulation stays meaningful");
static_assert(kMaxSseEventBytes <= kMaxMessageSize,
    "an SSE event carries one JSON-RPC message, so the event cap cannot exceed the message cap");
static_assert(kMaxHttpBodyBytes <= kMaxMessageSize,
    "an accepted HTTP body must fit the protocol message cap");
static_assert(kMaxDcrBytes <= kMaxHttpResponseBytes,
    "the DCR body cap must stay reachable below the HTTP response cap");
static_assert(kChannelCapacity > 0,
    "a blocking MessageChannel needs non-zero capacity");

}} // namespace mcp::detail
