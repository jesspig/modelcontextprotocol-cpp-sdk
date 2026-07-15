#include <mcp/transport/detail/PlatformIO.hpp>
#include <windows.h>
#include <processthreadsapi.h>
#include <string>
#include <vector>

namespace mcp { namespace detail {

namespace {
class Win32Pipe : public PipeHandle {
    HANDLE handle_ = INVALID_HANDLE_VALUE;
public:
    Win32Pipe(HANDLE h) : handle_(h) {}
    ~Win32Pipe() override { Close(); }
    size_t Read(char* buffer, size_t size) override;
    size_t Write(const char* data, size_t size) override;
    void Close() override;
    uintptr_t native_handle() const override { return (uintptr_t)handle_; }
    bool WaitForData(int timeout_ms) override {
        return WaitForSingleObject(handle_, timeout_ms) == WAIT_OBJECT_0;
    }
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
    DWORD written = 0;
    WriteFile(handle_, data, static_cast<DWORD>(size), &written, nullptr);
    return written;
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
    // Build command line
    std::string cmd_line = "cmd.exe /c \"" + info.command;
    for (const auto& arg : info.arguments) {
        cmd_line += " " + arg;
    }
    cmd_line += "\"";

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE stdout_read = nullptr, stdout_write = nullptr;
    CreatePipe(&stdout_read, &stdout_write, &sa, 0);
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    HANDLE stdin_read = nullptr, stdin_write = nullptr;
    CreatePipe(&stdin_read, &stdin_write, &sa, 0);
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdOutput = stdout_write;
    si.hStdInput = stdin_read;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};
    std::vector<char> cmd_buf(cmd_line.begin(), cmd_line.end());
    cmd_buf.push_back('\0');

    LPCSTR work_dir = info.working_directory.empty() ? nullptr : info.working_directory.c_str();

    CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                   CREATE_NO_WINDOW, nullptr, work_dir, &si, &pi);

    CloseHandle(stdin_read);
    CloseHandle(stdout_write);

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
    if (set_thread_desc) {
        int len = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
        std::wstring wname(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, name, -1, &wname[0], len);
        set_thread_desc(GetCurrentThread(), wname.c_str());
    }
}

}} // namespace mcp::detail
