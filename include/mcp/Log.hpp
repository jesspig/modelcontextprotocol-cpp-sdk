#pragma once

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <chrono>

namespace mcp {

enum class LogLevel : int {
    Off = 0,
    Error = 1,
    Warning = 2,
    Info = 3,
    Debug = 4,
    Trace = 5
};

inline LogLevel GetLogLevel() {
    static const LogLevel level = []() {
        auto* env = std::getenv("MCP_LOG_LEVEL");
        if (!env) return LogLevel::Off;
        int val = std::atoi(env);
        if (val <= 0) return LogLevel::Off;
        if (val >= 5) return LogLevel::Trace;
        return static_cast<LogLevel>(val);
    }();
    return level;
}

inline void LogWrite(LogLevel level, std::string_view tag, std::string_view message) {
    if (static_cast<int>(level) > static_cast<int>(GetLogLevel())) return;

    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    char time_buf[32] = {};
    std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", std::gmtime(&tt));

    if (!tag.empty())
        std::fprintf(stderr, "[MCP][%s][%s] %.*s\n", time_buf, tag.data(),
            static_cast<int>(message.size()), message.data());
    else
        std::fprintf(stderr, "[MCP][%s] %.*s\n", time_buf,
            static_cast<int>(message.size()), message.data());
}

} // namespace mcp

#define MCP_LOG(level, msg) \
    do { \
        if (static_cast<int>(level) <= static_cast<int>(::mcp::GetLogLevel())) \
            ::mcp::LogWrite(level, {}, msg); \
    } while(0)

#define MCP_LOG_TAG(level, tag, msg) \
    do { \
        if (static_cast<int>(level) <= static_cast<int>(::mcp::GetLogLevel())) \
            ::mcp::LogWrite(level, tag, msg); \
    } while(0)

#define MCP_P0(msg)       MCP_LOG_TAG(::mcp::LogLevel::Error, "P0", msg)
#define MCP_BUG(msg)      MCP_LOG_TAG(::mcp::LogLevel::Error, "BUG", msg)
#define MCP_NEEDS_REPRO(msg) MCP_LOG_TAG(::mcp::LogLevel::Warning, "NEEDS_REPRO", msg)
