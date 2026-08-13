// AtomicJsonFile.cpp - Atomic write + JSON load helpers

#include <mcp/detail/AtomicJsonFile.hpp>
#include <mcp/Log.hpp>

#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif
#include <fstream>
#include <stdexcept>
#include <string>

namespace mcp::detail {

bool WriteAtomic(const std::filesystem::path& path, const std::string& contents) {
    auto tmp_path = path;
    tmp_path += ".tmp." + std::to_string(
        static_cast<long long>(
#ifdef _WIN32
            GetCurrentProcessId()
#else
            getpid()
#endif
        ));
    std::FILE* file = std::fopen(tmp_path.string().c_str(), "wb");
    if (!file) {
        MCP_LOG(Error, "atomic write: failed to open tmp file: " + tmp_path.string());
        return false;
    }
    bool ok = std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
    ok = std::fflush(file) == 0 && ok;
#ifdef _WIN32
    ok = FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file)))) != 0 && ok;
#else
    ok = ::fsync(::fileno(file)) == 0 && ok;
#endif
    std::fclose(file);
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        MCP_LOG(Error, "atomic write: failed while writing: " + tmp_path.string());
        return false;
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
