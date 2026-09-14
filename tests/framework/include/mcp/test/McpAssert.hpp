#pragma once
// McpAssert.hpp — 断言宏（API 与 ToString 见 McpApi.hpp）

#include "McpApi.hpp"

#include <cctype>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iterator>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

// ── 内部辅助 ──
namespace mcp::test {
namespace detail {

inline std::string CaughtExceptionMessage() {
    try {
        throw;
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
    }
    return "unknown exception";
}

inline bool AsciiCaseEqual(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (std::tolower(static_cast<unsigned char>(*a)) !=
            std::tolower(static_cast<unsigned char>(*b))) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == *b;
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4018)
#pragma warning(disable : 4389)
#endif

struct CmpEq { template <typename A, typename B> bool operator()(const A& a, const B& b) const { return a == b; } };
struct CmpNe { template <typename A, typename B> bool operator()(const A& a, const B& b) const { return a != b; } };
struct CmpLt { template <typename A, typename B> bool operator()(const A& a, const B& b) const { return a < b; } };
struct CmpLe { template <typename A, typename B> bool operator()(const A& a, const B& b) const { return a <= b; } };
struct CmpGt { template <typename A, typename B> bool operator()(const A& a, const B& b) const { return a > b; } };
struct CmpGe { template <typename A, typename B> bool operator()(const A& a, const B& b) const { return a >= b; } };

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

template <typename Cmp, typename A, typename B>
inline bool CompareOperands(const char* file, int line, const char* expr_a, const char* expr_b,
                            Cmp cmp, const A& a, const B& b) {
    if (cmp(a, b)) return true;
    ::mcp::test::AssertionFailure(file, line, expr_a, expr_b, ::mcp::test::ToString(a), ::mcp::test::ToString(b));
    return false;
}

}  // namespace detail
}  // namespace mcp::test

