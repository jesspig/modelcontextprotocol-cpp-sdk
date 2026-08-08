#pragma once
// Limits.hpp — transport safety limits

#include <cstddef>

namespace mcp { namespace detail {

inline constexpr size_t kMaxMessageSize = 8 * 1024 * 1024;  // 8MB
inline constexpr size_t kReadBufferSize = 4096;
inline constexpr size_t kChannelCapacity = 64;

}} // namespace mcp::detail
