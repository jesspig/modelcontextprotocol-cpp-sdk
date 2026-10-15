#include <mcp/test/McpTest.hpp>
#include <mcp/test/McpTrace.hpp>

#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

TEST(FrameworkSelfTest, ToStringInt) {
    EXPECT_EQ(mcp::test::ToString(42), "42");
}

TEST(FrameworkSelfTest, ToStringString) {
    EXPECT_EQ(mcp::test::ToString(std::string("ab")), "\"ab\"");
}

TEST(FrameworkSelfTest, ToStringBool) {
    EXPECT_EQ(mcp::test::ToString(true), "true");
    EXPECT_EQ(mcp::test::ToString(false), "false");
}

TEST(FrameworkSelfTest, ToStringEnum) {
    enum class E { A = 7 };
    EXPECT_EQ(mcp::test::ToString(E::A), "7");
}

TEST(FrameworkSelfTest, ToStringNullptr) {
    EXPECT_EQ(mcp::test::ToString(nullptr), "(null)");
}

TEST(FrameworkSelfTest, ToStringCharPtr) {
    EXPECT_EQ(mcp::test::ToString("hi"), "\"hi\"");
    EXPECT_EQ(mcp::test::ToString(static_cast<const char*>(nullptr)), "(null)");
}

TEST(FrameworkSelfTest, ToStringStringView) {
    EXPECT_EQ(mcp::test::ToString(std::string_view("sv")), "\"sv\"");
}

TEST(FrameworkSelfTest, ToStringOptional) {
    EXPECT_EQ(mcp::test::ToString(std::optional<int>(5)), "5");
    EXPECT_EQ(mcp::test::ToString(std::optional<int>()), "(empty)");
}

TEST(FrameworkSelfTest, AssertionMacrosPass) {
    EXPECT_EQ(1, 1);
    EXPECT_NE(1, 2);
    EXPECT_LT(1, 2);
    EXPECT_LE(2, 2);
    EXPECT_GT(2, 1);
    EXPECT_GE(2, 2);
    EXPECT_TRUE(true);
    EXPECT_FALSE(false);
    EXPECT_THROW(throw std::runtime_error("x"), std::runtime_error);
    EXPECT_NO_THROW(int x = 0; (void)x);
    EXPECT_STREQ("a", "a");
}

TEST(FrameworkSelfTest, AssertionFailureRecordsFailure) {
    mcp::test::AssertionFailure(__FILE__, __LINE__, "probe");
    EXPECT_GE(FailureCount(), 1);
    ClearFailures();
    EXPECT_EQ(FailureCount(), 0);
}

TEST(FrameworkSelfTest, SubthreadFailureCounted) {
    std::thread t([] {
        mcp::test::AssertionFailure(__FILE__, __LINE__, "subthread probe");
    });
    t.join();
    EXPECT_GE(FailureCount(), 1);
    ClearFailures();
}

TEST(FrameworkSelfTest, CurrentTestNameReflects) {
    EXPECT_EQ(mcp::test::CurrentTestName(), "CurrentTestNameReflects");
    EXPECT_EQ(mcp::test::CurrentSuiteName(), "FrameworkSelfTest");
}

namespace {

class CounterFixture : public mcp::test::TestCase {
public:
    void SetUp() override { order_ += "S"; }
    void TearDown() override { order_ += "T"; }
    std::string order_;
};

}  // namespace

TEST_F(CounterFixture, LifecycleOrder) {
    EXPECT_EQ(order_, "S");
}

TEST(FrameworkSelfTest, RegistryHasEntries) {
    EXPECT_GT(mcp::test::Registry::Instance().Entries().size(), 0u);
    EXPECT_TRUE(&mcp::test::Registry::Instance() == &mcp::test::Registry::Instance());
}

namespace {

int NextCounter(int& counter) { return ++counter; }

}  // namespace

TEST(FrameworkSelfTest, ExpectCompareEvaluatesOperandsOnce) {
    int counter = 0;
    EXPECT_EQ(NextCounter(counter), 1);
    EXPECT_EQ(counter, 1);

    int counter2 = 0;
    EXPECT_NE(NextCounter(counter2), 99);
    EXPECT_EQ(counter2, 1);
}

