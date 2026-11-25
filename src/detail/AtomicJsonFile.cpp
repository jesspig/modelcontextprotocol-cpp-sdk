#include <mcp/detail/AtomicJsonFile.hpp>
#include <mcp/Log.hpp>
#include <mcp/McpError.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <random>
#ifdef _WIN32
#include <windows.h>
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

std::atomic<int> g_atomic_failure_stage{0};

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

bool AtomicStageMatches(AtomicWriteStage stage) {
    int expected = static_cast<int>(stage);
    if (expected == 0) return false;
    int current = g_atomic_failure_stage.load();
    if (current != expected) return false;
    g_atomic_failure_stage.compare_exchange_strong(current, 0);
    return true;
}

#ifdef _WIN32
void AtomicJsonRemoveTemp(const std::filesystem::path& tmp_path) {
    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
}
#endif

struct AtomicWriteOutcome {
    bool ok{true};
    std::string error;
};

AtomicWriteOutcome AtomicWriteFail(const std::string& message) {
    return AtomicWriteOutcome{false, message};
}

#ifdef _WIN32
constexpr int kAtomicRenameAttempts = 8;
constexpr DWORD kAtomicRenameRetryDelayMs = 20;

bool AtomicRenameErrorIsTransient(DWORD error) {
    return error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ||
           error == ERROR_LOCK_VIOLATION ||
           error == ERROR_UNABLE_TO_REMOVE_REPLACED ||
           error == ERROR_UNABLE_TO_MOVE_REPLACEMENT ||
           error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2;
}
#endif

