#pragma once

#include <mcp/JsonValue.hpp>

#include <filesystem>
#include <string_view>

namespace mcp::detail {

bool WriteAtomic(const std::filesystem::path& path, const std::string& contents);

bool WriteAtomic(const std::filesystem::path& path, const JsonValue& json);

JsonValue LoadJson(const std::filesystem::path& path);

} // namespace mcp::detail
