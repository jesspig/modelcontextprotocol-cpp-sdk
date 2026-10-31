#include <mcp/process/Process.hpp>
#include <mcp/McpError.hpp>
#include <mcp/Log.hpp>
#include <process/detail/WindowsProcessSupport.hpp>

#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

namespace mcp {

namespace {

void ValidateOptions(const SpawnOptions& options) {
    if (options.command.empty())
        throw McpError(McpErrorCode::InvalidParams, "spawn command must not be empty");
    for (const auto& entry : options.environment) {
        if (entry.first.empty() || entry.first.find('=') != std::string::npos)
            throw McpError(McpErrorCode::InvalidParams,
                           "spawn environment key must be non-empty and must not contain '='");
    }
}

HANDLE OpenRedirectFile(const std::string& path) {
    try {
        return process::detail::OpenWindowsRedirectFile(path);
    } catch (const std::runtime_error& error) {
        throw McpError(McpErrorCode::InternalError, error.what());
    }
}

HANDLE AcquireStandardHandle(DWORD which, bool& owned) {
    owned = false;
    HANDLE source = GetStdHandle(which);
    if (source == nullptr || source == INVALID_HANDLE_VALUE) return nullptr;
    HANDLE duplicate = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate,
                        0, TRUE, DUPLICATE_SAME_ACCESS)) {
        owned = true;
        return duplicate;
    }
    return source;
}

class WindowsSpawnedProcess : public SpawnedProcess {
public:
    WindowsSpawnedProcess(HANDLE process, HANDLE job, int64_t pid, bool detached)
        : process_(process), job_(job), pid_(pid), detached_(detached) {}

    ~WindowsSpawnedProcess() override {
        if (!detached_) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!exited_ && process_ != nullptr) {
                process::detail::TerminateWindowsJobOrProcess(job_, process_);
                WaitForSingleObject(process_, INFINITE);
                DWORD code = 0;
                exited_ = true;
                if (GetExitCodeProcess(process_, &code)) exit_code_ = static_cast<int>(code);
            }
        }
        if (job_ != nullptr) CloseHandle(job_);
        if (process_ != nullptr) CloseHandle(process_);
    }

    int64_t Pid() const override { return pid_; }

    bool IsRunning() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (exited_) return false;
        DWORD result = WaitForSingleObject(process_, 0);
        if (result == WAIT_TIMEOUT) return true;
        if (result == WAIT_OBJECT_0) {
            RecordExitLocked();
            return false;
        }
        throw McpError(McpErrorCode::InternalError,
                       "WaitForSingleObject failed for spawned process: Windows error " +
                           std::to_string(GetLastError()));
    }

    bool Wait(int timeout_ms) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (exited_) return true;
        }
        DWORD timeout = timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms);
        DWORD result = WaitForSingleObject(process_, timeout);
        if (result == WAIT_OBJECT_0) {
            std::lock_guard<std::mutex> lock(mutex_);
            RecordExitLocked();
            return true;
        }
        if (result == WAIT_TIMEOUT) {
            std::lock_guard<std::mutex> lock(mutex_);
            return exited_;
        }
        throw McpError(McpErrorCode::InternalError,
                       "WaitForSingleObject failed for spawned process: Windows error " +
                           std::to_string(GetLastError()));
    }

    std::optional<int> ExitCode() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!exited_) {
            DWORD result = WaitForSingleObject(process_, 0);
            if (result == WAIT_OBJECT_0) RecordExitLocked();
            else if (result != WAIT_TIMEOUT)
                throw McpError(McpErrorCode::InternalError,
                               "WaitForSingleObject failed for spawned process: Windows error " +
                                   std::to_string(GetLastError()));
        }
        return exit_code_;
    }

    bool Terminate(int grace_ms) override {
        if (!IsRunning()) return true;
        if (grace_ms > 0) {
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, static_cast<DWORD>(pid_));
            if (Wait(grace_ms)) {
                if (job_ != nullptr) TerminateJobObject(job_, 1);
                return true;
            }
        }
        if (!IsRunning()) return true;
        process::detail::TerminateWindowsJobOrProcess(job_, process_);
        Wait(-1);
        return true;
    }

private:
    void RecordExitLocked() const {
        DWORD code = 0;
        if (!GetExitCodeProcess(process_, &code))
            throw McpError(McpErrorCode::InternalError,
                           "GetExitCodeProcess failed for spawned process: Windows error " +
                               std::to_string(GetLastError()));
        exited_ = true;
        exit_code_ = static_cast<int>(code);
    }

    mutable std::mutex mutex_;
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    int64_t pid_ = 0;
    bool detached_ = false;
    mutable bool exited_ = false;
    mutable std::optional<int> exit_code_;
};

} // namespace