bool AtomicTargetIsSpecial(const std::filesystem::path& path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    return (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0;
#else
    std::error_code ec;
    auto status = std::filesystem::symlink_status(path, ec);
    if (ec) return false;
    return std::filesystem::is_directory(status) || std::filesystem::is_symlink(status);
#endif
}

AtomicWriteOutcome AtomicWriteBytes(const std::filesystem::path& path,
                                    const std::string& contents) {
    if (AtomicTargetIsSpecial(path)) {
        return AtomicWriteFail("atomic write: target is a directory or link: " + path.string());
    }
    auto tmp_path = path;
    tmp_path += AtomicJsonTempSuffix();
    if (AtomicTargetIsSpecial(tmp_path)) {
        return AtomicWriteFail("atomic write: temporary path is a directory or link: " +
                               tmp_path.string());
    }

#ifdef _WIN32
    if (AtomicStageMatches(AtomicWriteStage::Create)) {
        return AtomicWriteFail("atomic write: injected create failure for " + path.string());
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr)) {
        return AtomicWriteFail(
            "atomic write: failed to build an owner-only security descriptor for " +
            path.string());
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    HANDLE handle = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0,
        &attributes, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr);
    LocalFree(descriptor);
    if (handle == INVALID_HANDLE_VALUE) {
        AtomicJsonRemoveTemp(tmp_path);
        return AtomicWriteFail("atomic write: failed to create an exclusive temp file for " +
                               path.string());
    }

    const char* data = contents.data();
    size_t remaining = contents.size();
    bool write_failed = false;
    while (remaining > 0) {
        if (AtomicStageMatches(AtomicWriteStage::Write)) {
            write_failed = true;
            break;
        }
        DWORD chunk = static_cast<DWORD>((std::min)(remaining, static_cast<size_t>(1u << 30)));
        DWORD written = 0;
        if (!WriteFile(handle, data, chunk, &written, nullptr) || written == 0) {
            write_failed = true;
            break;
        }
        data += written;
        remaining -= written;
    }
    if (write_failed) {
        CloseHandle(handle);
        AtomicJsonRemoveTemp(tmp_path);
        return AtomicWriteFail("atomic write: failed to write " + path.string());
    }
    if (AtomicStageMatches(AtomicWriteStage::Flush)) {
        CloseHandle(handle);
        AtomicJsonRemoveTemp(tmp_path);
        return AtomicWriteFail("atomic write: injected flush failure for " + path.string());
    }
    if (!FlushFileBuffers(handle)) {
        CloseHandle(handle);
        AtomicJsonRemoveTemp(tmp_path);
        return AtomicWriteFail("atomic write: failed to flush " + path.string());
    }
    CloseHandle(handle);

    if (AtomicStageMatches(AtomicWriteStage::Rename)) {
        AtomicJsonRemoveTemp(tmp_path);
        return AtomicWriteFail("atomic write: injected rename failure for " + path.string());
    }
    BOOL replaced = FALSE;
    DWORD rename_error = ERROR_SUCCESS;
    for (int attempt = 0; attempt < kAtomicRenameAttempts; ++attempt) {
        DWORD target_attributes = GetFileAttributesW(path.c_str());
        bool use_replace = target_attributes != INVALID_FILE_ATTRIBUTES;
        replaced = use_replace
            ? ReplaceFileW(path.c_str(), tmp_path.c_str(), nullptr, 0, nullptr, nullptr)
            : MoveFileExW(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (replaced) break;
        rename_error = GetLastError();
        MCP_LOG(Warning, "atomic write: rename attempt failed for " + path.string() +
                         " (win32=" + std::to_string(rename_error) + ")");
        bool transient = AtomicRenameErrorIsTransient(rename_error);
        if (!transient || attempt + 1 == kAtomicRenameAttempts) break;
        Sleep(kAtomicRenameRetryDelayMs);
    }
    if (!replaced) {
        AtomicJsonRemoveTemp(tmp_path);
        return AtomicWriteFail("atomic write: rename failed for " + path.string() +
                               " (win32=" + std::to_string(rename_error) + ")");
    }
    if (AtomicStageMatches(AtomicWriteStage::ParentSync)) {
        return AtomicWriteFail("atomic write: injected parent sync failure for " + path.string());
    }
    return AtomicWriteOutcome{};
#else
    auto parent = path.parent_path();
    std::string parent_name = parent.empty() ? "." : parent.string();
    int dir_fd = ::open(parent_name.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) {
        return AtomicWriteFail("atomic write: failed to open parent directory for " +
                               path.string());
    }

    std::string tmp_name = tmp_path.filename().string();
    std::string target_name = path.filename().string();

    if (AtomicStageMatches(AtomicWriteStage::Create)) {
        ::close(dir_fd);
        return AtomicWriteFail("atomic write: injected create failure for " + path.string());
    }
    int fd = ::openat(dir_fd, tmp_name.c_str(),
        O_WRONLY | O_CREAT | O_EXCL
#ifdef O_NOFOLLOW
        | O_NOFOLLOW
#endif
        , S_IRUSR | S_IWUSR);
    if (fd < 0) {
        ::close(dir_fd);
        return AtomicWriteFail("atomic write: failed to create an exclusive temp file for " +
                               path.string());
    }

    const char* data = contents.data();
    size_t remaining = contents.size();
    bool write_failed = false;
    while (remaining > 0) {
        if (AtomicStageMatches(AtomicWriteStage::Write)) {
            write_failed = true;
            break;
        }
        ssize_t written = ::write(fd, data, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            write_failed = true;
            break;
        }
        if (written == 0) {
            write_failed = true;
            break;
        }
        data += written;
        remaining -= static_cast<size_t>(written);
    }
    if (write_failed) {
        ::close(fd);
        ::unlinkat(dir_fd, tmp_name.c_str(), 0);
        ::close(dir_fd);
        return AtomicWriteFail("atomic write: failed to write " + path.string());
    }
    if (AtomicStageMatches(AtomicWriteStage::Flush)) {
        ::close(fd);
        ::unlinkat(dir_fd, tmp_name.c_str(), 0);
        ::close(dir_fd);
        return AtomicWriteFail("atomic write: injected flush failure for " + path.string());
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlinkat(dir_fd, tmp_name.c_str(), 0);
        ::close(dir_fd);
        return AtomicWriteFail("atomic write: failed to flush " + path.string());
    }
    ::close(fd);

    if (AtomicStageMatches(AtomicWriteStage::Rename)) {
        ::unlinkat(dir_fd, tmp_name.c_str(), 0);
        ::close(dir_fd);
        return AtomicWriteFail("atomic write: injected rename failure for " + path.string());
    }
    if (::renameat(dir_fd, tmp_name.c_str(), dir_fd, target_name.c_str()) != 0) {
        ::unlinkat(dir_fd, tmp_name.c_str(), 0);
        ::close(dir_fd);
        return AtomicWriteFail("atomic write: rename failed for " + path.string());
    }

    bool parent_sync_failed = AtomicStageMatches(AtomicWriteStage::ParentSync);
    if (!parent_sync_failed && ::fsync(dir_fd) != 0) parent_sync_failed = true;
    ::close(dir_fd);
    if (parent_sync_failed) {
        return AtomicWriteFail("atomic write: failed to sync parent directory for " +
                               path.string());
    }
    return AtomicWriteOutcome{};
#endif
}

} // anonymous namespace

void SetAtomicWriteFailureInjection(AtomicWriteStage stage) {
    g_atomic_failure_stage.store(static_cast<int>(stage));
}

bool WriteAtomic(const std::filesystem::path& path, const std::string& contents) {
    auto outcome = AtomicWriteBytes(path, contents);
    if (!outcome.ok) {
        MCP_LOG(Error, outcome.error);
        return false;
    }
    return true;
}

bool WriteAtomic(const std::filesystem::path& path, const JsonValue& json) {
    return WriteAtomic(path, json.Dump(2));
}

bool WriteAtomicSecure(const std::filesystem::path& path, const std::string& contents) {
    auto outcome = AtomicWriteBytes(path, contents);
    if (!outcome.ok) {
        throw McpError(McpErrorCode::InternalError, outcome.error);
    }
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