TEST(FrameworkSelfTest, ExpectCompareEvaluatesOperandsOnceOnFailure) {
    int counter = 0;
    EXPECT_EQ(NextCounter(counter), 0);
    const int counter_after_failure = counter;
    ClearFailures();
    EXPECT_EQ(FailureCount(), 0);
    EXPECT_EQ(counter_after_failure, 1);
}

namespace {

void FatalProbe(int& marker) {
    ASSERT_EQ(1, 2);
    marker = 1;
}

void FailProbe(int& marker) {
    FAIL();
    marker = 1;
}

void NonFatalProbe() { ADD_FAILURE(); }

}  // namespace

TEST(FrameworkSelfTest, AssertStopsExecutionOnFailure) {
    int marker = 0;
    FatalProbe(marker);
    const int marker_value = marker;
    const bool fatal_flag = mcp::test::HasFatalFailure();
    ClearFailures();
    mcp::test::ClearFatalFailure();
    EXPECT_EQ(marker_value, 0);
    EXPECT_TRUE(fatal_flag);
}

TEST(FrameworkSelfTest, FailMacroIsFatal) {
    int marker = 0;
    FailProbe(marker);
    const int marker_value = marker;
    const bool fatal_flag = mcp::test::HasFatalFailure();
    ClearFailures();
    mcp::test::ClearFatalFailure();
    EXPECT_EQ(marker_value, 0);
    EXPECT_TRUE(fatal_flag);
}

TEST(FrameworkSelfTest, AddFailureIsNonFatal) {
    ADD_FAILURE();
    const int failure_count = FailureCount();
    ClearFailures();
    EXPECT_EQ(failure_count, 1);
}

TEST(FrameworkSelfTest, SucceedAndNoFatalFailureContinue) {
    SUCCEED();
    bool continued = false;
    ASSERT_NO_FATAL_FAILURE(NonFatalProbe());
    continued = true;
    const int failure_count = FailureCount();
    ClearFailures();
    EXPECT_TRUE(continued);
    EXPECT_EQ(failure_count, 1);
}

TEST(FrameworkSelfTest, DoubleAndNearAssertionsPass) {
    ASSERT_DOUBLE_EQ(1.0, 1.0);
    EXPECT_NEAR(1.0, 1.05, 0.1);
}

TEST(FrameworkSelfTest, ScopedTraceAppearsInFailure) {
    {
        SCOPED_TRACE("probe-trace");
        mcp::test::AssertionFailure(__FILE__, __LINE__, "probe-trace");
    }
    const std::vector<std::string> failures = Failures();
    ClearFailures();
    EXPECT_EQ(failures.size(), 1u);
    if (failures.size() == 1u) {
        EXPECT_NE(failures.back().find("probe-trace"), std::string::npos);
        EXPECT_NE(failures.back().find("Trace:"), std::string::npos);
    }
}

TEST(FrameworkSelfTest, ScopedTraceNestedInLoop) {
    for (int i = 0; i < 2; ++i) {
        SCOPED_TRACE(i);
        mcp::test::AssertionFailure(__FILE__, __LINE__, "loop-trace");
    }
    const std::vector<std::string> failures = Failures();
    ClearFailures();
    EXPECT_EQ(failures.size(), 2u);
    if (failures.size() == 2u) {
        EXPECT_NE(failures[0].find(": 0"), std::string::npos);
        EXPECT_NE(failures[1].find(": 1"), std::string::npos);
    }
}

namespace {

struct SkipFixture : mcp::test::TestCase {
    void SetUp() override { GTEST_SKIP("skip-setup"); }
};

}  // namespace

TEST_F(SkipFixture, BodyDoesNotRun) {
    FAIL();
}

TEST(FrameworkSelfTest, SkipInBodyMarksSkipped) {
    GTEST_SKIP("skip-body");
}

namespace {

class SuiteHookFixture : public mcp::test::TestCase {
public:
    static int& SetupCalls() {
        static int calls = 0;
        return calls;
    }

    static int& TeardownCalls() {
        static int calls = 0;
        return calls;
    }

    static void SetUpTestSuite() {
        ++SetupCalls();
        if (SetupCalls() > 1) throw std::runtime_error("SetUpTestSuite called twice");
    }