// ── 通用比较断言 ──
#define MCP_EXPECT_CMP(cmp_obj, a, b) \
    do { \
        (void)::mcp::test::detail::CompareOperands(__FILE__, __LINE__, #a, #b, cmp_obj, (a), (b)); \
    } while (0)

#define MCP_ASSERT_CMP(cmp_obj, a, b) \
    do { \
        if (!::mcp::test::detail::CompareOperands(__FILE__, __LINE__, #a, #b, cmp_obj, (a), (b))) { \
            ::mcp::test::RecordFatalFailure(); \
            return; \
        } \
    } while (0)

#define EXPECT_EQ(a, b) MCP_EXPECT_CMP(::mcp::test::detail::CmpEq{}, a, b)
#define EXPECT_NE(a, b) MCP_EXPECT_CMP(::mcp::test::detail::CmpNe{}, a, b)
#define EXPECT_LT(a, b) MCP_EXPECT_CMP(::mcp::test::detail::CmpLt{}, a, b)
#define EXPECT_LE(a, b) MCP_EXPECT_CMP(::mcp::test::detail::CmpLe{}, a, b)
#define EXPECT_GT(a, b) MCP_EXPECT_CMP(::mcp::test::detail::CmpGt{}, a, b)
#define EXPECT_GE(a, b) MCP_EXPECT_CMP(::mcp::test::detail::CmpGe{}, a, b)
#define ASSERT_EQ(a, b) MCP_ASSERT_CMP(::mcp::test::detail::CmpEq{}, a, b)
#define ASSERT_NE(a, b) MCP_ASSERT_CMP(::mcp::test::detail::CmpNe{}, a, b)
#define ASSERT_LT(a, b) MCP_ASSERT_CMP(::mcp::test::detail::CmpLt{}, a, b)
#define ASSERT_LE(a, b) MCP_ASSERT_CMP(::mcp::test::detail::CmpLe{}, a, b)
#define ASSERT_GT(a, b) MCP_ASSERT_CMP(::mcp::test::detail::CmpGt{}, a, b)
#define ASSERT_GE(a, b) MCP_ASSERT_CMP(::mcp::test::detail::CmpGe{}, a, b)

// ── 布尔、失败与跳过 ──
#define EXPECT_TRUE(expr) \
    do { if (!(expr)) ::mcp::test::AssertionFailure(__FILE__, __LINE__, #expr " evaluated to false"); } while (0)
#define EXPECT_FALSE(expr) \
    do { if (expr) ::mcp::test::AssertionFailure(__FILE__, __LINE__, #expr " evaluated to true"); } while (0)
#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, #expr " evaluated to false"); \
            ::mcp::test::RecordFatalFailure(); \
            return; \
        } \
    } while (0)
#define ASSERT_FALSE(expr) \
    do { \
        if (expr) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, #expr " evaluated to true"); \
            ::mcp::test::RecordFatalFailure(); \
            return; \
        } \
    } while (0)

#define FAIL() \
    do { \
        ::mcp::test::AssertionFailure(__FILE__, __LINE__, "FAIL()"); \
        ::mcp::test::RecordFatalFailure(); \
        return; \
    } while (0)
#define ADD_FAILURE() \
    do { ::mcp::test::AssertionFailure(__FILE__, __LINE__, "ADD_FAILURE()"); } while (0)
#define ADD_FAILURE_AT(file, line) \
    do { ::mcp::test::AssertionFailure(file, line, "ADD_FAILURE_AT"); } while (0)
#define SUCCEED() do {} while (0)

#define ASSERT_NO_FATAL_FAILURE(expr) \
    do { expr; if (::mcp::test::HasFatalFailure()) return; } while (0)
#define EXPECT_NO_FATAL_FAILURE(expr) \
    do { expr; } while (0)

#define GTEST_SKIP(...) throw ::mcp::test::SkipException(__VA_ARGS__)

// ── 异常断言 ──
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
#define ASSERT_THROW(expr, type) MCP_EXPECT_THROW(expr, type, ::mcp::test::RecordFatalFailure(); return)

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
#define ASSERT_NO_THROW(expr) MCP_EXPECT_NO_THROW(expr, ::mcp::test::RecordFatalFailure(); return)

#define MCP_EXPECT_ANY_THROW(expr, stop) \
    do { \
        bool _mcp_thrown = false; \
        try { expr; } \
        catch (...) { _mcp_thrown = true; } \
        if (!_mcp_thrown) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, \
                std::string("expected exception from: " #expr)); \
            stop; \
        } \
    } while (0)
#define EXPECT_ANY_THROW(expr) MCP_EXPECT_ANY_THROW(expr, (void)0)
#define ASSERT_ANY_THROW(expr) MCP_EXPECT_ANY_THROW(expr, ::mcp::test::RecordFatalFailure(); return)

#define MCP_EXPECT_THROW_MSG(expr, type, substr, stop) \
    do { \
        bool _mcp_any_thrown = false; \
        bool _mcp_type_matched = false; \
        std::string _mcp_what; \
        try { \
            expr; \
        } catch (const type& _mcp_e) { \
            _mcp_any_thrown = true; \
            _mcp_type_matched = true; \
            _mcp_what = _mcp_e.what(); \
        } catch (...) { \
            _mcp_any_thrown = true; \
            _mcp_what = ::mcp::test::detail::CaughtExceptionMessage(); \
        } \
        if (!_mcp_any_thrown) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, \
                std::string("expected exception ") + #type + " from: " #expr + ", but nothing was thrown"); \
            stop; \
        } else if (!_mcp_type_matched) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, \
                std::string("expected exception ") + #type + " from: " #expr + \
                ", but caught different exception: " + _mcp_what); \
            stop; \
        } else if (std::string_view(_mcp_what).find(substr) == std::string_view::npos) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, \
                std::string("exception message \"") + _mcp_what + "\" does not contain \"" + \
                std::string(substr) + "\" from: " #expr); \
            stop; \
        } \
    } while (0)
#define EXPECT_THROW_MSG(expr, type, substr) MCP_EXPECT_THROW_MSG(expr, type, substr, (void)0)
#define ASSERT_THROW_MSG(expr, type, substr) MCP_EXPECT_THROW_MSG(expr, type, substr, ::mcp::test::RecordFatalFailure(); return)

// ── 字符串断言 ──
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
            ::mcp::test::RecordFatalFailure(); \
            return; \
        } \
    } while (0)
#define EXPECT_STRNE(a, b) \
    do { \
        if (std::string_view((a) ? (a) : "") == std::string_view((b) ? (b) : "")) \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, #a, #b, \
                ::mcp::test::ToString((a) ? (a) : ""), ::mcp::test::ToString((b) ? (b) : "")); \
    } while (0)
