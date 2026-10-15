#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
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

namespace detail {

inline bool& FatalFailureFlag() {
    static thread_local bool flag = false;
    return flag;
}

}  // namespace detail

inline bool HasFatalFailure() { return detail::FatalFailureFlag(); }
inline void RecordFatalFailure() { detail::FatalFailureFlag() = true; }
inline void ClearFatalFailure() { detail::FatalFailureFlag() = false; }

struct SkipException {
    std::string message;
    SkipException() = default;
    explicit SkipException(std::string msg) : message(std::move(msg)) {}
};

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
    void MarkAbandoned();
    bool IsAbandoned() const;
    void RecordProperty(std::string key, std::string value);
    void RecordProperty(std::string key, long long value);
    const std::map<std::string, std::string>& Properties() const;

private:
    std::atomic<int> failure_count_{0};
    std::mutex failure_mutex_;
    std::vector<std::string> failures_;
    std::atomic<bool> abandoned_{false};
    std::map<std::string, std::string> properties_;
};

using TestFactory = std::function<std::unique_ptr<TestCase>()>;
using LifecycleHook = std::function<void()>;

struct TestEntry {
    std::string suite;
    std::string name;
    LifecycleHook suite_setup;
    LifecycleHook suite_teardown;
    TestFactory factory;
};

class Registry {
public:
    static Registry& Instance();
    void Register(std::string suite, std::string name,
                  LifecycleHook suite_setup, LifecycleHook suite_teardown,
                  TestFactory factory);
    const std::vector<TestEntry>& Entries() const;
    std::vector<const TestEntry*> Select(std::string_view filter) const;

private:
    Registry() = default;
    std::vector<TestEntry> entries_;
};

namespace detail {

template <typename T, typename = void>
struct SuiteSetUpHook {
    static void Run() {}
};
template <typename T>
struct SuiteSetUpHook<T, std::void_t<decltype(T::SetUpTestSuite())>> {
    static void Run() { T::SetUpTestSuite(); }
};

template <typename T, typename = void>
struct SuiteTearDownHook {
    static void Run() {}
};
template <typename T>
struct SuiteTearDownHook<T, std::void_t<decltype(T::TearDownTestSuite())>> {
    static void Run() { T::TearDownTestSuite(); }
};

}  // namespace detail

void SetCurrentTest(TestCase* test);
TestCase* CurrentTest();

std::string_view CurrentTestName();
std::string_view CurrentSuiteName();

int RunAll(int argc, char** argv);

void AssertionFailure(const char* file, int line,
                      std::string_view expr_a, std::string_view expr_b,
                      std::string_view val_a, std::string_view val_b);
void AssertionFailure(const char* file, int line, std::string_view message);

class Environment {
public:
    virtual ~Environment() = default;
    virtual void SetUp() {}
    virtual void TearDown() {}
};

void AddGlobalTestEnvironment(std::unique_ptr<Environment> env);

struct TestResult {
    std::string_view suite;
    std::string_view name;
    bool passed = true;
    bool skipped = false;
    long long elapsed_ms = 0;
};

using TestEndListener = std::function<void(const TestResult&)>;
void AddTestEndListener(TestEndListener listener);

template <typename T>
std::string ToString(const T& v);
inline std::string ToString(const std::string& v);
inline std::string ToString(std::string_view v);
inline std::string ToString(const char* v);
inline std::string ToString(bool v);
inline std::string ToString(std::nullptr_t);
template <typename T>
std::string ToString(const std::optional<T>& v);
template <typename K, typename V>
std::string ToString(const std::pair<K, V>& v);

namespace detail {

template <typename T, typename = void>
struct HasStreamPrint : std::false_type {};
template <typename T>
struct HasStreamPrint<T, std::void_t<decltype(
                             std::declval<std::ostringstream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <typename T>
auto AdlPrintTo(const T& v, std::ostream* os, int) -> decltype(PrintTo(v, os), void()) {
    PrintTo(v, os);
}

template <typename T, typename = void>
struct HasAdlPrintTo : std::false_type {};
template <typename T>
struct HasAdlPrintTo<T, std::void_t<decltype(AdlPrintTo(std::declval<const T&>(),
                                                       static_cast<std::ostream*>(nullptr), 0))>>
    : std::true_type {};

template <typename T>
void AdlPrintTo(const T&, std::ostream*, long) {}

template <typename T, typename = void>
struct HasBeginEnd : std::false_type {};
template <typename T>
struct HasBeginEnd<T, std::void_t<decltype(std::begin(std::declval<const T&>())),
                                  decltype(std::end(std::declval<const T&>()))>>
    : std::true_type {};

template <typename T>
struct IsExpandableContainer
    : std::bool_constant<HasBeginEnd<T>::value &&
                         !std::is_convertible<const T&, std::string_view>::value> {};

}  // namespace detail

template <typename T>
std::string ToStringImplContainer(const T& v, std::true_type) {
    std::string result = "[";
    bool first = true;
    for (const auto& item : v) {
        if (!first) result += ", ";
        first = false;
        result += ToString(item);
    }
    result += "]";
    return result;
}

template <typename T>
std::string ToStringImplContainer(const T&, std::false_type) {
    return "<" + std::string(typeid(T).name()) + ">";
}

template <typename T>
std::string ToStringImplStream(const T& v, std::true_type) {
    std::ostringstream os;
    os << v;
    return os.str();
}

template <typename T>
std::string ToStringImplStream(const T& v, std::false_type) {
    return ToStringImplContainer(v, detail::IsExpandableContainer<T>{});
}

template <typename T>
std::string ToStringImplPrintTo(const T& v, std::true_type) {
    std::ostringstream os;
    detail::AdlPrintTo(v, &os, 0);
    return os.str();
}

template <typename T>
std::string ToStringImplPrintTo(const T& v, std::false_type) {
    return ToStringImplStream(v, detail::HasStreamPrint<T>{});
}

template <typename T>
std::string ToStringImpl(const T& v, std::true_type) {
    return std::to_string(static_cast<long long>(v));
}

template <typename T>
std::string ToStringImpl(const T& v, std::false_type) {
    return ToStringImplPrintTo(v, detail::HasAdlPrintTo<T>{});
}

template <typename T>
std::string ToString(const T& v) { return ToStringImpl(v, std::is_enum<T>{}); }

inline std::string ToString(const std::string& v) { return "\"" + v + "\""; }
inline std::string ToString(std::string_view v) { return "\"" + std::string(v) + "\""; }
inline std::string ToString(const char* v) { return v ? "\"" + std::string(v) + "\"" : "(null)"; }
inline std::string ToString(bool v) { return v ? "true" : "false"; }
inline std::string ToString(std::nullptr_t) { return "(null)"; }

template <typename K, typename V>
std::string ToString(const std::pair<K, V>& v) {
    return "(" + ToString(v.first) + ", " + ToString(v.second) + ")";
}

template <typename T>
std::string ToString(const std::optional<T>& v) {
    return v ? ToString(*v) : "(empty)";
}

template <typename T>
std::string PrintToString(const T& v) { return ToString(v); }

}  // namespace mcp::test
