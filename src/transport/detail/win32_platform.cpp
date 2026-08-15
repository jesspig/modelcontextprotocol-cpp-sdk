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
#include <cwchar>
#include <mutex>

namespace mcp { namespace detail {

namespace {

constexpr DWORD kIoWaitTimeoutMs = 100;

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
        static_cast<int>(s.size()), nullptr, 0);
    if (len == 0)
        throw std::runtime_error("invalid UTF-8 in process string: Windows error " +
            std::to_string(GetLastError()));
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
        static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string QuoteArg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) return arg;
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (char ch : arg) {
        if (ch == '\\') {
            ++backslashes;
        } else if (ch == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            backslashes = 0;
            out += ch;
        }
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

std::string ArgvToCommandLine(const std::string& command, const std::vector<std::string>& args) {
    std::string line = QuoteArg(command);
    for (const auto& arg : args) {
        line += ' ';
        line += QuoteArg(arg);
    }
    return line;
}

constexpr DWORD kPipeBufferSize = 65536;

std::wstring MakePipeName() {
    static std::atomic<unsigned long long> counter{0};
    return L"\\\\.\\pipe\\mcp-stdio-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(counter.fetch_add(1));
}

struct StdioPipe {
    HANDLE parent = nullptr;
    HANDLE child = nullptr;
};

StdioPipe CreateStdioPipe(SECURITY_ATTRIBUTES* sa, bool parent_reads) {
    std::wstring name = MakePipeName();
    DWORD open_mode = PIPE_ACCESS_INBOUND;
    if (parent_reads) open_mode |= FILE_FLAG_OVERLAPPED;
    HANDLE server = CreateNamedPipeW(name.c_str(), open_mode,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, kPipeBufferSize, kPipeBufferSize, 0, sa);
    if (server == INVALID_HANDLE_VALUE)
        throw std::runtime_error("CreateNamedPipeW failed for stdio pipe");
    DWORD client_flags = parent_reads ? 0 : FILE_FLAG_OVERLAPPED;
    HANDLE client = CreateFileW(name.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, sa, OPEN_EXISTING,
        client_flags, nullptr);
    if (client == INVALID_HANDLE_VALUE) {
        CloseHandle(server);
        throw std::runtime_error("CreateFileW failed for stdio pipe");
    }
    if (!ConnectNamedPipe(server, nullptr) &&
        GetLastError() != ERROR_PIPE_CONNECTED) {
        CloseHandle(server);
        CloseHandle(client);
        throw std::runtime_error("ConnectNamedPipe failed for stdio pipe");
    }
    StdioPipe result;
    result.parent = parent_reads ? server : client;
    result.child = parent_reads ? client : server;
    return result;
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
    while (total < size) {
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            if (closed_.load()) return total;
        }
        DWORD written = 0;
        BOOL ok = WriteFile(h, data + total,
            static_cast<DWORD>(size - total), &written, nullptr);
        if (!ok)
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

    StdioPipe stdout_pipe = CreateStdioPipe(&sa, true);
    if (!SetHandleInformation(stdout_pipe.parent, HANDLE_FLAG_INHERIT, 0))
        MCP_LOG(Warning, "SetHandleInformation failed for stdout read pipe");

    StdioPipe stdin_pipe;
    try {
        stdin_pipe = CreateStdioPipe(&sa, false);
    } catch (...) {
        CloseHandle(stdout_pipe.parent);
        CloseHandle(stdout_pipe.child);
        throw;
    }
    if (!SetHandleInformation(stdin_pipe.parent, HANDLE_FLAG_INHERIT, 0))
        MCP_LOG(Warning, "SetHandleInformation failed for stdin write pipe");

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdOutput = stdout_pipe.child;
    si.hStdInput = stdin_pipe.child;
    si.dwFlags |= STARTF_USESTDHANDLES;

    void* env_block = nullptr;
    std::wstring env_block_str;
    if (!info.environment_variables.empty() || !info.inherit_environment) {
        if (info.inherit_environment) {
            LPWCH cur_env = GetEnvironmentStringsW();
            if (cur_env) {
                for (LPWCH item = cur_env; *item != L'\0'; item += wcslen(item) + 1)
                    env_block_str.append(item, wcslen(item) + 1);
                FreeEnvironmentStringsW(cur_env);
            }
        }
        for (const auto& [key, val] : info.environment_variables) {
            env_block_str += Utf8ToWide(key) + L"=" + Utf8ToWide(val) + L'\0';
        }
        size_t tail_zeros = 0;
        while (tail_zeros < 2 && env_block_str.size() > tail_zeros &&
               env_block_str[env_block_str.size() - tail_zeros - 1] == L'\0')
            ++tail_zeros;
        env_block_str.append(2 - tail_zeros, L'\0');
        env_block = static_cast<void*>(env_block_str.data());
    }

    PROCESS_INFORMATION pi = {};
    std::wstring cmd_line_w = Utf8ToWide(cmd_line);
    std::vector<wchar_t> cmd_buf(cmd_line_w.begin(), cmd_line_w.end());
    cmd_buf.push_back(L'\0');

    std::wstring work_dir;
    if (!info.working_directory.empty()) work_dir = Utf8ToWide(info.working_directory);
    LPCWSTR work_dir_ptr = work_dir.empty() ? nullptr : work_dir.c_str();

    BOOL success = CreateProcessW(
        nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, env_block, work_dir_ptr, &si, &pi);

    CloseHandle(stdin_pipe.child);
    CloseHandle(stdout_pipe.child);

    if (!success) {
        CloseHandle(stdout_pipe.parent);
        CloseHandle(stdin_pipe.parent);
        throw std::runtime_error("CreateProcess failed for '" + info.command +
            "': Windows error " + std::to_string(GetLastError()));
    }

    CreatedProcess result;
    result.process = std::make_unique<Win32Process>(pi);
    result.stdin_pipe = std::make_unique<Win32Pipe>(stdin_pipe.parent, true);
    result.stdout_pipe = std::make_unique<Win32Pipe>(stdout_pipe.parent, true);
    return result;
}

std::unique_ptr<PipeHandle> OpenStandardInput() {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {
        if (GetFileType(h) == FILE_TYPE_CHAR) {
            HANDLE ov = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED, nullptr);
            if (ov != INVALID_HANDLE_VALUE)
                return std::make_unique<Win32Pipe>(ov, true);
        }
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
        if (GetFileType(h) == FILE_TYPE_CHAR) {
            HANDLE ov = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED, nullptr);
            if (ov != INVALID_HANDLE_VALUE)
                return std::make_unique<Win32Pipe>(ov, true);
        }
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
