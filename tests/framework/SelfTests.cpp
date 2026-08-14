// SelfTests.cpp — 自研测试框架自测（仅自研宏）

#include <mcp/test/McpTest.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

// ── ToString ──
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

// ── 断言宏全通过 ──
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

// ── 断言失败记录机制 ──
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

// ── 当前测试名反射 ──
TEST(FrameworkSelfTest, CurrentTestNameReflects) {
    EXPECT_EQ(mcp::test::CurrentTestName(), "CurrentTestNameReflects");
    EXPECT_EQ(mcp::test::CurrentSuiteName(), "FrameworkSelfTest");
}

// ── fixture 生命周期 ──
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

// ── 注册表 ──
TEST(FrameworkSelfTest, RegistryHasEntries) {
    EXPECT_GT(mcp::test::Registry::Instance().Entries().size(), 0u);
    EXPECT_TRUE(&mcp::test::Registry::Instance() == &mcp::test::Registry::Instance());
}