#define ASSERT_STRNE(a, b) \
    do { \
        if (std::string_view((a) ? (a) : "") == std::string_view((b) ? (b) : "")) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, #a, #b, \
                ::mcp::test::ToString((a) ? (a) : ""), ::mcp::test::ToString((b) ? (b) : "")); \
            ::mcp::test::RecordFatalFailure(); \
            return; \
        } \
    } while (0)

#define EXPECT_STRCASEEQ(a, b) \
    do { \
        if (!::mcp::test::detail::AsciiCaseEqual((a) ? (a) : "", (b) ? (b) : "")) \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, #a, #b, \
                ::mcp::test::ToString((a) ? (a) : ""), ::mcp::test::ToString((b) ? (b) : "")); \
    } while (0)
#define ASSERT_STRCASEEQ(a, b) \
    do { \
        if (!::mcp::test::detail::AsciiCaseEqual((a) ? (a) : "", (b) ? (b) : "")) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, #a, #b, \
                ::mcp::test::ToString((a) ? (a) : ""), ::mcp::test::ToString((b) ? (b) : "")); \
            ::mcp::test::RecordFatalFailure(); \
            return; \
        } \
    } while (0)
#define EXPECT_STRCASENE(a, b) \
    do { \
        if (::mcp::test::detail::AsciiCaseEqual((a) ? (a) : "", (b) ? (b) : "")) \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, #a, #b, \
                ::mcp::test::ToString((a) ? (a) : ""), ::mcp::test::ToString((b) ? (b) : "")); \
    } while (0)
#define ASSERT_STRCASENE(a, b) \
    do { \
        if (::mcp::test::detail::AsciiCaseEqual((a) ? (a) : "", (b) ? (b) : "")) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, #a, #b, \
                ::mcp::test::ToString((a) ? (a) : ""), ::mcp::test::ToString((b) ? (b) : "")); \
            ::mcp::test::RecordFatalFailure(); \
            return; \
        } \
    } while (0)

// ── 浮点断言 ──
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
#define ASSERT_DOUBLE_EQ(a, b) \
    do { \
        double _mcp_a = (a); \
        double _mcp_b = (b); \
        double _mcp_diff = _mcp_a - _mcp_b; \
        if (_mcp_diff < 0) _mcp_diff = -_mcp_diff; \
        double _mcp_scale = (_mcp_a < 0 ? -_mcp_a : _mcp_a) + (_mcp_b < 0 ? -_mcp_b : _mcp_b); \
        if (_mcp_scale < 1.0) _mcp_scale = 1.0; \
        if (_mcp_diff > 1e-12 * _mcp_scale) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, #a, #b, \
                ::mcp::test::ToString(_mcp_a), ::mcp::test::ToString(_mcp_b)); \
            ::mcp::test::RecordFatalFailure(); \
            return; \
        } \
    } while (0)

#define EXPECT_FLOAT_EQ(a, b) \
    do { \
        float _mcp_a = (a); \
        float _mcp_b = (b); \
        float _mcp_diff = _mcp_a - _mcp_b; \
        if (_mcp_diff < 0) _mcp_diff = -_mcp_diff; \
        float _mcp_scale = (_mcp_a < 0 ? -_mcp_a : _mcp_a) + (_mcp_b < 0 ? -_mcp_b : _mcp_b); \
        if (_mcp_scale < 1.0f) _mcp_scale = 1.0f; \
        if (_mcp_diff > 1e-6f * _mcp_scale) \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, #a, #b, \
                ::mcp::test::ToString(_mcp_a), ::mcp::test::ToString(_mcp_b)); \
    } while (0)
#define ASSERT_FLOAT_EQ(a, b) \
    do { \
        float _mcp_a = (a); \
        float _mcp_b = (b); \
        float _mcp_diff = _mcp_a - _mcp_b; \
        if (_mcp_diff < 0) _mcp_diff = -_mcp_diff; \
        float _mcp_scale = (_mcp_a < 0 ? -_mcp_a : _mcp_a) + (_mcp_b < 0 ? -_mcp_b : _mcp_b); \
        if (_mcp_scale < 1.0f) _mcp_scale = 1.0f; \
        if (_mcp_diff > 1e-6f * _mcp_scale) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, #a, #b, \
                ::mcp::test::ToString(_mcp_a), ::mcp::test::ToString(_mcp_b)); \
            ::mcp::test::RecordFatalFailure(); \
            return; \
        } \
    } while (0)

