// win32_platform.cpp — Win32 process and pipe implementations

#include <mcp/transport/detail/PlatformIO.hpp>
#include <mcp/Log.hpp>
#include <windows.h>
#include <processthreadsapi.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace mcp { namespace detail {

namespace {

std::string ArgvToCommandLine(const std::string& command, const std::vector<std::string>& args) {
    std::string line;
    bool first = true;
    auto append = [&line, &first](const std::string& arg) {
        if (!first) line += ' ';
        first = false;
        if (arg.find_first_of(" \t\"") == std::string::npos) {
            line += arg;
            return;
        }
        line += '"';
        for (char ch : arg) {
            if (ch == '"') line += '\\';
            line += ch;
        }
        line += '"';
    };
    append(command);
    for (const auto& arg : args) append(arg);
    return line;
}

class Win32Pipe : public PipeHandle {
    HANDLE handle_ = INVALID_HANDLE_VALUE;
public:
    Win32Pipe(HANDLE h) : handle_(h) {}
    ~Win32Pipe() override { Close(); }
    size_t Read(char* buffer, size_t size) override;
    size_t Write(const char* data, size_t size) override;
    void Close() override;
    uintptr_t native_handle() const override { return (uintptr_t)handle_; }
};

class Win32Process : public ProcessHandle {
    PROCESS_INFORMATION pi_{};
public:
    Win32Process(PROCESS_INFORMATION pi) : pi_(pi) {}
    ~Win32Process() override;
    bool IsRunning() override;
    bool Terminate(int timeout_ms) override;
    int WaitForExit(int timeout_ms) override;
    uintptr_t native_handle() const override { return (uintptr_t)pi_.hProcess; }
};
} // anonymous namespace

// Pipe implementations
size_t Win32Pipe::Read(char* buffer, size_t size) {
    DWORD read = 0;
    if (!ReadFile(handle_, buffer, static_cast<DWORD>(size), &read, nullptr))
        return 0;
    return read;
}

size_t Win32Pipe::Write(const char* data, size_t size) {
    size_t total = 0;
    while (total < size) {
        DWORD written = 0;
        if (!WriteFile(handle_, data + total, static_cast<DWORD>(size - total), &written, nullptr))
            return total;
        if (written == 0)
            return total;
        total += written;
    }
    return total;
}

void Win32Pipe::Close() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
}

// Process implementations
Win32Process::~Win32Process() {
    if (pi_.hProcess) {
        CloseHandle(pi_.hProcess);
        pi_.hProcess = nullptr;
    }
    if (pi_.hThread) {
        CloseHandle(pi_.hThread);
        pi_.hThread = nullptr;
    }
}

bool Win32Process::IsRunning() {
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(pi_.hProcess, &exit_code))
        return false;
    return exit_code == STILL_ACTIVE;
}

bool Win32Process::Terminate(int timeout_ms) {
    if (!TerminateProcess(pi_.hProcess, 1))
        return false;
    return WaitForSingleObject(pi_.hProcess, timeout_ms) == WAIT_OBJECT_0;
}

int Win32Process::WaitForExit(int timeout_ms) {
    DWORD result = WaitForSingleObject(pi_.hProcess, timeout_ms);
    if (result == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        GetExitCodeProcess(pi_.hProcess, &exit_code);
        return exit_code;
    }
    return -1;
}

// Factory implementation
CreatedProcess CreateProcess(const ProcessStartInfo& info) {
    std::string cmd_line = ArgvToCommandLine(info.command, info.arguments);

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE stdout_read = nullptr, stdout_write = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0))
        throw std::runtime_error("CreatePipe failed for stdout");
    if (!SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0))
        MCP_LOG(Warning, "SetHandleInformation failed for stdout read pipe");

    HANDLE stdin_read = nullptr, stdin_write = nullptr;
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        throw std::runtime_error("CreatePipe failed for stdin");
    }
    if (!SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0))
        MCP_LOG(Warning, "SetHandleInformation failed for stdin write pipe");

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdOutput = stdout_write;
    si.hStdInput = stdin_read;
    si.dwFlags |= STARTF_USESTDHANDLES;

    // Build environment block
    void* env_block = nullptr;
    std::string env_block_str;
    if (!info.environment_variables.empty() || !info.inherit_environment) {
        if (info.inherit_environment) {
            LPCH cur_env = GetEnvironmentStrings();
            if (cur_env) {
                LPCH end = cur_env;
                while (*end || *(end + 1)) ++end;
                env_block_str = std::string(cur_env, end - cur_env + 2);
                FreeEnvironmentStrings(cur_env);
            }
        }
        for (const auto& [key, val] : info.environment_variables) {
            env_block_str += key + "=" + val + '\0';
        }
        env_block_str += '\0';
        env_block = static_cast<void*>(env_block_str.data());
    }

    PROCESS_INFORMATION pi = {};
    std::vector<char> cmd_buf(cmd_line.begin(), cmd_line.end());
    cmd_buf.push_back('\0');

    LPCSTR work_dir = info.working_directory.empty() ? nullptr : info.working_directory.c_str();

    BOOL success = CreateProcessA(
        nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, env_block, work_dir, &si, &pi);

    CloseHandle(stdin_read);
    CloseHandle(stdout_write);

    if (!success) {
        CloseHandle(stdout_read);
        CloseHandle(stdin_write);
        throw std::runtime_error("CreateProcess failed for '" + info.command +
            "': Windows error " + std::to_string(GetLastError()));
    }

    CreatedProcess result;
    result.process = std::make_unique<Win32Process>(pi);
    result.stdin_pipe = std::make_unique<Win32Pipe>(stdin_write);
    result.stdout_pipe = std::make_unique<Win32Pipe>(stdout_read);
    return result;
}

std::unique_ptr<PipeHandle> OpenStandardInput() {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    return std::make_unique<Win32Pipe>(h);
}

std::unique_ptr<PipeHandle> OpenStandardOutput() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    return std::make_unique<Win32Pipe>(h);
}

std::unique_ptr<PipeHandle> OpenStandardError() {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    return std::make_unique<Win32Pipe>(h);
}

void SetThreadName(const char* name) {
    typedef HRESULT(WINAPI* SetThreadDescriptionFunc)(HANDLE, PCWSTR);
    auto set_thread_desc = (SetThreadDescriptionFunc)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
    if (!set_thread_desc) {
        MCP_LOG(Debug, "SetThreadDescription not available; thread name not set");
        return;
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    std::wstring wname(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name, -1, &wname[0], len);
    set_thread_desc(GetCurrentThread(), wname.c_str());
}

}} // namespace mcp::detail
