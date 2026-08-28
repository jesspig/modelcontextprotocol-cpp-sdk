#pragma once

// Log.hpp — Logging utilities with level-based filtering

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace mcp {

enum class LogLevel : int {
    Off = 0,
    Error = 1,
    Warning = 2,
    Info = 3,
    Debug = 4,
    Trace = 5
};

struct LogContext {
    std::string request_id;
    std::string method;
    std::string session_id;
    std::string peer;
};

struct LogRecord {
    LogLevel level;
    std::string_view tag;        // 可空
    std::string_view message;
    const char* file;            // 无则 nullptr
    int line;                    // 无则 0
    const LogContext* context;   // 可空
};

using LogHandler = std::function<void(const LogRecord&)>;

namespace detail {

struct LogState {
    std::mutex mtx;
    std::function<void(const LogRecord&)> handler;
    std::atomic<bool> has_handler{false};
    std::atomic<LogLevel> level{LogLevel::Off};
};

inline LogState& GetLogState() {
    static LogState s;
    static std::once_flag flag;
    std::call_once(flag, [] {
        auto* env = std::getenv("MCP_LOG_LEVEL");
        LogLevel lvl = LogLevel::Off;
        if (env) {
            int v = std::atoi(env);
            if (v > 0) lvl = v >= 5 ? LogLevel::Trace : static_cast<LogLevel>(v);
        }
        s.level.store(lvl);
    });
    return s;
}

inline std::string FormatLogTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    char time_buf[32] = {};
    std::tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tm, &tt);
#endif
    std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm);
    return time_buf;
}

} // namespace detail

inline LogLevel GetLogLevel() {
    return detail::GetLogState().level.load();
}

// 设置自定义日志钩子；传 nullptr 恢复默认 stderr 行为。
// 钩子在发射日志的线程同步调用，须线程安全，禁止在其中调用 Close()。
inline void SetLogHandler(LogHandler handler) {
    auto& s = detail::GetLogState();
    std::lock_guard<std::mutex> g(s.mtx);
    s.handler = std::move(handler);
    s.has_handler.store(static_cast<bool>(s.handler));
}

// 获取当前钩子（无则空 callable）。
inline LogHandler GetLogHandler() {
    auto& s = detail::GetLogState();
    std::lock_guard<std::mutex> g(s.mtx);
    return s.handler;
}

// 运行时覆盖 MCP_LOG_LEVEL 环境变量设定的级别。
inline void SetLogLevel(LogLevel level) {
    detail::GetLogState().level.store(level);
}

inline void LogWrite(LogLevel level, std::string_view tag, std::string_view message) {
    if (static_cast<int>(level) > static_cast<int>(GetLogLevel())) return;

    LogRecord rec{level, tag, message, nullptr, 0, nullptr};

    if (detail::GetLogState().has_handler.load(std::memory_order_acquire)) {
        std::function<void(const LogRecord&)> local;
        {
            auto& s = detail::GetLogState();
            std::lock_guard<std::mutex> g(s.mtx);
            local = s.handler;
        }
        if (local) {
            try {
                local(rec);
            } catch (...) {
            }
        }
        return;
    }

    auto ts = detail::FormatLogTimestamp();

    if (!tag.empty())
        std::fprintf(stderr, "[MCP][%s][%s] %.*s\n", ts.c_str(), tag.data(),
            static_cast<int>(message.size()), message.data());
    else
        std::fprintf(stderr, "[MCP][%s] %.*s\n", ts.c_str(),
            static_cast<int>(message.size()), message.data());
}

inline void LogMessage(LogLevel level, const char* file, int line, const LogContext& ctx, const std::string& msg) {
    if (static_cast<int>(level) > static_cast<int>(GetLogLevel())) return;

    LogRecord rec{level, {}, msg, file, line, &ctx};

    if (detail::GetLogState().has_handler.load(std::memory_order_acquire)) {
        std::function<void(const LogRecord&)> local;
        {
            auto& s = detail::GetLogState();
            std::lock_guard<std::mutex> g(s.mtx);
            local = s.handler;
        }
        if (local) {
            try {
                local(rec);
            } catch (...) {
            }
        }
        return;
    }

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
