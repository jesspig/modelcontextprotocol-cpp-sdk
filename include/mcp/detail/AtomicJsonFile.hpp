#pragma once

#include <mcp/JsonValue.hpp>

#include <filesystem>
#include <string_view>

namespace mcp::detail {

[[nodiscard]] bool WriteAtomic(const std::filesystem::path& path, const std::string& contents);

[[nodiscard]] bool WriteAtomic(const std::filesystem::path& path, const JsonValue& json);

[[nodiscard]] bool WriteAtomicSecure(const std::filesystem::path& path, const std::string& contents);

JsonValue LoadJson(const std::filesystem::path& path);

} // namespace mcp::detail
