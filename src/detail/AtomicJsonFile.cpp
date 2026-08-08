// AtomicJsonFile.cpp - Atomic write + JSON load helpers

#include <mcp/detail/AtomicJsonFile.hpp>
#include <mcp/Log.hpp>

#include <fstream>
#include <stdexcept>
#include <string>

namespace mcp::detail {

bool WriteAtomic(const std::filesystem::path& path, const std::string& contents) {
    auto tmp_path = path;
    tmp_path += ".tmp";
    {
        std::ofstream file(tmp_path, std::ios::binary);
        if (!file.is_open()) {
            MCP_LOG(Error, "atomic write: failed to open tmp file: " + tmp_path.string());
            return false;
        }
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!file) {
            file.close();
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
            MCP_LOG(Error, "atomic write: failed while writing: " + tmp_path.string());
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        std::filesystem::remove(tmp_path, ec);
        MCP_LOG(Error, "atomic write: rename failed for " + path.string() + ": " + ec.message());
        return false;
    }
    return true;
}

bool WriteAtomic(const std::filesystem::path& path, const JsonValue& json) {
    return WriteAtomic(path, json.Dump(2));
}

JsonValue LoadJson(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return JsonValue();
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("load json: failed to open " + path.string());
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto json = JsonValue::Parse(content);
    if (json.IsNull()) {
        throw std::runtime_error("load json: invalid JSON in " + path.string());
    }
    return json;
}

} // namespace mcp::detail
