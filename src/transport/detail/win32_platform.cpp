#include <mcp/transport/detail/PlatformIO.hpp>
#include <mcp/McpError.hpp>
#include <mcp/ErrorCodes.hpp>
#include <mcp/Log.hpp>
#include <process/detail/WindowsProcessSupport.hpp>
#include <windows.h>
#include <processthreadsapi.h>
#include <stdexcept>
#include <chrono>
#include <exception>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>

#undef CreateProcess

namespace mcp { namespace detail {

namespace {

constexpr DWORD kIoWaitTimeoutMs = 100;

using process::detail::BuildWindowsCommandLine;
using process::detail::BuildWindowsEnvironmentBlock;
using process::detail::Utf8ToWide;

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
            cancel_event_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
            if (!cancel_event_) {
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
};

class Win32Process : public ProcessHandle {
    PROCESS_INFORMATION pi_{};
public:
    Win32Process(PROCESS_INFORMATION pi) : pi_(pi) {}
    ~Win32Process() override;
    bool IsRunning() override;
    bool Terminate(int timeout_ms) override;
};
} // anonymous namespace

bool WriteWholeMessage(PipeHandle& pipe, const char* data, size_t size) {
    constexpr int kMaxIdleAttempts = 64;
    constexpr std::chrono::milliseconds kIdleDelay(5);
    size_t total = 0;
    int idle_attempts = 0;
    while (total < size) {
        size_t written = 0;
        try {
            written = pipe.Write(data + total, size - total);
        } catch (const std::exception&) {
            return false;
        }
        if (written == 0) {
            if (++idle_attempts > kMaxIdleAttempts) return false;
            std::this_thread::sleep_for(kIdleDelay);
            continue;
        }
        idle_attempts = 0;
        total += written;
    }
    return true;
}

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
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr)
        throw std::runtime_error("CreateEventA failed for pipe read");
    ov.hEvent = event;
    DWORD io_error = ERROR_SUCCESS;
    bool pending = false;
    size_t result = 0;
    HANDLE cancel_evt = nullptr;
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (closed_.load() || eof_) {
            CloseHandle(event);
            return 0;
        }
        cancel_evt = cancel_event_;
        ++io_in_flight_;
        if (!ReadFile(h, buffer, static_cast<DWORD>(size), nullptr, &ov)) {
            io_error = GetLastError();
            if (io_error == ERROR_IO_PENDING || io_error == ERROR_IO_INCOMPLETE) {
                pending = true;
                io_error = ERROR_SUCCESS;
            }
        } else {
            result = static_cast<size_t>(ov.InternalHigh);
            if (result == 0) eof_ = true;
        }
    }
    if (pending) {
        DWORD got = 0;
        HANDLE waits[2] = { event, cancel_evt };
        DWORD wait = WaitForMultipleObjects(2, waits, FALSE, kIoWaitTimeoutMs);
        if (wait == WAIT_OBJECT_0) {
            if (GetOverlappedResult(h, &ov, &got, FALSE)) {
                result = got;
                if (got == 0) eof_ = true;
            } else {
                DWORD err = GetLastError();
                if (err == ERROR_IO_INCOMPLETE) {
                    CancelIoEx(h, &ov);
                    GetOverlappedResult(h, &ov, &got, TRUE);
                } else if (err != ERROR_OPERATION_ABORTED) {
                    io_error = err;
                }
            }
        } else {
            CancelIoEx(h, &ov);
            SetLastError(ERROR_SUCCESS);
            if (GetOverlappedResult(h, &ov, &got, TRUE)) {
                if (GetLastError() != ERROR_OPERATION_ABORTED) {
                    result = got;
                    if (got == 0) eof_ = true;
                }
            } else {
                DWORD err = GetLastError();
                if (err != ERROR_OPERATION_ABORTED) io_error = err;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        --io_in_flight_;
        io_done_cv_.notify_all();
    }
    CloseHandle(event);
    if (result == 0 && (io_error == ERROR_BROKEN_PIPE || io_error == ERROR_HANDLE_EOF))
        eof_ = true;
    if (result == 0 && io_error != ERROR_SUCCESS &&
        io_error != ERROR_BROKEN_PIPE && io_error != ERROR_HANDLE_EOF) {
        eof_ = true;
        throw McpError(McpErrorCode::ConnectionClosed,
                       "pipe read failed: Windows error " + std::to_string(io_error));
    }
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
            if (err == ERROR_BROKEN_PIPE) {
                eof_ = true;
                return 0;
            }
            eof_ = true;
            throw McpError(McpErrorCode::ConnectionClosed,
                           "pipe read failed: Windows error " + std::to_string(err));
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
            if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) {
                eof_ = true;
                return 0;
            }
            eof_ = true;
            throw McpError(McpErrorCode::ConnectionClosed,
                           "pipe read failed: Windows error " + std::to_string(err));
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
        HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr)
            throw std::runtime_error("CreateEventA failed for pipe write");
        ov.hEvent = event;
        DWORD written = 0;
        bool pending = false;
        bool failed = false;
        DWORD io_error = ERROR_SUCCESS;
        HANDLE cancel_evt = nullptr;
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            if (closed_.load()) {
                CloseHandle(event);
                return total;
            }
            cancel_evt = cancel_event_;
            ++io_in_flight_;
            if (!WriteFile(h, data + total, static_cast<DWORD>(size - total), nullptr, &ov)) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING || err == ERROR_IO_INCOMPLETE) pending = true;
                else {
                    failed = true;
                    io_error = err;
                }
            } else {
                written = static_cast<DWORD>(ov.InternalHigh);
            }
        }
        if (pending) {
            HANDLE waits[2] = { event, cancel_evt };
            DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            DWORD got = 0;
            if (wait == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(h, &ov, &got, FALSE)) {
                    DWORD err = GetLastError();
                    if (err == ERROR_IO_INCOMPLETE) {
                        if (GetOverlappedResult(h, &ov, &got, TRUE)) written = got;
                        else {
                            failed = true;
                            io_error = GetLastError();
                        }
                    } else {
                        failed = true;
                        io_error = err;
                    }
                } else {
                    written = got;
                }
            } else {
                CancelIoEx(h, &ov);
                if (GetOverlappedResult(h, &ov, &got, TRUE)) written = got;
                else {
                    failed = true;
                    io_error = GetLastError();
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            --io_in_flight_;
            io_done_cv_.notify_all();
        }
        CloseHandle(event);
        if (failed) {
            if (io_error != ERROR_OPERATION_ABORTED) {
                eof_ = true;
                throw McpError(McpErrorCode::ConnectionClosed,
                               "pipe write failed: Windows error " + std::to_string(io_error));
            }
            return total;
        }
        if (written == 0) return total;
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
        if (!ok) {
            DWORD err = GetLastError();
            eof_ = true;
            throw McpError(McpErrorCode::ConnectionClosed,
                           "pipe write failed: Windows error " + std::to_string(err));
        }
        if (written == 0)
            return total;
        total += written;
    }
    return total;
}

void Win32Pipe::Close() {
    HANDLE h = INVALID_HANDLE_VALUE;
    HANDLE cancel_evt = nullptr;
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (closed_.exchange(true)) return;
        if (cancel_event_) SetEvent(cancel_event_);
        h = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        cancel_evt = cancel_event_;
        cancel_event_ = nullptr;
    }
    if (h != INVALID_HANDLE_VALUE)
        CancelIoEx(h, nullptr);
    {
        std::unique_lock<std::mutex> lock(io_mutex_);
        while (io_in_flight_ > 0) io_done_cv_.wait(lock);
    }
    if (cancel_evt) CloseHandle(cancel_evt);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

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

CreatedProcess CreateProcess(const ProcessStartInfo& info) {
    std::string cmd_line = BuildWindowsCommandLine(info.command, info.arguments);

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
        std::map<std::string, std::string> overrides;
        for (const auto& [key, val] : info.environment_variables)
            overrides[key] = val;
        env_block_str = BuildWindowsEnvironmentBlock(overrides, info.inherit_environment);
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

std::unique_ptr<PipeHandle> CreatePipeFromHandle(HANDLE handle, bool overlapped) {
    return std::make_unique<Win32Pipe>(handle, overlapped);
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
