#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace mcp { namespace detail {

// ── Process handle abstraction ──
struct ProcessHandle {
    virtual ~ProcessHandle() = default;
    virtual bool IsRunning() = 0;
    virtual bool Terminate(int timeout_ms) = 0;
    virtual int WaitForExit(int timeout_ms) = 0;
};

// ── Pipe handle abstraction ──
struct PipeHandle {
    virtual ~PipeHandle() = default;
    virtual size_t Read(char* buffer, size_t size) = 0;
    virtual size_t Write(const char* data, size_t size) = 0;
    virtual void Close() = 0;
};

// ── Process startup info ──
struct ProcessStartInfo {
    std::string command;
    std::vector<std::string> arguments;
    std::string working_directory;
    bool inherit_environment = true;
    std::vector<std::pair<std::string, std::string>> environment_variables;
};

// ── Created process with pipes ──
struct CreatedProcess {
    std::unique_ptr<ProcessHandle> process;
    std::unique_ptr<PipeHandle> stdin_pipe;   // 父进程写入
    std::unique_ptr<PipeHandle> stdout_pipe;  // 父进程读取
    std::unique_ptr<PipeHandle> stderr_pipe;  // 父进程读取
};

// ── Platform-specific factory ──
CreatedProcess CreateProcess(const ProcessStartInfo& info);

}} // namespace mcp::detail
