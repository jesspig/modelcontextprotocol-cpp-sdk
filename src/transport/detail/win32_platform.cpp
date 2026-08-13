// win32_platform.cpp — Win32 process and pipe implementations

#include <mcp/transport/detail/PlatformIO.hpp>
#include <mcp/Log.hpp>
#include <windows.h>
#include <processthreadsapi.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace mcp { namespace detail {

namespace {

constexpr DWORD kIoWaitTimeoutMs = 100;

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
    HANDLE read_event_ = nullptr;
    HANDLE write_event_ = nullptr;
    HANDLE cancel_event_ = nullptr;
    std::mutex io_mutex_;
    std::condition_variable io_done_cv_;
    size_t io_in_flight_ = 0;
    std::atomic<bool> closed_{false};
    bool eof_ = false;
    bool overlapped_;

    size_t ReadOverlapped(HANDLE h, char* buffer, size_t size);
    size_t ReadSync(HANDLE h, char* buffer, size_t size);
    size_t WriteOverlapped(HANDLE h, const char* data, size_t size);
    size_t WriteSync(HANDLE h, const char* data, size_t size);
public:
    Win32Pipe(HANDLE h, bool overlapped) : handle_(h ? h : INVALID_HANDLE_VALUE), overlapped_(overlapped) {
        if (handle_ != INVALID_HANDLE_VALUE) {
            read_event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            write_event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            cancel_event_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
            if (!read_event_ || !write_event_ || !cancel_event_) {
                if (read_event_) { CloseHandle(read_event_); read_event_ = nullptr; }
                if (write_event_) { CloseHandle(write_event_); write_event_ = nullptr; }
                if (cancel_event_) { CloseHandle(cancel_event_); cancel_event_ = nullptr; }
                overlapped_ = false;
            }
        } else {
            overlapped_ = false;
        }
    }
    ~Win32Pipe() override { Close(); }
    size_t Read(char* buffer, size_t size) override;
    size_t Write(const char* data, size_t size) override;
    void Close() override;
    bool IsEof() const override { return closed_.load() || eof_; }
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
    HANDLE h;
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (closed_.load() || handle_ == INVALID_HANDLE_VALUE || eof_) return 0;
        h = handle_;
    }
    if (overlapped_) return ReadOverlapped(h, buffer, size);
    return ReadSync(h, buffer, size);
}

size_t Win32Pipe::ReadOverlapped(HANDLE h, char* buffer, size_t size) {
    OVERLAPPED ov = {};
    ov.hEvent = read_event_;
    DWORD io_error = ERROR_SUCCESS;
    bool pending = false;
    size_t result = 0;
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (closed_.load() || eof_) return 0;
        ++io_in_flight_;
        ResetEvent(read_event_);
        if (!ReadFile(h, buffer, static_cast<DWORD>(size), nullptr, &ov)) {
            io_error = GetLastError();
            if (io_error == ERROR_IO_PENDING) pending = true;
        } else {
            result = static_cast<size_t>(ov.InternalHigh);
            if (result == 0) eof_ = true;
        }
    }
    if (pending) {
        HANDLE waits[2] = { read_event_, cancel_event_ };
        DWORD wait = WaitForMultipleObjects(2, waits, FALSE, kIoWaitTimeoutMs);
        DWORD got = 0;
        if (wait == WAIT_OBJECT_0) {
            if (GetOverlappedResult(h, &ov, &got, FALSE)) {
                result = got;
                if (got == 0) eof_ = true;
            } else {
                io_error = GetLastError();
            }
        } else {
            CancelIoEx(h, &ov);
            if (GetOverlappedResult(h, &ov, &got, TRUE)) {
                result = got;
                if (got == 0) eof_ = true;
            } else {
                io_error = GetLastError();
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        --io_in_flight_;
        io_done_cv_.notify_all();
    }
    if (result == 0 && (io_error == ERROR_BROKEN_PIPE || io_error == ERROR_HANDLE_EOF))
        eof_ = true;
    return result;
}

size_t Win32Pipe::ReadSync(HANDLE h, char* buffer, size_t size) {
    DWORD available = 0;
    DWORD ftype;
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (closed_.load()) return 0;
        ftype = GetFileType(h);
        if (ftype == FILE_TYPE_PIPE &&
            !PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) eof_ = true;
            return 0;
        }
    }
    if (ftype == FILE_TYPE_PIPE && available == 0) {
        if (cancel_event_) WaitForSingleObject(cancel_event_, kIoWaitTimeoutMs);
        else Sleep(kIoWaitTimeoutMs);
        return 0;
    }
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (closed_.load()) return 0;
        DWORD got = 0;
        if (!ReadFile(h, buffer, static_cast<DWORD>(size), &got, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) eof_ = true;
            return 0;
        }
        if (got == 0) eof_ = true;
        return got;
    }
}