    static void TearDownTestSuite() {
        ++TeardownCalls();
        if (TeardownCalls() > 1) throw std::runtime_error("TearDownTestSuite called twice");
    }
};

}  // namespace

TEST_F(SuiteHookFixture, SetUpTestSuiteRunsBeforeFirstCase) {
    EXPECT_EQ(SetupCalls(), 1);
}

TEST_F(SuiteHookFixture, SetUpTestSuiteNotRepeatedForSecondCase) {
    EXPECT_EQ(SetupCalls(), 1);
}

namespace {

int& EnvironmentSetUpCount() {
    static int count = 0;
    return count;
}

int& EnvironmentTearDownCount() {
    static int count = 0;
    return count;
}

struct ProbeEnvironment : mcp::test::Environment {
    void SetUp() override { ++EnvironmentSetUpCount(); }
    void TearDown() override { ++EnvironmentTearDownCount(); }
};

struct ProbeEnvironmentRegistrar {
    ProbeEnvironmentRegistrar() {
        mcp::test::AddGlobalTestEnvironment(std::make_unique<ProbeEnvironment>());
    }
};

const ProbeEnvironmentRegistrar kProbeEnvironmentRegistrar;

}  // namespace

TEST(FrameworkSelfTest, GlobalEnvironmentSetUpRunsOnce) {
    (void)kProbeEnvironmentRegistrar;
    EXPECT_EQ(EnvironmentSetUpCount(), 1);
}

TEST(FrameworkSelfTest, StringCaseAssertionsPass) {
    EXPECT_STRNE("a", "b");
    EXPECT_STRCASEEQ("AbC", "aBc");
    EXPECT_STRCASENE("abc", "xyz");
}

TEST(FrameworkSelfTest, ExceptionAssertionsPass) {
    EXPECT_ANY_THROW(throw std::runtime_error("x"));
    EXPECT_THROW_MSG(throw std::runtime_error("boom-42"), std::runtime_error, "boom");
}

TEST(FrameworkSelfTest, FloatAssertionsPass) {
    EXPECT_FLOAT_EQ(1.0f, 1.0f);
    EXPECT_NEAR(1.0, 1.1, 0.2);
}

TEST(FrameworkSelfTest, MatchersMatchValues) {
    EXPECT_THAT(std::string("hello world"), mcp::test::HasSubstr("world"));
    const std::vector<int> values{1, 2, 3};
    EXPECT_THAT(values, mcp::test::SizeIs(3));
    EXPECT_THAT(values, mcp::test::ElementsAre(1, 2, 3));
    EXPECT_THAT(42, mcp::test::Eq(42));
}

TEST(FrameworkSelfTest, MatcherFailureIsRecorded) {
    EXPECT_THAT(std::string("abc"), mcp::test::HasSubstr("zzz"));
    const int failure_count = FailureCount();
    ClearFailures();
    EXPECT_GE(failure_count, 1);
}

TEST(FrameworkSelfTest, ToStringExpandsContainers) {
    EXPECT_EQ(mcp::test::ToString(std::vector<int>{1, 2, 3}), "[1, 2, 3]");
    EXPECT_EQ(mcp::test::ToString(std::pair<int, std::string>(1, "x")), "(1, \"x\")");
    EXPECT_EQ(mcp::test::ToString(std::map<std::string, int>{{"a", 1}}), "[(\"a\", 1)]");
}

namespace {

struct AdlPrinted {};

void PrintTo(const AdlPrinted&, std::ostream* os) { *os << "adl-printed"; }

}  // namespace

TEST(FrameworkSelfTest, ToStringPrefersAdlPrintTo) {
    EXPECT_EQ(mcp::test::ToString(AdlPrinted{}), "adl-printed");
}

TEST(FrameworkSelfTest, RecordPropertyStoresValues) {
    RecordProperty("k", "v");
    RecordProperty("n", 7);
    EXPECT_EQ(Properties().at("k"), "v");
    EXPECT_EQ(Properties().at("n"), "7");
}

namespace {

int g_test_end_listener_calls = 0;

}  // namespace

TEST(FrameworkSelfTest, TestEndListenerCanRegister) {
    mcp::test::AddTestEndListener([](const mcp::test::TestResult&) { ++g_test_end_listener_calls; });
    SUCCEED();
}
