#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace mcp {

enum class SpanKind : std::uint8_t {
    ServerRequest,
    TransportReceive,
    TransportSend,
};

enum class SpanPhase : std::uint8_t {
    Begin,
    End,
};

struct SpanEvent {
    SpanPhase phase = SpanPhase::Begin;
    SpanKind kind = SpanKind::ServerRequest;
    std::string name;
    std::string method;
    std::string request_id;
    std::string traceparent;
    std::string tracestate;
    std::string baggage;
    bool failed = false;
};

using SpanHandler = std::function<void(const SpanEvent&)>;

} // namespace mcp
