#pragma once

#include <mcp/Transport.hpp>
#include <mcp/transport/SseClientTransport.hpp>

#include <cstdint>
#include <memory>

namespace mcp { namespace sse_client_impl {

std::shared_ptr<ITransport> CreateSession(
    SseClientTransportOptions options,
    std::shared_ptr<detail::SessionLifecycle> lifecycle,
    uint64_t generation,
    std::shared_ptr<MessageChannel> channel);

}} // namespace mcp::sse_client_impl
