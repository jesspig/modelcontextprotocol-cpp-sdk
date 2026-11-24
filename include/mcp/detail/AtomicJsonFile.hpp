#pragma once

#include <mcp/JsonValue.hpp>

#include <filesystem>
#include <string_view>

namespace mcp::detail {

[[nodiscard]] bool WriteAtomic(const std::filesystem::path& path, const std::string& contents);

[[nodiscard]] bool WriteAtomic(const std::filesystem::path& path, const JsonValue& json);

[[nodiscard]] bool WriteAtomicSecure(const std::filesystem::path& path, const std::string& contents);

enum class AtomicWriteStage { None, Create, Write, Flush, Rename, ParentSync };

void SetAtomicWriteFailureInjection(AtomicWriteStage stage);

JsonValue LoadJson(const std::filesystem::path& path);

} // namespace mcp::detail