#define MCP_EXPECT_NEAR(a, b, abs_error, stop) \
    do { \
        double _mcp_a = (a); \
        double _mcp_b = (b); \
        double _mcp_eps = (abs_error); \
        double _mcp_diff = _mcp_a - _mcp_b; \
        if (_mcp_diff < 0) _mcp_diff = -_mcp_diff; \
        if (_mcp_diff > _mcp_eps) { \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, \
                std::string("The difference between ") + ::mcp::test::ToString(_mcp_a) + \
                " and " + ::mcp::test::ToString(_mcp_b) + " is " + ::mcp::test::ToString(_mcp_diff) + \
                ", which exceeds " + ::mcp::test::ToString(_mcp_eps)); \
            stop; \
        } \
    } while (0)
#define EXPECT_NEAR(a, b, abs_error) MCP_EXPECT_NEAR(a, b, abs_error, (void)0)
#define ASSERT_NEAR(a, b, abs_error) MCP_EXPECT_NEAR(a, b, abs_error, ::mcp::test::RecordFatalFailure(); return)

// ── 匹配器 ──
namespace mcp::test {

template <typename T>
struct EqMatcher {
    T expected;

    template <typename U>
    bool Match(const U& v) const { return v == expected; }
    std::string Describe() const { return "Eq(" + ToString(expected) + ")"; }
};

template <typename T>
EqMatcher<T> Eq(T v) { return EqMatcher<T>{std::move(v)}; }

struct HasSubstrMatcher {
    std::string substring;

    bool Match(std::string_view v) const { return v.find(substring) != std::string_view::npos; }
    std::string Describe() const { return "HasSubstr(" + ToString(substring) + ")"; }
};

inline HasSubstrMatcher HasSubstr(std::string s) { return HasSubstrMatcher{std::move(s)}; }

struct SizeIsMatcher {
    std::size_t size;

    template <typename C>
    bool Match(const C& c) const { return c.size() == size; }
    std::string Describe() const { return "SizeIs(" + std::to_string(size) + ")"; }
};

inline SizeIsMatcher SizeIs(std::size_t n) { return SizeIsMatcher{n}; }

template <typename... Ts>
struct ElementsAreMatcher {
    std::tuple<Ts...> expected;

    template <typename C>
    bool Match(const C& c) const {
        if (c.size() != sizeof...(Ts)) return false;
        return MatchImpl(c, std::index_sequence_for<Ts...>{});
    }
    std::string Describe() const { return "ElementsAre(" + DescribeImpl(std::index_sequence_for<Ts...>{}) + ")"; }

private:
    template <typename C, std::size_t... Is>
    bool MatchImpl(const C& c, std::index_sequence<Is...>) const {
        auto it = std::begin(c);
        bool matched = true;
        ((matched = matched && (*std::next(it, Is) == std::get<Is>(expected))), ...);
        return matched;
    }

    template <std::size_t... Is>
    std::string DescribeImpl(std::index_sequence<Is...>) const {
        std::string result;
        bool first = true;
        ((result += (first ? "" : ", ") + ToString(std::get<Is>(expected)), first = false), ...);
        return result;
    }
};

template <typename... Ts>
ElementsAreMatcher<Ts...> ElementsAre(Ts... vs) {
    return ElementsAreMatcher<Ts...>{std::tuple<Ts...>(std::move(vs)...)};
}

}  // namespace mcp::test

// ── 匹配器断言 ──
namespace mcp::test {
namespace detail {

template <typename Matcher, typename Value>
inline bool MatchValueOrFail(const char* file, int line, const char* expr,
                             Matcher matcher, const Value& value) {
    if (matcher.Match(value)) return true;
    ::mcp::test::AssertionFailure(file, line,
        std::string("Value of: ") + expr + "\n  Actual: " + ::mcp::test::ToString(value) +
        "\nExpected: " + matcher.Describe());
    return false;
}

}  // namespace detail
}  // namespace mcp::test

#define EXPECT_THAT(value, matcher_expr) \
    do { \
        (void)::mcp::test::detail::MatchValueOrFail(__FILE__, __LINE__, #value, (matcher_expr), (value)); \
    } while (0)
#define ASSERT_THAT(value, matcher_expr) \
    do { \
        if (!::mcp::test::detail::MatchValueOrFail(__FILE__, __LINE__, #value, (matcher_expr), (value))) { \
            ::mcp::test::RecordFatalFailure(); \
            return; \
        } \
    } while (0)
