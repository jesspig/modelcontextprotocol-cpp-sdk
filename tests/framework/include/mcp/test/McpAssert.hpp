#pragma once
// McpAssert.hpp — 断言宏（API 与 ToString 见 McpApi.hpp）

#include "McpApi.hpp"

#define MCP_EXPECT_COMPARE(op, a, b) \
    do { \
        if (!((a) op (b))) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, #a, #b, \
                ::mcp::test::ToString(a), ::mcp::test::ToString(b)); \
        } \
    } while (0)

#define MCP_ASSERT_COMPARE(op, a, b) \
    do { \
        if (!((a) op (b))) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, #a, #b, \
                ::mcp::test::ToString(a), ::mcp::test::ToString(b)); \
            return; \
        } \
    } while (0)

#define EXPECT_EQ(a, b) MCP_EXPECT_COMPARE(==, a, b)
#define EXPECT_NE(a, b) MCP_EXPECT_COMPARE(!=, a, b)
#define EXPECT_LT(a, b) MCP_EXPECT_COMPARE(<, a, b)
#define EXPECT_LE(a, b) MCP_EXPECT_COMPARE(<=, a, b)
#define EXPECT_GT(a, b) MCP_EXPECT_COMPARE(>, a, b)
#define EXPECT_GE(a, b) MCP_EXPECT_COMPARE(>=, a, b)
#define ASSERT_EQ(a, b) MCP_ASSERT_COMPARE(==, a, b)
#define ASSERT_NE(a, b) MCP_ASSERT_COMPARE(!=, a, b)
#define ASSERT_LT(a, b) MCP_ASSERT_COMPARE(<, a, b)
#define ASSERT_LE(a, b) MCP_ASSERT_COMPARE(<=, a, b)
#define ASSERT_GT(a, b) MCP_ASSERT_COMPARE(>, a, b)
#define ASSERT_GE(a, b) MCP_ASSERT_COMPARE(>=, a, b)

#define EXPECT_TRUE(expr) \
    do { if (!(expr)) ::mcp::test::AssertionFailure(__FILE__, __LINE__, #expr " evaluated to false"); } while (0)
#define EXPECT_FALSE(expr) \
    do { if (expr) ::mcp::test::AssertionFailure(__FILE__, __LINE__, #expr " evaluated to true"); } while (0)
#define ASSERT_TRUE(expr) \
    do { if (!(expr)) { ::mcp::test::AssertionFailure(__FILE__, __LINE__, #expr " evaluated to false"); return; } } while (0)
#define ASSERT_FALSE(expr) \
    do { if (expr) { ::mcp::test::AssertionFailure(__FILE__, __LINE__, #expr " evaluated to true"); return; } } while (0)

#define MCP_EXPECT_THROW(expr, type, stop) \
    do { \
        bool _mcp_thrown = false; \
        try { expr; } \
        catch (const type&) { _mcp_thrown = true; } \
        catch (...) {} \
        if (!_mcp_thrown) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, \
                std::string("expected exception ") + #type + " from: " + #expr); \
            stop; \
        } \
    } while (0)
#define EXPECT_THROW(expr, type) MCP_EXPECT_THROW(expr, type, (void)0)
#define ASSERT_THROW(expr, type) MCP_EXPECT_THROW(expr, type, return)

#define MCP_EXPECT_NO_THROW(expr, stop) \
    do { \
        try { expr; } \
        catch (const std::exception& _mcp_e) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, \
                std::string(#expr " threw: ") + _mcp_e.what()); \
            stop; \
        } catch (...) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, std::string(#expr " threw unknown exception")); \
            stop; \
        } \
    } while (0)
#define EXPECT_NO_THROW(expr) MCP_EXPECT_NO_THROW(expr, (void)0)
#define ASSERT_NO_THROW(expr) MCP_EXPECT_NO_THROW(expr, return)

#define EXPECT_STREQ(a, b) \
    do { \
        if (std::string_view((a) ? (a) : "") != std::string_view((b) ? (b) : "")) \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, #a, #b, \
                ::mcp::test::ToString((a) ? (a) : ""), ::mcp::test::ToString((b) ? (b) : "")); \
    } while (0)
#define ASSERT_STREQ(a, b) \
    do { \
        if (std::string_view((a) ? (a) : "") != std::string_view((b) ? (b) : "")) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, #a, #b, \
                ::mcp::test::ToString((a) ? (a) : ""), ::mcp::test::ToString((b) ? (b) : "")); \
            return; \
        } \
    } while (0)

#define EXPECT_DOUBLE_EQ(a, b) \
    do { \
        double _mcp_a = (a); \
        double _mcp_b = (b); \
        double _mcp_diff = _mcp_a - _mcp_b; \
        if (_mcp_diff < 0) _mcp_diff = -_mcp_diff; \
        double _mcp_scale = (_mcp_a < 0 ? -_mcp_a : _mcp_a) + (_mcp_b < 0 ? -_mcp_b : _mcp_b); \
        if (_mcp_scale < 1.0) _mcp_scale = 1.0; \
        if (_mcp_diff > 1e-12 * _mcp_scale) \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, #a, #b, \
                ::mcp::test::ToString(_mcp_a), ::mcp::test::ToString(_mcp_b)); \
    } while (0)
