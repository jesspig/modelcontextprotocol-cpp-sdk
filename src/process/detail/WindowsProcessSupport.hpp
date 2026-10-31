#pragma once

#include <map>
#include <string>
#include <vector>

namespace mcp { namespace process { namespace detail {

std::wstring Utf8ToWide(const std::string& value);
std::string QuoteWindowsArgument(const std::string& argument);
std::string BuildWindowsCommandLine(const std::string& command, const std::vector<std::string>& arguments);
std::wstring BuildWindowsEnvironmentBlock(const std::map<std::string, std::string>& overrides, bool inherit);

}}} // namespace mcp::process::detail
