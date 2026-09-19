#pragma once

// SpanHooks.hpp — span observation hooks (无 OpenTelemetry 依赖)

#include <cstdint>
#include <functional>
#include <string>

namespace mcp {

// 钩子覆盖的边界类别。
enum class SpanKind : std::uint8_t {
    ServerRequest,
    TransportReceive,
    TransportSend,
};

// Begin 与 End 成对出现，按 (kind, request_id) 配对。
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

// 跨度钩子。在发射事件的线程同步调用，须线程安全；
// 不得抛出异常，也不得在其中调用 Close()。
using SpanHandler = std::function<void(const SpanEvent&)>;

} // namespace mcp
