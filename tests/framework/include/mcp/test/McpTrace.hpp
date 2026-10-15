#pragma once
#include "McpApi.hpp"

#include <string>
#include <vector>

namespace mcp::test {

namespace detail {

inline std::vector<std::string>& TraceStack() {
    static thread_local std::vector<std::string> stack;
    return stack;
}

}  // namespace detail

class ScopedTrace {
public:
    ScopedTrace(const char* file, int line, std::string message) {
        detail::TraceStack().push_back(std::string(file) + ":" + std::to_string(line) + ": " + message);
    }

    ~ScopedTrace() {
        if (active_ && !detail::TraceStack().empty()) {
            detail::TraceStack().pop_back();
        }
    }

    ScopedTrace(const ScopedTrace&) = delete;
    ScopedTrace& operator=(const ScopedTrace&) = delete;
    ScopedTrace(ScopedTrace&&) = delete;
    ScopedTrace& operator=(ScopedTrace&&) = delete;

private:
    bool active_ = true;
};

inline std::string CurrentTraceChain() {
    const std::vector<std::string>& stack = detail::TraceStack();
    if (stack.empty()) {
        return {};
    }
    std::string chain;
    for (const std::string& entry : stack) {
        chain += "\n  Trace: " + entry;
    }
    return chain;
}

}  // namespace mcp::test

#define SCOPED_TRACE(expr) \
    ::mcp::test::ScopedTrace mcp_scoped_trace_##__COUNTER__(__FILE__, __LINE__, ::mcp::test::ToString(expr))
