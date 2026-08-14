#pragma once
// McpTestInfo.hpp — 当前测试名反射

#include <string_view>

namespace mcp::test {

std::string_view CurrentTestName();
std::string_view CurrentSuiteName();

}  // namespace mcp::test
