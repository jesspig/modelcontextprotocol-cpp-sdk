#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
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

struct LogContext {
    std::string_view request_id;
    std::string_view method;
    std::string_view session_id;
    std::string_view peer;
};

inline void LogMessage(LogLevel level, const char* file, int line, const LogContext& ctx, const std::string& msg) {
    if (static_cast<int>(level) > static_cast<int>(GetLogLevel())) return;

    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    char time_buf[32] = {};
    std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", std::gmtime(&tt));

    std::string ctx_str;
    if (!ctx.request_id.empty())
        ctx_str += " [req=" + std::string(ctx.request_id) + "]";
    if (!ctx.method.empty())
        ctx_str += " [method=" + std::string(ctx.method) + "]";
    if (!ctx.session_id.empty())
        ctx_str += " [session=" + std::string(ctx.session_id) + "]";
    if (!ctx.peer.empty())
        ctx_str += " [peer=" + std::string(ctx.peer) + "]";

    std::fprintf(stderr, "[MCP][%s]%s %s:%d %s\n", time_buf,
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

#define MCP_BUG(msg)      MCP_LOG_TAG(Error, "BUG", msg)
