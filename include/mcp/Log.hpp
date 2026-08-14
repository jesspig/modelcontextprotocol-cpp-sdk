#pragma once

// Log.hpp — Logging utilities with level-based filtering

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <chrono>
#include <ctime>

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

namespace detail {

inline std::string FormatLogTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    char time_buf[32] = {};
    std::tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm);
    return time_buf;
}

} // namespace detail

inline void LogWrite(LogLevel level, std::string_view tag, std::string_view message) {
    if (static_cast<int>(level) > static_cast<int>(GetLogLevel())) return;

    auto ts = detail::FormatLogTimestamp();

    if (!tag.empty())
        std::fprintf(stderr, "[MCP][%s][%s] %.*s\n", ts.c_str(), tag.data(),
            static_cast<int>(message.size()), message.data());
    else
        std::fprintf(stderr, "[MCP][%s] %.*s\n", ts.c_str(),
            static_cast<int>(message.size()), message.data());
}

struct LogContext {
    std::string request_id;
    std::string method;
    std::string session_id;
    std::string peer;
};

inline void LogMessage(LogLevel level, const char* file, int line, const LogContext& ctx, const std::string& msg) {
    if (static_cast<int>(level) > static_cast<int>(GetLogLevel())) return;

    std::string ctx_str;
    if (!ctx.request_id.empty())
        ctx_str += " [req=" + ctx.request_id + "]";
    if (!ctx.method.empty())
        ctx_str += " [method=" + ctx.method + "]";
    if (!ctx.session_id.empty())
        ctx_str += " [session=" + ctx.session_id + "]";
    if (!ctx.peer.empty())
        ctx_str += " [peer=" + ctx.peer + "]";

    auto ts = detail::FormatLogTimestamp();
    std::fprintf(stderr, "[MCP][%s]%s %s:%d %s\n", ts.c_str(),
        ctx_str.c_str(), file, line, msg.c_str());
}

} // namespace mcp

#define MCP_LOG(LEVEL, msg) \
    do { \
        constexpr auto _mcp_lvl_ = ::mcp::LogLevel::LEVEL; \
        if (static_cast<int>(_mcp_lvl_) <= static_cast<int>(::mcp::GetLogLevel())) \
            ::mcp::LogWrite(_mcp_lvl_, {}, msg); \
    } while(0)

#define MCP_LOG_TAG(LEVEL, tag, msg) \
    do { \
        constexpr auto _mcp_lvl_ = ::mcp::LogLevel::LEVEL; \
        if (static_cast<int>(_mcp_lvl_) <= static_cast<int>(::mcp::GetLogLevel())) \
            ::mcp::LogWrite(_mcp_lvl_, tag, msg); \
    } while(0)

#define MCP_LOG_CTX(LEVEL, ctx, ...) \
    do { \
        if (::mcp::LogLevel::LEVEL <= ::mcp::GetLogLevel()) { \
            ::mcp::LogMessage(::mcp::LogLevel::LEVEL, __FILE__, __LINE__, ctx, __VA_ARGS__); \
        } \
    } while(0)
