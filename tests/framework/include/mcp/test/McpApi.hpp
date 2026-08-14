#pragma once
// McpApi.hpp — 自研测试框架 API（无断言宏，供 gtest 见证用例等无冲突引入）

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace mcp::test {

class TestCase {
public:
    virtual ~TestCase() = default;
    virtual void SetUp() {}
    virtual void TearDown() {}
    virtual void RunBody() {}

    int FailureCount() const { return failure_count_.load(); }
    bool HasFailures() const { return FailureCount() > 0; }
    const std::vector<std::string>& Failures() const { return failures_; }
    void RecordFailure(std::string message);
    void ClearFailures();

private:
    std::atomic<int> failure_count_{0};
    std::mutex failure_mutex_;
    std::vector<std::string> failures_;
};

using TestFactory = std::function<std::unique_ptr<TestCase>()>;
struct TestEntry {
    std::string suite;
    std::string name;
    TestFactory factory;
};

class Registry {
public:
    static Registry& Instance();
    void Register(std::string suite, std::string name, TestFactory factory);
    const std::vector<TestEntry>& Entries() const;
    std::vector<const TestEntry*> Select(std::string_view filter) const;

private:
    Registry() = default;
    std::vector<TestEntry> entries_;
};

void SetCurrentTest(TestCase* test);
TestCase* CurrentTest();

std::string_view CurrentTestName();
std::string_view CurrentSuiteName();

int RunAll(int argc, char** argv);

void AssertionFailure(const char* file, int line,
                      std::string_view expr_a, std::string_view expr_b,
                      std::string_view val_a, std::string_view val_b);
void AssertionFailure(const char* file, int line, std::string_view message);

// ── ToString ──
namespace detail {

template <typename T, typename = void>
struct HasStreamPrint : std::false_type {};
template <typename T>
struct HasStreamPrint<T, std::void_t<decltype(
                             std::declval<std::ostringstream&>() << std::declval<const T&>())>>
    : std::true_type {};

}  // namespace detail

template <typename T>
std::string ToStringImpl(const T& v, std::true_type) {  // 枚举
    return std::to_string(static_cast<long long>(v));
}

template <typename T>
std::string ToStringImplStream(const T& v, std::true_type) {  // ostream 可打印
    std::ostringstream os;
    os << v;
    return os.str();
}

template <typename T>
std::string ToStringImplStream(const T&, std::false_type) {  // 兜底：类型名
    return "<" + std::string(typeid(T).name()) + ">";
}

template <typename T>
std::string ToStringImpl(const T& v, std::false_type) {
    return ToStringImplStream(v, detail::HasStreamPrint<T>{});
}

template <typename T>
std::string ToString(const T& v) { return ToStringImpl(v, std::is_enum<T>{}); }

inline std::string ToString(const std::string& v) { return "\"" + v + "\""; }
inline std::string ToString(std::string_view v) { return "\"" + std::string(v) + "\""; }
inline std::string ToString(const char* v) { return v ? "\"" + std::string(v) + "\"" : "(null)"; }
inline std::string ToString(bool v) { return v ? "true" : "false"; }
inline std::string ToString(std::nullptr_t) { return "(null)"; }

template <typename T>
std::string ToString(const std::optional<T>& v) {
    return v ? ToString(*v) : "(empty)";
}

}  // namespace mcp::test