size_t Win32Pipe::Write(const char* data, size_t size) {
    HANDLE h;
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (closed_.load() || handle_ == INVALID_HANDLE_VALUE) return 0;
        h = handle_;
    }
    if (overlapped_) return WriteOverlapped(h, data, size);
    return WriteSync(h, data, size);
}

size_t Win32Pipe::WriteOverlapped(HANDLE h, const char* data, size_t size) {
    size_t total = 0;
    while (total < size) {
        OVERLAPPED ov = {};
        ov.hEvent = write_event_;
        DWORD written = 0;
        bool pending = false;
        bool failed = false;
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            if (closed_.load()) return total;
            ++io_in_flight_;
            ResetEvent(write_event_);
            if (!WriteFile(h, data + total, static_cast<DWORD>(size - total), nullptr, &ov)) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) pending = true;
                else failed = true;
            } else {
                written = static_cast<DWORD>(ov.InternalHigh);
            }
        }
        if (pending) {
            HANDLE waits[2] = { write_event_, cancel_event_ };
            DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            DWORD got = 0;
            if (wait == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(h, &ov, &got, FALSE)) failed = true;
                else written = got;
            } else {
                CancelIoEx(h, &ov);
                GetOverlappedResult(h, &ov, &got, TRUE);
                written = got;
            }
        }
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            --io_in_flight_;
            io_done_cv_.notify_all();
        }
        if (failed || written == 0) return total;
        total += written;
    }
    return total;
}

size_t Win32Pipe::WriteSync(HANDLE h, const char* data, size_t size) {
    size_t total = 0;
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (closed_.load()) return 0;
    while (total < size) {
        DWORD written = 0;
        if (!WriteFile(h, data + total, static_cast<DWORD>(size - total), &written, nullptr))
            return total;
        if (written == 0)
            return total;
        total += written;
    }
    return total;
}

void Win32Pipe::Close() {
    HANDLE h = INVALID_HANDLE_VALUE;
    HANDLE read_evt = nullptr, write_evt = nullptr, cancel_evt = nullptr;
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (closed_.exchange(true)) return;
        if (cancel_event_) SetEvent(cancel_event_);
        h = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        read_evt = read_event_;
        read_event_ = nullptr;
        write_evt = write_event_;
        write_event_ = nullptr;
        cancel_evt = cancel_event_;
        cancel_event_ = nullptr;
    }
    if (h != INVALID_HANDLE_VALUE)
        CancelIoEx(h, nullptr);
    {
        std::unique_lock<std::mutex> lock(io_mutex_);
        while (io_in_flight_ > 0) io_done_cv_.wait(lock);
    }
    if (read_evt) CloseHandle(read_evt);
    if (write_evt) CloseHandle(write_evt);
    if (cancel_evt) CloseHandle(cancel_evt);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
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
    result.stdin_pipe = std::make_unique<Win32Pipe>(stdin_write, false);
    result.stdout_pipe = std::make_unique<Win32Pipe>(stdout_read, false);
    return result;
}

std::unique_ptr<PipeHandle> OpenStandardInput() {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {
        HANDLE ov = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr);
        if (ov != INVALID_HANDLE_VALUE)
            return std::make_unique<Win32Pipe>(ov, true);
        HANDLE dup = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup,
                            0, FALSE, DUPLICATE_SAME_ACCESS))
            return std::make_unique<Win32Pipe>(dup, false);
    }
    return std::make_unique<Win32Pipe>(INVALID_HANDLE_VALUE, false);
}

std::unique_ptr<PipeHandle> OpenStandardOutput() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {
        HANDLE ov = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr);
        if (ov != INVALID_HANDLE_VALUE)
            return std::make_unique<Win32Pipe>(ov, true);
        HANDLE dup = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup,
                            0, FALSE, DUPLICATE_SAME_ACCESS))
            return std::make_unique<Win32Pipe>(dup, false);
    }
    return std::make_unique<Win32Pipe>(INVALID_HANDLE_VALUE, false);
}

std::unique_ptr<PipeHandle> OpenStandardError() {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    return std::make_unique<Win32Pipe>(h, false);
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
