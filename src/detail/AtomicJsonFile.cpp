#include <mcp/detail/AtomicJsonFile.hpp>
#include <mcp/Log.hpp>
#include <mcp/McpError.hpp>

#include <atomic>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <random>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <fstream>
#include <stdexcept>
#include <string>

namespace mcp::detail {

namespace {

std::string AtomicJsonTempSuffix() {
    static std::atomic<unsigned long long> tmp_counter{0};
    std::random_device rd;
    unsigned long long salt =
        (static_cast<unsigned long long>(rd()) << 32) ^ static_cast<unsigned long long>(rd());
    return ".tmp." + std::to_string(
#ifdef _WIN32
        static_cast<long long>(GetCurrentProcessId())
#else
        static_cast<long long>(getpid())
#endif
        ) + "." + std::to_string(tmp_counter.fetch_add(1)) +
        "." + std::to_string(salt);
}

void AtomicJsonRemoveTemp(const std::filesystem::path& tmp_path) {
    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
}

} // anonymous namespace

bool WriteAtomic(const std::filesystem::path& path, const std::string& contents) {
    static std::atomic<unsigned long long> tmp_counter{0};
    auto tmp_path = path;
    tmp_path += ".tmp." + std::to_string(
        static_cast<long long>(
#ifdef _WIN32
            GetCurrentProcessId()
#else
            getpid()
#endif
        )) + "." + std::to_string(tmp_counter.fetch_add(1));
#ifdef _WIN32
    std::FILE* file = _wfopen(tmp_path.wstring().c_str(), L"wb");
#else
    std::FILE* file = std::fopen(tmp_path.string().c_str(), "wb");
#endif
    if (!file) {
        MCP_LOG(Error, "atomic write: failed to open tmp file: " + tmp_path.string());
        return false;
    }
    bool ok = std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
    ok = std::fflush(file) == 0 && ok;
#ifdef _WIN32
    intptr_t os_handle = _get_osfhandle(_fileno(file));
    ok = os_handle != -1 && FlushFileBuffers(reinterpret_cast<HANDLE>(os_handle)) != 0 && ok;
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

bool WriteAtomicSecure(const std::filesystem::path& path, const std::string& contents) {
    auto tmp_path = path;
    tmp_path += AtomicJsonTempSuffix();

#ifdef _WIN32
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr)) {
        throw McpError(McpErrorCode::InternalError,
            "atomic secure write: failed to build an owner-only security descriptor");
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    HANDLE handle = CreateFileW(tmp_path.wstring().c_str(), GENERIC_WRITE, 0,
        &attributes, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    LocalFree(descriptor);
    if (handle == INVALID_HANDLE_VALUE) {
        throw McpError(McpErrorCode::InternalError,
            "atomic secure write: failed to create an exclusive temp file for " +
            path.string());
    }
    const char* data = contents.data();
    size_t remaining = contents.size();
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(handle, data, static_cast<DWORD>(remaining), &written, nullptr) ||
            written == 0) {
            CloseHandle(handle);
            AtomicJsonRemoveTemp(tmp_path);
            throw McpError(McpErrorCode::InternalError,
                "atomic secure write: failed to write " + path.string());
        }
        data += written;
        remaining -= written;
    }
    if (!FlushFileBuffers(handle)) {
        CloseHandle(handle);
        AtomicJsonRemoveTemp(tmp_path);
        throw McpError(McpErrorCode::InternalError,
            "atomic secure write: failed to flush " + path.string());
    }
    CloseHandle(handle);
#else
    int fd = ::open(tmp_path.c_str(),
        O_WRONLY | O_CREAT | O_EXCL
#ifdef O_NOFOLLOW
        | O_NOFOLLOW
#endif
        , S_IRUSR | S_IWUSR);
    if (fd < 0) {
        throw McpError(McpErrorCode::InternalError,
            "atomic secure write: failed to create an exclusive temp file for " +
            path.string());
    }
    const char* data = contents.data();
    size_t remaining = contents.size();
    while (remaining > 0) {
        ssize_t written = ::write(fd, data, remaining);
        if (written <= 0) {
            if (written < 0 && errno == EINTR) continue;
            ::close(fd);
            AtomicJsonRemoveTemp(tmp_path);
            throw McpError(McpErrorCode::InternalError,
                "atomic secure write: failed to write " + path.string());
        }
        data += written;
        remaining -= static_cast<size_t>(written);
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        AtomicJsonRemoveTemp(tmp_path);
        throw McpError(McpErrorCode::InternalError,
            "atomic secure write: failed to flush " + path.string());
    }
    ::close(fd);
#endif

    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        AtomicJsonRemoveTemp(tmp_path);
        throw McpError(McpErrorCode::InternalError,
            "atomic secure write: rename failed for " + path.string() + ": " + ec.message());
    }

#ifndef _WIN32
    auto parent = path.parent_path();
    if (!parent.empty()) {
        int dir_fd = ::open(parent.c_str(), O_RDONLY);
        if (dir_fd >= 0) {
            ::fsync(dir_fd);
            ::close(dir_fd);
        }
    }
#endif
    return true;
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