std::unique_ptr<SpawnedProcess> Spawn(const SpawnOptions& options) {
    ValidateOptions(options);

    std::wstring command_line = process::detail::Utf8ToWide(
        process::detail::BuildWindowsCommandLine(options.command, options.arguments));
    std::vector<wchar_t> command_buffer(command_line.begin(), command_line.end());
    command_buffer.push_back(L'\0');

    std::wstring working_directory;
    if (!options.working_directory.empty())
        working_directory = process::detail::Utf8ToWide(options.working_directory);

    std::wstring environment_block = process::detail::BuildWindowsEnvironmentBlock(
        options.environment, options.inherit_environment);
    void* environment = environment_block.empty()
        ? nullptr
        : static_cast<void*>(environment_block.data());

    bool stdin_owned = false;
    bool stdout_owned = false;
    bool stderr_owned = false;
    HANDLE stdin_handle = nullptr;
    HANDLE stdout_handle = nullptr;
    HANDLE stderr_handle = nullptr;
    try {
        stdin_handle = AcquireStandardHandle(STD_INPUT_HANDLE, stdin_owned);
        if (options.stdout_path.empty()) {
            stdout_handle = AcquireStandardHandle(STD_OUTPUT_HANDLE, stdout_owned);
        } else {
            stdout_handle = OpenRedirectFile(options.stdout_path);
            stdout_owned = true;
        }
        if (options.stderr_path.empty()) {
            stderr_handle = AcquireStandardHandle(STD_ERROR_HANDLE, stderr_owned);
        } else {
            stderr_handle = OpenRedirectFile(options.stderr_path);
            stderr_owned = true;
        }
    } catch (...) {
        if (stdin_owned && stdin_handle != nullptr) CloseHandle(stdin_handle);
        if (stdout_owned && stdout_handle != nullptr) CloseHandle(stdout_handle);
        if (stderr_owned && stderr_handle != nullptr) CloseHandle(stderr_handle);
        throw;
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_handle;
    startup.hStdOutput = stdout_handle;
    startup.hStdError = stderr_handle;

    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP;
    if (options.detached) flags |= DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB;

    PROCESS_INFORMATION process_info = {};
    BOOL created = CreateProcessW(nullptr, command_buffer.data(), nullptr, nullptr, TRUE,
        flags, environment, working_directory.empty() ? nullptr : working_directory.c_str(),
        &startup, &process_info);
    DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    if (!created && options.detached && create_error == ERROR_ACCESS_DENIED) {
        flags &= ~CREATE_BREAKAWAY_FROM_JOB;
        created = CreateProcessW(nullptr, command_buffer.data(), nullptr, nullptr, TRUE,
            flags, environment, working_directory.empty() ? nullptr : working_directory.c_str(),
            &startup, &process_info);
        create_error = created ? ERROR_SUCCESS : GetLastError();
    }

    if (stdin_owned && stdin_handle != nullptr) CloseHandle(stdin_handle);
    if (stdout_owned && stdout_handle != nullptr) CloseHandle(stdout_handle);
    if (stderr_owned && stderr_handle != nullptr) CloseHandle(stderr_handle);

    if (!created)
        throw McpError(McpErrorCode::InternalError,
                       "CreateProcessW failed for '" + options.command + "': Windows error " +
                           std::to_string(create_error));

    HANDLE job = process::detail::CreateWindowsJobObject(!options.detached, true);
    if (job == nullptr) {
        MCP_LOG(Warning, "failed to create job object for spawned process: Windows error " +
            std::to_string(GetLastError()));
    } else if (!process::detail::AssignWindowsProcessToJob(job, process_info.hProcess)) {
        MCP_LOG(Warning, "AssignProcessToJobObject failed for spawned process: Windows error " +
            std::to_string(GetLastError()));
        CloseHandle(job);
        job = nullptr;
    }

    if (ResumeThread(process_info.hThread) == static_cast<DWORD>(-1)) {
        DWORD resume_error = GetLastError();
        TerminateProcess(process_info.hProcess, 1);
        WaitForSingleObject(process_info.hProcess, INFINITE);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        if (job != nullptr) CloseHandle(job);
        throw McpError(McpErrorCode::InternalError,
                       "ResumeThread failed for spawned process: Windows error " +
                           std::to_string(resume_error));
    }
    CloseHandle(process_info.hThread);

    return std::make_unique<WindowsSpawnedProcess>(process_info.hProcess, job,
        static_cast<int64_t>(process_info.dwProcessId), options.detached);
}

} // namespace mcp
