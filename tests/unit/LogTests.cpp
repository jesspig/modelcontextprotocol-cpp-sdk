// LogTests.cpp — 日志钩子单元测试
#include <mcp/Log.hpp>
#include <mcp/test/McpTest.hpp>

#include <string>
#include <vector>

namespace {

struct Captured {
    mcp::LogLevel level;
    std::string message;
    std::string tag;
    bool has_context;
};

} // namespace

TEST(LogTest, HandlerCapturesRecord) {
    std::vector<Captured> out;
    mcp::SetLogLevel(mcp::LogLevel::Debug);
    mcp::SetLogHandler([&out](const mcp::LogRecord& r) {
        Captured c;
        c.level = r.level;
        c.message = std::string(r.message);
        c.tag = std::string(r.tag);
        c.has_context = (r.context != nullptr);
        out.push_back(std::move(c));
    });
    MCP_LOG(Debug, "hello-world");
    mcp::SetLogHandler(nullptr);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].level, mcp::LogLevel::Debug);
    EXPECT_EQ(out[0].message, "hello-world");
}

TEST(LogTest, LevelFilterBelowNotTriggered) {
    std::vector<Captured> out;
    mcp::SetLogLevel(mcp::LogLevel::Warning); // 仅 Warning 及以上
    mcp::SetLogHandler([&out](const mcp::LogRecord& r) {
        Captured c; c.level = r.level; c.message = std::string(r.message);
        out.push_back(std::move(c));
    });
    MCP_LOG(Info, "should-be-filtered"); // Info < Warning，不应触发
    MCP_LOG(Error, "should-fire");
    mcp::SetLogHandler(nullptr);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].level, mcp::LogLevel::Error);
}

TEST(LogTest, HandlerThrowsDoesNotCrash) {
    mcp::SetLogLevel(mcp::LogLevel::Debug);
    mcp::SetLogHandler([](const mcp::LogRecord&) {
        throw std::runtime_error("boom");
    });
    // 钩子抛异常必须被 SDK 吞掉，不应崩溃/传播
    MCP_LOG(Debug, "msg-after-handler-throw");
    mcp::SetLogHandler(nullptr);
    EXPECT_TRUE(true);
}

TEST(LogTest, NullHandlerRestoresDefaultNoCrash) {
    mcp::SetLogLevel(mcp::LogLevel::Debug);
    mcp::SetLogHandler(nullptr); // 恢复默认 stderr，不应崩溃
    MCP_LOG(Debug, "default-path");
    EXPECT_TRUE(true);
}
