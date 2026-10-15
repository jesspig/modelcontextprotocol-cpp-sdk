#pragma once

#include <gtest/gtest.h>

#include <string_view>

namespace mcp::test {

inline std::string_view CurrentTestName() {
    const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
    return info != nullptr ? std::string_view(info->name()) : std::string_view();
}

inline std::string_view CurrentSuiteName() {
    const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
    return info != nullptr ? std::string_view(info->test_suite_name()) : std::string_view();
}

}  // namespace mcp::test
