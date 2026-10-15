#pragma once
#include <string_view>

namespace mcp::test {

std::string_view CurrentTestName();
std::string_view CurrentSuiteName();

}  // namespace mcp::test
