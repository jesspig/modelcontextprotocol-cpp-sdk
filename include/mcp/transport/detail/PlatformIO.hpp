#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace mcp { namespace detail {

struct ProcessHandle {
    virtual ~ProcessHandle() = default;
    virtual bool IsRunning() = 0;
    virtual bool Terminate(int timeout_ms) = 0;
    virtual int64_t Pid() const = 0;
};

struct PipeHandle {
    virtual ~PipeHandle() = default;
    virtual size_t Read(char* buffer, size_t size) = 0;
    virtual size_t Write(const char* data, size_t size) = 0;
    virtual void Close() = 0;
    virtual bool IsEof() const { return true; }
};

struct ProcessStartInfo {
    std::string command;
    std::vector<std::string> arguments;
    std::string working_directory;
    bool inherit_environment = true;
    std::vector<std::pair<std::string, std::string>> environment_variables;
    std::string stderr_path;
    bool capture_stderr = false;
    bool terminate_process_tree = false;
};

struct CreatedProcess {
    std::unique_ptr<ProcessHandle> process;
    std::unique_ptr<PipeHandle> stdin_pipe;
    std::unique_ptr<PipeHandle> stdout_pipe;
    std::unique_ptr<PipeHandle> stderr_pipe;
};

CreatedProcess CreateProcess(const ProcessStartInfo& info);

std::unique_ptr<PipeHandle> OpenStandardInput();
std::unique_ptr<PipeHandle> OpenStandardOutput();
bool WriteWholeMessage(PipeHandle& pipe, const char* data, std::size_t size);
void SetThreadName(const char* name);

}} // namespace mcp::detail
