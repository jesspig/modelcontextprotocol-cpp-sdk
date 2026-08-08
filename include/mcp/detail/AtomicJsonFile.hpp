// AtomicJsonFile.hpp - Shared atomic write + JSON load helpers

#pragma once

#include <mcp/JsonValue.hpp>

#include <filesystem>
#include <string_view>

namespace mcp::detail {

// Atomically write raw bytes to `path` via a sibling `.tmp` file + rename.
// Returns true on success; on failure removes the leftover `.tmp` and logs Error.
// Caller must ensure the parent directory exists.
bool WriteAtomic(const std::filesystem::path& path, const std::string& contents);

// Convenience overload: dumps `json` (indent 2) and writes it atomically.
bool WriteAtomic(const std::filesystem::path& path, const JsonValue& json);

// Load a JSON file. Returns a null JsonValue if the file does not exist.
// Throws std::runtime_error if the file cannot be opened or is not valid JSON.
JsonValue LoadJson(const std::filesystem::path& path);

} // namespace mcp::detail
